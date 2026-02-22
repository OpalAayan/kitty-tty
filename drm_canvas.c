/*
 * drm_canvas.c — Phase 2+3: DRM Canvas with FreeType2 Text Rendering
 *
 * Takes over the screen via Linux DRM/KMS, dynamically detects the
 * monitor's native resolution, allocates a dumb framebuffer, paints
 * it a solid color, renders text using FreeType2, then cleanly
 * restores the original display state after 3 seconds.
 *
 * Compile:
 *   gcc -Wall -Wextra -o drm_canvas drm_canvas.c \
 *       $(pkg-config --cflags --libs freetype2 libdrm)
 *
 * Run (requires root or video group membership):
 *   sudo ./drm_canvas
 */

#include <drm.h> /* struct drm_mode_create_dumb */
#include <drm_mode.h>
#include <fcntl.h>       /* open(), O_RDWR            */
#include <stdint.h>      /* uint32_t, uint8_t         */
#include <stdio.h>       /* perror(), fprintf()       */
#include <stdlib.h>      /* exit(), EXIT_*            */
#include <string.h>      /* memset(), strlen()        */
#include <sys/ioctl.h>   /* ioctl()                   */
#include <sys/mman.h>    /* mmap(), munmap()          */
#include <unistd.h>      /* close(), sleep()          */
#include <xf86drm.h>     /* drmIoctl, DRM ioctls      */
#include <xf86drmMode.h> /* drmModeGetResources, etc. */

/* FreeType2 headers */
#include <ft2build.h>
#include FT_FREETYPE_H

/* ── Configuration ──────────────────────────────────────────────── */

/* Font path — change this to any monospace TTF on your system      */
#define FONT_PATH "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf"
#define FONT_SIZE 32 /* pixel height for glyphs            */

/* Colors (XRGB8888: 0x00RRGGBB)                                    */
#define BG_COLOR 0x002E3440 /* Nord Polar Night dark blue-gray    */
#define FG_COLOR 0x00ECEFF4 /* Nord Snow Storm white              */

/* The text we'll paint onto the framebuffer                        */
#define DEMO_TEXT "kitty-tty pure C"

/* ── Helpers ────────────────────────────────────────────────────── */

static void die(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

/*
 * draw_bitmap — blit a single FreeType glyph bitmap onto the DRM fb.
 *
 * @bmp:    pointer to the FreeType FT_Bitmap (8-bit grayscale)
 * @fb:     pointer to the mmap'd DRM framebuffer
 * @stride: bytes per row in the DRM buffer (may include padding)
 * @scr_w:  screen width in pixels  (bounds check)
 * @scr_h:  screen height in pixels (bounds check)
 * @pen_x:  top-left X on screen where the glyph starts
 * @pen_y:  top-left Y on screen where the glyph starts
 *
 * The DRM buffer is XRGB8888.  FreeType gives us an 8-bit coverage
 * map (0 = transparent, 255 = fully opaque).  We do a simple alpha
 * blend between BG_COLOR and FG_COLOR based on that coverage value.
 */
static void draw_bitmap(const FT_Bitmap *bmp, uint8_t *fb, uint32_t stride,
                        uint32_t scr_w, uint32_t scr_h, int pen_x, int pen_y) {
  /* Decompose foreground and background into RGB channels once */
  uint32_t fg_r = (FG_COLOR >> 16) & 0xFF;
  uint32_t fg_g = (FG_COLOR >> 8) & 0xFF;
  uint32_t fg_b = (FG_COLOR) & 0xFF;

  uint32_t bg_r = (BG_COLOR >> 16) & 0xFF;
  uint32_t bg_g = (BG_COLOR >> 8) & 0xFF;
  uint32_t bg_b = (BG_COLOR) & 0xFF;

  for (unsigned int row = 0; row < bmp->rows; row++) {
    for (unsigned int col = 0; col < bmp->width; col++) {

      int sx = pen_x + (int)col; /* screen X */
      int sy = pen_y + (int)row; /* screen Y */

      /* Clip to screen bounds */
      if (sx < 0 || sy < 0 || (uint32_t)sx >= scr_w || (uint32_t)sy >= scr_h)
        continue;

      /* FreeType coverage: 0..255 */
      uint8_t alpha = bmp->buffer[row * (unsigned int)bmp->pitch + col];
      if (alpha == 0)
        continue; /* fully transparent — skip */

      /* Alpha blend: out = fg * a + bg * (1 - a)  */
      uint32_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
      uint32_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
      uint32_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

      /* Write the blended pixel into the framebuffer */
      uint32_t *pixel =
          (uint32_t *)(fb + (uint32_t)sy * stride + (uint32_t)sx * 4);
      *pixel = (r << 16) | (g << 8) | b;
    }
  }
}

/*
 * draw_text — render a null-terminated string onto the DRM fb.
 *
 * @face:   initialized FreeType face (font already loaded + sized)
 * @text:   the string to render
 * @fb:     mmap'd DRM framebuffer
 * @stride: DRM buffer stride (bytes per row)
 * @scr_w:  screen width
 * @scr_h:  screen height
 * @start_x: starting X position (pixels)
 * @start_y: Y baseline position (pixels)
 */
static void draw_text(FT_Face face, const char *text, uint8_t *fb,
                      uint32_t stride, uint32_t scr_w, uint32_t scr_h,
                      int start_x, int start_y) {
  int pen_x = start_x;

  for (size_t i = 0; text[i] != '\0'; i++) {
    /* Load the glyph and render it to an 8-bit bitmap */
    if (FT_Load_Char(face, (FT_ULong)text[i], FT_LOAD_RENDER)) {
      fprintf(stderr, "[drm_canvas] FT_Load_Char failed for '%c'\n", text[i]);
      continue;
    }

    FT_GlyphSlot g = face->glyph;

    /*
     * bitmap_left / bitmap_top give the glyph's offset from the
     * pen position.  bitmap_top is measured upward from the
     * baseline, so we subtract it from our Y coordinate.
     */
    int glyph_x = pen_x + g->bitmap_left;
    int glyph_y = start_y - g->bitmap_top;

    draw_bitmap(&g->bitmap, fb, stride, scr_w, scr_h, glyph_x, glyph_y);

    /* Advance the pen to the next character position.
     * advance.x is in 1/64th of a pixel (26.6 fixed point). */
    pen_x += (int)(g->advance.x >> 6);
  }
}

/*
 * measure_text_width — compute the total pixel width of a string
 *                      so we can center it on screen.
 */
static int measure_text_width(FT_Face face, const char *text) {
  int width = 0;

  for (size_t i = 0; text[i] != '\0'; i++) {
    if (FT_Load_Char(face, (FT_ULong)text[i], FT_LOAD_DEFAULT))
      continue;
    width += (int)(face->glyph->advance.x >> 6);
  }

  return width;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
  int drm_fd = -1;

  /* ── Step 1: Dynamically find a usable DRM device node ───────── */
  drmModeRes *resources = NULL;

  for (int card = 0; card < 64; card++) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/dri/card%d", card);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;

    drmModeRes *r = drmModeGetResources(fd);
    if (!r) {
      close(fd);
      continue;
    }

    if (r->count_connectors > 0 && r->count_crtcs > 0) {
      drm_fd = fd;
      resources = r;
      fprintf(stderr, "[drm_canvas] Found KMS device: %s\n", path);
      break;
    }

    drmModeFreeResources(r);
    close(fd);
  }

  if (drm_fd < 0) {
    fprintf(stderr, "FATAL: Could not find any /dev/dri/cardX with KMS.\n");
    return EXIT_FAILURE;
  }

  /* ── Step 3: Find the first connected monitor ──────────────── */
  drmModeConnector *connector = NULL;

  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(drm_fd, resources->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      connector = c;
      break;
    }
    drmModeFreeConnector(c);
  }

  if (!connector) {
    fprintf(stderr, "No connected monitor found.\n");
    drmModeFreeResources(resources);
    close(drm_fd);
    return EXIT_FAILURE;
  }

  /* Extract the native (preferred) mode — modes[0] is the best  */
  drmModeModeInfo mode = connector->modes[0];
  uint32_t width = mode.hdisplay;
  uint32_t height = mode.vdisplay;

  fprintf(stderr, "[drm_canvas] Detected resolution: %ux%u\n", width, height);

  /* ── Find the CRTC tied to this connector ──────────────────── */
  drmModeEncoder *encoder = NULL;
  uint32_t crtc_id = 0;

  if (connector->encoder_id) {
    encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
    if (encoder) {
      crtc_id = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
    }
  }

  /* Fallback: pick the first CRTC */
  if (!crtc_id) {
    for (int i = 0; i < resources->count_crtcs; i++) {
      crtc_id = resources->crtcs[i];
      break;
    }
  }

  if (!crtc_id) {
    fprintf(stderr, "Could not find a CRTC for the connector.\n");
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    close(drm_fd);
    return EXIT_FAILURE;
  }

  /* Save original CRTC state for restoration */
  drmModeCrtc *orig_crtc = drmModeGetCrtc(drm_fd, crtc_id);

  /* ── Step 4: Create a Dumb Buffer ──────────────────────────── */
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = width;
  create_req.height = height;
  create_req.bpp = 32; /* XRGB8888 */

  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
    die("DRM_IOCTL_MODE_CREATE_DUMB");

  uint32_t stride = create_req.pitch;
  uint32_t size = create_req.size;
  uint32_t handle = create_req.handle;

  fprintf(stderr, "[drm_canvas] Dumb buffer: stride=%u, size=%u\n", stride,
          size);

  /* ── Create a framebuffer object ───────────────────────────── */
  uint32_t fb_id = 0;
  if (drmModeAddFB(drm_fd, width, height, 24, 32, stride, handle, &fb_id) < 0)
    die("drmModeAddFB");

  /* ── Step 5: mmap the dumb buffer into user space ──────────── */
  struct drm_mode_map_dumb map_req = {0};
  map_req.handle = handle;

  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0)
    die("DRM_IOCTL_MODE_MAP_DUMB");

  uint8_t *framebuffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                              drm_fd, map_req.offset);
  if (framebuffer == MAP_FAILED)
    die("mmap");

  /* ── Fill the screen with the background color ─────────────── */
  for (uint32_t y = 0; y < height; y++) {
    uint32_t *row = (uint32_t *)(framebuffer + y * stride);
    for (uint32_t x = 0; x < width; x++) {
      row[x] = BG_COLOR;
    }
  }

  fprintf(stderr, "[drm_canvas] Background filled.\n");

  /* ── FreeType2: Initialize and load the font ───────────────── */
  FT_Library ft_lib;
  FT_Face ft_face;

  if (FT_Init_FreeType(&ft_lib)) {
    fprintf(stderr, "FT_Init_FreeType failed.\n");
    goto cleanup_drm;
  }

  if (FT_New_Face(ft_lib, FONT_PATH, 0, &ft_face)) {
    fprintf(stderr, "FT_New_Face failed — check FONT_PATH: %s\n", FONT_PATH);
    FT_Done_FreeType(ft_lib);
    goto cleanup_drm;
  }

  FT_Set_Pixel_Sizes(ft_face, 0, FONT_SIZE);

  fprintf(stderr, "[drm_canvas] FreeType loaded: %s @ %dpx\n", FONT_PATH,
          FONT_SIZE);

  /* ── Render text centered on screen ────────────────────────── */
  {
    const char *text = DEMO_TEXT;

    /* Measure text width for centering */
    int text_w = measure_text_width(ft_face, text);

    /* Center horizontally, place roughly at vertical center.
     * The Y coordinate is the baseline — offset it up by half
     * the font size so the text appears centered.              */
    int text_x = ((int)width - text_w) / 2;
    int text_y = ((int)height + FONT_SIZE) / 2;

    /* Clamp to safe minimum */
    if (text_x < 0)
      text_x = 0;
    if (text_y < FONT_SIZE)
      text_y = FONT_SIZE;

    fprintf(stderr, "[drm_canvas] Drawing \"%s\" at (%d, %d)\n", text, text_x,
            text_y);

    draw_text(ft_face, text, framebuffer, stride, width, height, text_x,
              text_y);
  }

  /* ── FreeType cleanup ──────────────────────────────────────── */
  FT_Done_Face(ft_face);
  FT_Done_FreeType(ft_lib);

  fprintf(stderr, "[drm_canvas] FreeType cleaned up.\n");

  /* ── Mode Setting: Push framebuffer to the display ─────────── */
  if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector->connector_id, 1,
                     &mode) < 0)
    die("drmModeSetCrtc");

  fprintf(stderr, "[drm_canvas] Screen painted! Holding for 3 seconds...\n");
  sleep(3);

  /* ── Cleanup: Restore original state & free everything ─────── */
cleanup_drm:

  /* Restore original CRTC so the TTY comes back */
  if (orig_crtc) {
    drmModeSetCrtc(drm_fd, orig_crtc->crtc_id, orig_crtc->buffer_id,
                   orig_crtc->x, orig_crtc->y, &connector->connector_id, 1,
                   &orig_crtc->mode);
    drmModeFreeCrtc(orig_crtc);
  }

  munmap(framebuffer, size);
  drmModeRmFB(drm_fd, fb_id);

  struct drm_mode_destroy_dumb destroy_req = {0};
  destroy_req.handle = handle;
  drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

  drmModeFreeConnector(connector);
  drmModeFreeResources(resources);
  close(drm_fd);

  fprintf(stderr, "[drm_canvas] Original display restored. Done.\n");
  return EXIT_SUCCESS;
}
