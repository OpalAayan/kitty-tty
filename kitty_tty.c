#define _GNU_SOURCE
/*
 * kitty_tty.c — Bare-metal DRM terminal emulator.
 *
 * Single-file C program: DRM framebuffer, FreeType glyph rendering,
 * libvterm terminal emulation, tabbed sessions with vertical splits,
 * shadow-buffered two-pass rendering, and Unix socket IPC.
 *
 * Compile: make
 * Run:     sudo ./kitty_tty
 * Log:     /tmp/kitty-tty.log
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <vterm.h>

/* -- Constants ---------------------------------------------------- */

#define IPC_READ_TIMEOUT_MS 200

static void get_socket_path(char *buf, size_t size) {
  snprintf(buf, size, "/tmp/kitty_tty_%d.sock", getuid());
}

/* -- Logging ------------------------------------------------------ */

#define LOG_PATH "/tmp/kitty-tty.log"

static FILE *g_logfile = NULL;

static void log_init(void) {
  g_logfile = fopen(LOG_PATH, "w");
  if (!g_logfile)
    g_logfile = stderr;
}

static void log_close(void) {
  if (g_logfile && g_logfile != stderr) {
    fclose(g_logfile);
    g_logfile = NULL;
  }
}

__attribute__((format(printf, 2, 3))) static void
log_msg(const char *level, const char *fmt, ...) {
  if (!g_logfile)
    return;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char tb[16];
  strftime(tb, sizeof(tb), "%H:%M:%S", t);
  fprintf(g_logfile, "[%s][%s] ", tb, level);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_logfile, fmt, ap);
  va_end(ap);
  fflush(g_logfile);
}

#define LOG_INFO(...) log_msg("INFO", __VA_ARGS__)
#define LOG_WARN(...) log_msg("WARN", __VA_ARGS__)
#define LOG_FATAL(...) log_msg("FATAL", __VA_ARGS__)

/* -- Helpers ------------------------------------------------------ */

static inline uint32_t rgb_pack(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define MAX_EAGAIN_RETRIES 50

static int write_all(int fd, const char *buf, size_t len) {
  int eagain_count = 0;
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN) {
        if (++eagain_count > MAX_EAGAIN_RETRIES)
          return -1;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        poll(&pfd, 1, 100);
        continue;
      }
      return -1;
    }
    eagain_count = 0;
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

/* -- Termios — Raw Mode ------------------------------------------- */

static struct termios g_orig_termios;
static int g_termios_saved = 0;

static void disable_raw_mode(void) {
  if (g_termios_saved) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_termios_saved = 0;
    LOG_INFO("Restored original termios.\n");
  }
}

static int enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) {
    LOG_FATAL("tcgetattr failed: %s\n", strerror(errno));
    return -1;
  }
  g_termios_saved = 1;

  struct termios raw = g_orig_termios;
  raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_cflag |= (unsigned)(CS8);
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
    LOG_FATAL("tcsetattr failed: %s\n", strerror(errno));
    return -1;
  }

  struct termios verify;
  if (tcgetattr(STDIN_FILENO, &verify) == 0) {
    if (verify.c_lflag & ISIG)
      LOG_WARN("ISIG still enabled after tcsetattr!\n");
    if (verify.c_lflag & ECHO)
      LOG_WARN("ECHO still enabled after tcsetattr!\n");
    if (verify.c_lflag & ICANON)
      LOG_WARN("ICANON still enabled after tcsetattr!\n");
  }

  LOG_INFO("Raw mode enabled.\n");
  return 0;
}

/* -- State Structures --------------------------------------------- */

typedef struct {
  int font_size;
  uint32_t default_bg;
  uint32_t default_fg;
  uint32_t cursor_bg;
  uint32_t cursor_fg;
  uint32_t tabbar_bg;
  uint32_t tabbar_fg;
  uint32_t tabbar_active;
} AppConfig;

typedef struct {
  int fd;
  uint32_t width, height, stride, size, handle, fb_id, crtc_id, conn_id;
  drmModeModeInfo mode;
  drmModeCrtc *orig_crtc;
  uint8_t *framebuffer;
  uint8_t *back_buffer;
} DrmState;

typedef struct {
  FT_Library lib;
  FT_Face face;
  int cell_w, cell_h, ascender;
} FontState;

typedef struct {
  DrmState drm;
  FontState font;
} HardwareState;

#define MAX_TABS 8
#define MAX_PANES 2

typedef struct {
  int master_fd;
  pid_t child_pid;
  VTerm *vt;
  VTermScreen *vtscreen;
  int term_cols;
  int start_col;
} PaneSession;

typedef struct {
  PaneSession panes[MAX_PANES];
  int num_panes;
  int active_pane;
  int term_rows;
  int active;
} TabSession;

typedef struct {
  AppConfig cfg;
  HardwareState hw;
  TabSession tabs[MAX_TABS];
  int active_tab;
  int num_tabs;
  int initialized;
} AppCtx;

static AppCtx g_app = {
    .cfg =
        {
            .font_size = 20,
            .default_bg = 0x002E3440,
            .default_fg = 0x00D8DEE9,
            .cursor_bg = 0x00D8DEE9,
            .cursor_fg = 0x002E3440,
            .tabbar_bg = 0x003B4252,
            .tabbar_fg = 0x00D8DEE9,
            .tabbar_active = 0x0088C0D0,
        },
    .hw = {.drm = {.fd = -1}},
};

static int g_ipc_fd = -1;
static int g_tty_fd = -1;
static volatile sig_atomic_t g_vt_active = 1;
static struct vt_mode g_orig_vt_mode;
static int g_vt_mode_saved = 0;

/* -- Signals ------------------------------------------------------ */

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_last_signal = 0;

static void signal_handler(int sig) {
  g_last_signal = sig;
  g_shutdown = 1;
}

static void vt_release_handler(int sig) {
  (void)sig;
  g_vt_active = 0;
  if (g_app.hw.drm.fd >= 0)
    drmDropMaster(g_app.hw.drm.fd);
  if (g_tty_fd >= 0)
    ioctl(g_tty_fd, VT_RELDISP, 1);
}

static void vt_acquire_handler(int sig) {
  (void)sig;
  g_vt_active = 1;
  if (g_app.hw.drm.fd >= 0)
    drmSetMaster(g_app.hw.drm.fd);
  if (g_tty_fd >= 0)
    ioctl(g_tty_fd, VT_RELDISP, VT_ACKACQ);
}

static void install_signal_handlers(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGCHLD, &sa, NULL);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = vt_release_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = vt_acquire_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, NULL);
}

/* -- VT Switching ------------------------------------------------- */

static int vt_setup(void) {
  g_tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (g_tty_fd < 0)
    return 0;

  if (ioctl(g_tty_fd, VT_GETMODE, &g_orig_vt_mode) < 0) {
    LOG_WARN("VT_GETMODE failed: %s\n", strerror(errno));
    close(g_tty_fd);
    g_tty_fd = -1;
    return 0;
  }
  g_vt_mode_saved = 1;

  struct vt_mode vtm;
  memset(&vtm, 0, sizeof(vtm));
  vtm.mode = VT_PROCESS;
  vtm.relsig = SIGUSR1;
  vtm.acqsig = SIGUSR2;

  if (ioctl(g_tty_fd, VT_SETMODE, &vtm) < 0) {
    LOG_WARN("VT_SETMODE failed: %s\n", strerror(errno));
    g_vt_mode_saved = 0;
    close(g_tty_fd);
    g_tty_fd = -1;
    return 0;
  }

  LOG_INFO("VT_PROCESS mode enabled.\n");
  return 0;
}

static void vt_cleanup(void) {
  if (g_vt_mode_saved && g_tty_fd >= 0) {
    ioctl(g_tty_fd, VT_SETMODE, &g_orig_vt_mode);
    g_vt_mode_saved = 0;
  }
  if (g_tty_fd >= 0) {
    close(g_tty_fd);
    g_tty_fd = -1;
  }
}

/* -- Cleanup ------------------------------------------------------ */

static void full_cleanup(void) {
  LOG_INFO("Running full cleanup...\n");
  disable_raw_mode();
  vt_cleanup();

  if (g_ipc_fd >= 0) {
    close(g_ipc_fd);
    g_ipc_fd = -1;
  }
  char sock_path[64];
  get_socket_path(sock_path, sizeof(sock_path));
  unlink(sock_path);

  for (int i = 0; i < MAX_TABS; i++) {
    TabSession *tab = &g_app.tabs[i];
    if (!tab->active)
      continue;
    for (int p = 0; p < tab->num_panes; p++) {
      PaneSession *pane = &tab->panes[p];
      if (pane->master_fd >= 0) {
        close(pane->master_fd);
        pane->master_fd = -1;
      }
      if (pane->child_pid > 0) {
        waitpid(pane->child_pid, NULL, WNOHANG);
        pane->child_pid = -1;
      }
      if (pane->vt) {
        vterm_free(pane->vt);
        pane->vt = NULL;
        pane->vtscreen = NULL;
      }
    }
    tab->active = 0;
  }

  HardwareState *hw = &g_app.hw;
  if (hw->font.face) {
    FT_Done_Face(hw->font.face);
    hw->font.face = NULL;
  }
  if (hw->font.lib) {
    FT_Done_FreeType(hw->font.lib);
    hw->font.lib = NULL;
  }

  DrmState *drm = &hw->drm;
  if (drm->orig_crtc) {
    drmModeSetCrtc(drm->fd, drm->orig_crtc->crtc_id, drm->orig_crtc->buffer_id,
                   drm->orig_crtc->x, drm->orig_crtc->y, &drm->conn_id, 1,
                   &drm->orig_crtc->mode);
    drmModeFreeCrtc(drm->orig_crtc);
    drm->orig_crtc = NULL;
  }
  free(drm->back_buffer);
  drm->back_buffer = NULL;
  if (drm->framebuffer && drm->framebuffer != MAP_FAILED) {
    munmap(drm->framebuffer, drm->size);
    drm->framebuffer = NULL;
  }
  if (drm->fb_id) {
    drmModeRmFB(drm->fd, drm->fb_id);
    drm->fb_id = 0;
  }
  if (drm->handle) {
    struct drm_mode_destroy_dumb dreq = {0};
    dreq.handle = drm->handle;
    drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    drm->handle = 0;
  }
  if (drm->fd >= 0) {
    close(drm->fd);
    drm->fd = -1;
  }
  LOG_INFO("Goodbye.\n");
  log_close();
}

/* -- DRM Setup ---------------------------------------------------- */

static int drm_init(DrmState *drm) {
  memset(drm, 0, sizeof(*drm));
  drm->fd = -1;
  drmModeRes *res = NULL;
  drmModeConnector *conn = NULL;

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
      drm->fd = fd;
      res = r;
      LOG_INFO("Found KMS device: %s (%d conn, %d CRTCs)\n", path,
               r->count_connectors, r->count_crtcs);
      break;
    }
    drmModeFreeResources(r);
    close(fd);
  }
  if (drm->fd < 0) {
    LOG_FATAL("No KMS device found.\n");
    goto fail;
  }

  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(drm->fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      conn = c;
      break;
    }
    drmModeFreeConnector(c);
  }
  if (!conn) {
    LOG_FATAL("No connected monitor.\n");
    goto fail;
  }

  drm->mode = conn->modes[0];
  drm->width = drm->mode.hdisplay;
  drm->height = drm->mode.vdisplay;
  drm->conn_id = conn->connector_id;
  LOG_INFO("Resolution: %ux%u\n", drm->width, drm->height);

  if (conn->encoder_id) {
    drmModeEncoder *enc = drmModeGetEncoder(drm->fd, conn->encoder_id);
    if (enc) {
      drm->crtc_id = enc->crtc_id;
      drmModeFreeEncoder(enc);
    }
  }
  if (!drm->crtc_id && res->count_crtcs > 0)
    drm->crtc_id = res->crtcs[0];
  if (!drm->crtc_id) {
    LOG_FATAL("No CRTC.\n");
    goto fail;
  }

  drm->orig_crtc = drmModeGetCrtc(drm->fd, drm->crtc_id);

  {
    struct drm_mode_create_dumb creq = {0};
    creq.width = drm->width;
    creq.height = drm->height;
    creq.bpp = 32;
    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
      perror("CREATE_DUMB");
      goto fail;
    }
    drm->stride = creq.pitch;
    drm->size = creq.size;
    drm->handle = creq.handle;
  }
  if (drmModeAddFB(drm->fd, drm->width, drm->height, 24, 32, drm->stride,
                   drm->handle, &drm->fb_id) < 0) {
    perror("AddFB");
    goto fail;
  }
  {
    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = drm->handle;
    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
      perror("MAP_DUMB");
      goto fail;
    }
    drm->framebuffer = mmap(NULL, drm->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            drm->fd, mreq.offset);
    if (drm->framebuffer == MAP_FAILED) {
      perror("mmap");
      drm->framebuffer = NULL;
      goto fail;
    }
  }

  drm->back_buffer = malloc(drm->size);
  if (!drm->back_buffer) {
    perror("back_buffer malloc");
    goto fail;
  }
  if (drmModeSetCrtc(drm->fd, drm->crtc_id, drm->fb_id, 0, 0, &drm->conn_id, 1,
                     &drm->mode) < 0) {
    perror("SetCrtc");
    goto fail;
  }

  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  LOG_INFO("DRM initialized (stride=%u).\n", drm->stride);
  return 0;

fail:
  if (conn)
    drmModeFreeConnector(conn);
  if (res)
    drmModeFreeResources(res);
  return -1;
}

/* -- FreeType Setup ----------------------------------------------- */

static const char *font_fallbacks[] = {
    "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
    "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/TTF/FiraCodeNerdFont-Regular.ttf",
    "/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    NULL};

static int font_init(FontState *font, const AppConfig *cfg) {
  memset(font, 0, sizeof(*font));
  if (FT_Init_FreeType(&font->lib)) {
    LOG_FATAL("FT init failed.\n");
    return -1;
  }

  const char *found = NULL;
  for (int i = 0; font_fallbacks[i]; i++) {
    if (access(font_fallbacks[i], F_OK) == 0) {
      found = font_fallbacks[i];
      break;
    }
  }
  if (!found) {
    LOG_FATAL("No monospace font found. Install one of:\n"
              "  ttf-jetbrains-mono-nerd  (Arch)\n"
              "  fonts-jetbrains-mono     (Debian/Ubuntu)\n"
              "  ttf-fira-code            (Arch)\n"
              "  ttf-dejavu               / fonts-dejavu-core\n"
              "  ttf-liberation           / fonts-liberation\n");
    FT_Done_FreeType(font->lib);
    font->lib = NULL;
    return -1;
  }

  if (FT_New_Face(font->lib, found, 0, &font->face)) {
    LOG_FATAL("FT_New_Face: %s\n", found);
    FT_Done_FreeType(font->lib);
    font->lib = NULL;
    return -1;
  }
  FT_Set_Pixel_Sizes(font->face, 0, cfg->font_size);
  if (FT_Load_Char(font->face, 'M', FT_LOAD_DEFAULT)) {
    LOG_FATAL("FT_Load_Char('M') failed.\n");
    FT_Done_Face(font->face);
    FT_Done_FreeType(font->lib);
    font->face = NULL;
    font->lib = NULL;
    return -1;
  }
  font->cell_w = (int)(font->face->glyph->advance.x >> 6);
  font->cell_h = (int)(font->face->size->metrics.height >> 6);
  font->ascender = (int)(font->face->size->metrics.ascender >> 6);
  if (font->cell_w <= 0 || font->cell_h <= 0) {
    LOG_FATAL("Bad metrics: %dx%d\n", font->cell_w, font->cell_h);
    FT_Done_Face(font->face);
    FT_Done_FreeType(font->lib);
    font->face = NULL;
    font->lib = NULL;
    return -1;
  }
  LOG_INFO("Font: %s @ %dpx cell %dx%d (asc=%d)\n", found, cfg->font_size,
           font->cell_w, font->cell_h, font->ascender);
  return 0;
}

/* -- Tab / Pane Session ------------------------------------------- */

static int pane_spawn(PaneSession *pane, int rows, int cols, int start_col_px,
                      const HardwareState *hw, const AppConfig *cfg) {
  memset(pane, 0, sizeof(*pane));
  pane->master_fd = -1;
  pane->child_pid = -1;
  pane->term_cols = cols;
  pane->start_col = start_col_px;

  pane->vt = vterm_new(rows, cols);
  if (!pane->vt) {
    LOG_FATAL("vterm_new failed.\n");
    return -1;
  }
  vterm_set_utf8(pane->vt, 1);

  VTermState *vtstate = vterm_obtain_state(pane->vt);
  VTermColor def_fg, def_bg;
  vterm_color_rgb(&def_fg, (cfg->default_fg >> 16) & 0xFF,
                  (cfg->default_fg >> 8) & 0xFF, cfg->default_fg & 0xFF);
  vterm_color_rgb(&def_bg, (cfg->default_bg >> 16) & 0xFF,
                  (cfg->default_bg >> 8) & 0xFF, cfg->default_bg & 0xFF);
  vterm_state_set_default_colors(vtstate, &def_fg, &def_bg);

  pane->vtscreen = vterm_obtain_screen(pane->vt);
  vterm_screen_reset(pane->vtscreen, 1);

  int cw = hw->font.cell_w;
  struct winsize ws = {
      .ws_row = (unsigned short)rows,
      .ws_col = (unsigned short)cols,
      .ws_xpixel = (unsigned short)(cols * cw),
      .ws_ypixel = (unsigned short)(rows * hw->font.cell_h),
  };

  pane->child_pid = forkpty(&pane->master_fd, NULL, NULL, &ws);
  if (pane->child_pid < 0) {
    LOG_FATAL("forkpty failed.\n");
    vterm_free(pane->vt);
    pane->vt = NULL;
    pane->vtscreen = NULL;
    return -1;
  }
  if (pane->child_pid == 0) {
    execlp("/bin/bash", "bash", (char *)NULL);
    _exit(EXIT_FAILURE);
  }

  int flags = fcntl(pane->master_fd, F_GETFL);
  if (flags >= 0)
    fcntl(pane->master_fd, F_SETFL, flags | O_NONBLOCK);

  LOG_INFO("Pane spawned (PID %d), master_fd=%d, cols=%d, start_col=%dpx.\n",
           pane->child_pid, pane->master_fd, cols, start_col_px);
  return 0;
}

static int tab_session_init(TabSession *tab, const HardwareState *hw,
                            const AppConfig *cfg) {
  memset(tab, 0, sizeof(*tab));

  int total_cols = (int)hw->drm.width / hw->font.cell_w;
  int rows = ((int)hw->drm.height / hw->font.cell_h) - 1;

  if (total_cols < 1 || rows < 1) {
    LOG_FATAL("Grid too small: %dx%d\n", total_cols, rows);
    return -1;
  }

  tab->term_rows = rows;
  tab->num_panes = 1;
  tab->active_pane = 0;

  LOG_INFO("Grid: %d cols x %d rows\n", total_cols, rows);

  if (pane_spawn(&tab->panes[0], rows, total_cols, 0, hw, cfg) < 0)
    return -1;

  tab->active = 1;
  return 0;
}

static int split_pane_vertical(TabSession *tab, const HardwareState *hw,
                               const AppConfig *cfg) {
  if (tab->num_panes >= MAX_PANES) {
    LOG_WARN("Already at max panes (%d).\n", MAX_PANES);
    return -1;
  }

  int cw = hw->font.cell_w;
  int old_cols = tab->panes[0].term_cols;
  int left_cols = old_cols / 2;
  int right_cols = old_cols - left_cols;

  if (left_cols < 2 || right_cols < 2) {
    LOG_WARN("Not enough columns to split (%d).\n", old_cols);
    return -1;
  }

  tab->panes[0].term_cols = left_cols;
  vterm_set_size(tab->panes[0].vt, tab->term_rows, left_cols);

  struct winsize ws = {
      .ws_row = (unsigned short)tab->term_rows,
      .ws_col = (unsigned short)left_cols,
      .ws_xpixel = (unsigned short)(left_cols * cw),
      .ws_ypixel = (unsigned short)(tab->term_rows * hw->font.cell_h),
  };
  if (ioctl(tab->panes[0].master_fd, TIOCSWINSZ, &ws) < 0)
    LOG_WARN("TIOCSWINSZ on pane 0 failed: %s\n", strerror(errno));
  LOG_INFO("Pane 0 resized to %d cols.\n", left_cols);

  int right_start_px = left_cols * cw;
  if (pane_spawn(&tab->panes[1], tab->term_rows, right_cols, right_start_px, hw,
                 cfg) < 0) {
    tab->panes[0].term_cols = old_cols;
    vterm_set_size(tab->panes[0].vt, tab->term_rows, old_cols);
    ws.ws_col = (unsigned short)old_cols;
    ws.ws_xpixel = (unsigned short)(old_cols * cw);
    ioctl(tab->panes[0].master_fd, TIOCSWINSZ, &ws);
    return -1;
  }

  tab->num_panes = 2;
  tab->active_pane = 1;
  LOG_INFO("Vertical split: pane0=%dcols, pane1=%dcols.\n", left_cols,
           right_cols);
  return 0;
}

/* -- Glyph Blitting & Rendering ----------------------------------- */

static void draw_glyph(const FT_Bitmap *bmp, uint8_t *fb, uint32_t stride,
                       uint32_t scr_w, uint32_t scr_h, int pen_x, int pen_y,
                       uint32_t fg_color, uint32_t bg_color) {
  uint8_t fg_r = (fg_color >> 16) & 0xFF, fg_g = (fg_color >> 8) & 0xFF,
          fg_b = fg_color & 0xFF;
  uint8_t bg_r = (bg_color >> 16) & 0xFF, bg_g = (bg_color >> 8) & 0xFF,
          bg_b = bg_color & 0xFF;
  for (unsigned row = 0; row < bmp->rows; row++) {
    int sy = pen_y + (int)row;
    if (sy < 0 || (uint32_t)sy >= scr_h)
      continue;
    uint32_t *fb_row = (uint32_t *)(fb + (uint32_t)sy * stride);
    for (unsigned col = 0; col < bmp->width; col++) {
      int sx = pen_x + (int)col;
      if (sx < 0 || (uint32_t)sx >= scr_w)
        continue;
      uint8_t a = bmp->buffer[row * (unsigned)bmp->pitch + col];
      if (a == 0)
        continue;
      fb_row[sx] = rgb_pack((uint8_t)((fg_r * a + bg_r * (255 - a)) / 255),
                            (uint8_t)((fg_g * a + bg_g * (255 - a)) / 255),
                            (uint8_t)((fg_b * a + bg_b * (255 - a)) / 255));
    }
  }
}

static uint32_t vterm_color_to_rgb(VTermScreen *vts, VTermColor *c,
                                   uint32_t fb) {
  vterm_screen_convert_color_to_rgb(vts, c);
  if (VTERM_COLOR_IS_RGB(c))
    return rgb_pack(c->rgb.red, c->rgb.green, c->rgb.blue);
  return fb;
}

static void fill_cell_bg(uint8_t *fb, uint32_t stride, uint32_t sw, uint32_t sh,
                         int x, int y, int w, int h, uint32_t color) {
  for (int r = 0; r < h; r++) {
    int sy = y + r;
    if (sy < 0 || (uint32_t)sy >= sh)
      continue;
    uint32_t *row = (uint32_t *)(fb + (uint32_t)sy * stride);
    for (int c = 0; c < w; c++) {
      int sx = x + c;
      if (sx >= 0 && (uint32_t)sx < sw)
        row[sx] = color;
    }
  }
}

static void render_cell_bg(const HardwareState *hw, const AppConfig *cfg,
                           VTermScreen *vts, int row, int col, int is_cursor,
                           int px_offset) {
  const DrmState *drm = &hw->drm;
  int cw = hw->font.cell_w, ch = hw->font.cell_h;
  VTermScreenCell cell;
  VTermPos pos = {.row = row, .col = col};
  vterm_screen_get_cell(vts, pos, &cell);

  if (cell.width == 0)
    return;

  int px = px_offset + col * cw, py = row * ch;
  uint32_t bg = vterm_color_to_rgb(vts, &cell.bg, cfg->default_bg);
  uint32_t fg = vterm_color_to_rgb(vts, &cell.fg, cfg->default_fg);
  if (cell.attrs.reverse) {
    uint32_t t = bg;
    bg = fg;
    fg = t;
  }
  if (is_cursor)
    bg = cfg->cursor_bg;

  int full_px_w = cell.width * cw;
  fill_cell_bg(drm->back_buffer, drm->stride, drm->width, drm->height, px, py,
               full_px_w, ch, bg);
}

static void render_cell_fg(const HardwareState *hw, const AppConfig *cfg,
                           VTermScreen *vts, int row, int col, int is_cursor,
                           int px_offset) {
  const DrmState *drm = &hw->drm;
  int cw = hw->font.cell_w, ch = hw->font.cell_h, asc = hw->font.ascender;
  VTermScreenCell cell;
  VTermPos pos = {.row = row, .col = col};
  vterm_screen_get_cell(vts, pos, &cell);

  if (cell.width == 0)
    return;
  if (cell.chars[0] == 0 || cell.chars[0] == ' ')
    return;

  int px = px_offset + col * cw, py = row * ch;
  uint32_t bg = vterm_color_to_rgb(vts, &cell.bg, cfg->default_bg);
  uint32_t fg = vterm_color_to_rgb(vts, &cell.fg, cfg->default_fg);
  if (cell.attrs.reverse) {
    uint32_t t = bg;
    bg = fg;
    fg = t;
  }
  if (is_cursor) {
    bg = cfg->cursor_bg;
    fg = cfg->cursor_fg;
  }

  FontState *font = (FontState *)&hw->font;
  if (FT_Load_Char(font->face, cell.chars[0], FT_LOAD_RENDER))
    return;
  FT_GlyphSlot g = font->face->glyph;

  int full_px_w = cell.width * cw;
  int advance_px = (int)(g->advance.x >> 6);
  int x_offset = (full_px_w - advance_px) / 2;
  if (x_offset < 0)
    x_offset = 0;

  draw_glyph(&g->bitmap, drm->back_buffer, drm->stride, drm->width, drm->height,
             px + x_offset + g->bitmap_left, py + asc - g->bitmap_top, fg, bg);
}

/* -- Tab Bar ------------------------------------------------------ */

static void draw_ui_string(const HardwareState *hw, int px, int py,
                           const char *str, uint32_t fg, uint32_t bg) {
  FontState *font = (FontState *)&hw->font;
  const DrmState *drm = &hw->drm;
  int pen_x = px;

  for (size_t i = 0; str[i] != '\0'; i++) {
    if (FT_Load_Char(font->face, (FT_ULong)str[i], FT_LOAD_RENDER))
      continue;
    FT_GlyphSlot g = font->face->glyph;
    int gx = pen_x + g->bitmap_left;
    int gy = py + hw->font.ascender - g->bitmap_top;
    draw_glyph(&g->bitmap, drm->back_buffer, drm->stride, drm->width,
               drm->height, gx, gy, fg, bg);
    pen_x += (int)(g->advance.x >> 6);
  }
}

static void render_tab_bar(const AppCtx *ctx) {
  const HardwareState *hw = &ctx->hw;
  const DrmState *drm = &hw->drm;
  int cw = hw->font.cell_w, ch = hw->font.cell_h;
  int bar_y = (int)drm->height - ch;

  fill_cell_bg(drm->back_buffer, drm->stride, drm->width, drm->height, 0, bar_y,
               (int)drm->width, ch, ctx->cfg.tabbar_bg);

  int pen_x = cw / 2;
  for (int i = 0; i < ctx->num_tabs; i++) {
    char label[16];
    snprintf(label, sizeof(label), " %d ", i + 1);

    uint32_t fg, bg;
    if (i == ctx->active_tab) {
      fg = ctx->cfg.cursor_fg;
      bg = ctx->cfg.tabbar_active;
    } else {
      fg = ctx->cfg.tabbar_fg;
      bg = ctx->cfg.tabbar_bg;
    }

    int label_px_w = (int)strlen(label) * cw;
    fill_cell_bg(drm->back_buffer, drm->stride, drm->width, drm->height, pen_x,
                 bar_y, label_px_w, ch, bg);

    draw_ui_string(hw, pen_x, bar_y, label, fg, bg);
    pen_x += label_px_w + cw / 2;
  }
}

/* Two-pass multi-pane renderer with shadow buffer swap. */
static void render_screen(const HardwareState *hw, const TabSession *tab,
                          const AppConfig *cfg) {
  int rows = tab->term_rows;

  for (int p = 0; p < tab->num_panes; p++) {
    const PaneSession *pane = &tab->panes[p];
    if (!pane->vt)
      continue;

    int cols = pane->term_cols;
    int px_off = pane->start_col;
    VTermPos cursor_pos;
    VTermState *vtstate = vterm_obtain_state(pane->vt);
    vterm_state_get_cursorpos(vtstate, &cursor_pos);

    int is_active_pane = (p == tab->active_pane);

    for (int r = 0; r < rows; r++)
      for (int c = 0; c < cols; c++)
        render_cell_bg(
            hw, cfg, pane->vtscreen, r, c,
            (is_active_pane && r == cursor_pos.row && c == cursor_pos.col),
            px_off);

    for (int r = 0; r < rows; r++)
      for (int c = 0; c < cols; c++)
        render_cell_fg(
            hw, cfg, pane->vtscreen, r, c,
            (is_active_pane && r == cursor_pos.row && c == cursor_pos.col),
            px_off);
  }

  if (tab->num_panes == 2) {
    const DrmState *drm = &hw->drm;
    int border_x = tab->panes[1].start_col;
    if (border_x > 0)
      border_x -= 1;
    int border_h = rows * hw->font.cell_h;
    fill_cell_bg(drm->back_buffer, drm->stride, drm->width, drm->height,
                 border_x, 0, 1, border_h, cfg->tabbar_fg);
  }

  render_tab_bar(&g_app);

  /* Swap: copy completed frame to DRM framebuffer */
  memcpy(hw->drm.framebuffer, hw->drm.back_buffer, hw->drm.size);
}

/* -- IPC ---------------------------------------------------------- */

static void print_help(void) {
  char sock_path[64];
  get_socket_path(sock_path, sizeof(sock_path));
  fprintf(stdout,
          "kitty_tty -- Bare-metal DRM terminal emulator\n"
          "\n"
          "Usage:\n"
          "  sudo ./kitty_tty              Start the terminal (server mode)\n"
          "  ./kitty_tty <command>         Send IPC command to running server\n"
          "\n"
          "IPC Commands:\n"
          "  --new-tab, -nt                Open a new tab\n"
          "  --next,    -n                 Switch to the next tab\n"
          "  --prev,    -p                 Switch to the previous tab\n"
          "  --split-v, -s                 Split active tab vertically\n"
          "  --left,    -l                 Focus left pane\n"
          "  --right,   -r                 Focus right pane\n"
          "  --help,    -h                 Show this help message\n"
          "\n"
          "Log: /tmp/kitty-tty.log\n"
          "IPC: %s\n",
          sock_path);
}

static const char *ipc_normalize_cmd(const char *arg) {
  if (strcmp(arg, "--new-tab") == 0 || strcmp(arg, "-nt") == 0)
    return "--new-tab";
  if (strcmp(arg, "--next") == 0 || strcmp(arg, "-n") == 0)
    return "--next";
  if (strcmp(arg, "--prev") == 0 || strcmp(arg, "-p") == 0)
    return "--prev";
  if (strcmp(arg, "--split-v") == 0 || strcmp(arg, "-s") == 0)
    return "--split-v";
  if (strcmp(arg, "--left") == 0 || strcmp(arg, "-l") == 0)
    return "--left";
  if (strcmp(arg, "--right") == 0 || strcmp(arg, "-r") == 0)
    return "--right";
  return NULL;
}

static int ipc_try_client(int argc, char **argv) {
  if (argc >= 2 &&
      (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    print_help();
    return 0;
  }

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  char sock_path[64];
  get_socket_path(sock_path, sizeof(sock_path));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }

  if (argc < 2) {
    fprintf(stderr, "kitty_tty: server already running.\n"
                    "Use --new-tab (-nt), --next (-n), --prev (-p),\n"
                    "    --left (-l), --right (-r),\n"
                    "    --split-v (-s), or --help (-h).\n");
    close(sock);
    return 1;
  }

  const char *cmd = ipc_normalize_cmd(argv[1]);
  if (cmd) {
    write(sock, cmd, strlen(cmd));
  } else {
    fprintf(stderr,
            "kitty_tty: unknown command '%s'\n"
            "Use --help (-h) to see available commands.\n",
            argv[1]);
    close(sock);
    return 1;
  }

  close(sock);
  return 0;
}

static int ipc_server_init(void) {
  char sock_path[64];
  get_socket_path(sock_path, sizeof(sock_path));
  unlink(sock_path);

  g_ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_ipc_fd < 0) {
    LOG_FATAL("socket() failed: %s\n", strerror(errno));
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (bind(g_ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_FATAL("bind(%s) failed: %s\n", sock_path, strerror(errno));
    close(g_ipc_fd);
    g_ipc_fd = -1;
    return -1;
  }

  if (listen(g_ipc_fd, 5) < 0) {
    LOG_FATAL("listen() failed: %s\n", strerror(errno));
    close(g_ipc_fd);
    g_ipc_fd = -1;
    unlink(sock_path);
    return -1;
  }

  int flags = fcntl(g_ipc_fd, F_GETFL);
  if (flags >= 0)
    fcntl(g_ipc_fd, F_SETFL, flags | O_NONBLOCK);

  LOG_INFO("IPC server listening on %s (fd=%d).\n", sock_path, g_ipc_fd);
  return 0;
}

static int ipc_handle_command(const char *cmd) {
  if (strcmp(cmd, "--new-tab") == 0) {
    if (g_app.num_tabs < MAX_TABS) {
      int idx = g_app.num_tabs;
      if (tab_session_init(&g_app.tabs[idx], &g_app.hw, &g_app.cfg) == 0) {
        g_app.num_tabs++;
        g_app.active_tab = idx;
        LOG_INFO("IPC: New tab %d created.\n", idx);
      }
    } else {
      LOG_WARN("IPC: Max tabs (%d) reached.\n", MAX_TABS);
    }
    return 1;
  }

  if (strcmp(cmd, "--next") == 0) {
    if (g_app.num_tabs > 0) {
      g_app.active_tab = (g_app.active_tab + 1) % g_app.num_tabs;
      LOG_INFO("IPC: Switched to tab %d.\n", g_app.active_tab);
    }
    return 1;
  }

  if (strcmp(cmd, "--prev") == 0) {
    if (g_app.num_tabs > 0) {
      g_app.active_tab =
          (g_app.active_tab - 1 + g_app.num_tabs) % g_app.num_tabs;
      LOG_INFO("IPC: Switched to tab %d.\n", g_app.active_tab);
    }
    return 1;
  }

  if (strcmp(cmd, "--split-v") == 0) {
    TabSession *tab = &g_app.tabs[g_app.active_tab];
    if (tab->active) {
      if (split_pane_vertical(tab, &g_app.hw, &g_app.cfg) == 0)
        LOG_INFO("IPC: Split tab %d vertically.\n", g_app.active_tab);
    }
    return 1;
  }

  if (strcmp(cmd, "--left") == 0) {
    TabSession *tab = &g_app.tabs[g_app.active_tab];
    if (tab->active && tab->num_panes == 2) {
      tab->active_pane = 0;
      LOG_INFO("IPC: Focus left pane (tab %d).\n", g_app.active_tab);
    }
    return 1;
  }

  if (strcmp(cmd, "--right") == 0) {
    TabSession *tab = &g_app.tabs[g_app.active_tab];
    if (tab->active && tab->num_panes == 2) {
      tab->active_pane = 1;
      LOG_INFO("IPC: Focus right pane (tab %d).\n", g_app.active_tab);
    }
    return 1;
  }

  LOG_WARN("IPC: Unknown command '%s'\n", cmd);
  return 0;
}

static int ipc_accept_and_handle(void) {
  int client_fd = accept(g_ipc_fd, NULL, NULL);
  if (client_fd < 0)
    return 0;

  int flags = fcntl(client_fd, F_GETFL);
  if (flags >= 0)
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

  struct pollfd pfd = {.fd = client_fd, .events = POLLIN};
  int pret = poll(&pfd, 1, IPC_READ_TIMEOUT_MS);

  int result = 0;
  if (pret > 0 && (pfd.revents & POLLIN)) {
    char cmd_buf[64];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    ssize_t n = read(client_fd, cmd_buf, sizeof(cmd_buf) - 1);
    if (n > 0) {
      cmd_buf[n] = '\0';
      result = ipc_handle_command(cmd_buf);
    }
  } else if (pret == 0) {
    LOG_WARN("IPC: Client sent no data within %dms, closing.\n",
             IPC_READ_TIMEOUT_MS);
  }

  close(client_fd);
  return result;
}

/* -- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
  int client_rc = ipc_try_client(argc, argv);
  if (client_rc == 0)
    return EXIT_SUCCESS;
  if (client_rc == 1)
    return EXIT_FAILURE;

  log_init();
  LOG_INFO("kitty-tty starting (server mode)...\n");
  atexit(full_cleanup);
  install_signal_handlers();

  if (ipc_server_init() < 0)
    return EXIT_FAILURE;
  if (drm_init(&g_app.hw.drm) < 0)
    return EXIT_FAILURE;
  if (font_init(&g_app.hw.font, &g_app.cfg) < 0)
    return EXIT_FAILURE;

  vt_setup();

  if (enable_raw_mode() < 0)
    return EXIT_FAILURE;

  if (tab_session_init(&g_app.tabs[0], &g_app.hw, &g_app.cfg) < 0)
    return EXIT_FAILURE;
  g_app.active_tab = 0;
  g_app.num_tabs = 1;
  g_app.initialized = 1;

  LOG_INFO("Interactive. IPC: --new-tab (-nt), --next (-n), --prev (-p), "
           "--split-v (-s), --left (-l), --right (-r)\n");

#define PFD_PTY_SLOTS (MAX_TABS * MAX_PANES)
#define PFD_STDIN_IDX (PFD_PTY_SLOTS)
#define PFD_IPC_IDX (PFD_PTY_SLOTS + 1)
#define PFD_TOTAL (PFD_PTY_SLOTS + 2)

  struct pollfd pfds[PFD_TOTAL];
  char buf[4096];

  render_screen(&g_app.hw, &g_app.tabs[g_app.active_tab], &g_app.cfg);

  while (!g_shutdown) {
    for (int i = 0; i < MAX_TABS; i++) {
      TabSession *tab = &g_app.tabs[i];
      for (int p = 0; p < MAX_PANES; p++) {
        int slot = i * MAX_PANES + p;
        if (tab->active && p < tab->num_panes && tab->panes[p].master_fd >= 0)
          pfds[slot] =
              (struct pollfd){.fd = tab->panes[p].master_fd, .events = POLLIN};
        else
          pfds[slot] = (struct pollfd){.fd = -1, .events = 0};
      }
    }

    pfds[PFD_STDIN_IDX] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
    pfds[PFD_IPC_IDX] = (struct pollfd){.fd = g_ipc_fd, .events = POLLIN};

    int ret = poll(pfds, PFD_TOTAL, -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (!g_vt_active)
      continue;

    int need_render = 0;

    for (int i = 0; i < MAX_TABS; i++) {
      TabSession *tab = &g_app.tabs[i];
      if (!tab->active)
        continue;

      for (int p = 0; p < tab->num_panes; p++) {
        int slot = i * MAX_PANES + p;
        if (pfds[slot].fd < 0)
          continue;
        PaneSession *pane = &tab->panes[p];

        if (pfds[slot].revents & (POLLIN | POLLHUP)) {
          for (;;) {
            ssize_t n = read(pane->master_fd, buf, sizeof(buf));
            if (n > 0) {
              vterm_input_write(pane->vt, buf, (size_t)n);
              if (i == g_app.active_tab)
                need_render = 1;
              continue;
            }
            if (n == 0 || (n < 0 && errno == EIO)) {
              LOG_INFO("Tab %d pane %d shell exited.\n", i, p);
              close(pane->master_fd);
              pane->master_fd = -1;
              if (pane->child_pid > 0) {
                waitpid(pane->child_pid, NULL, WNOHANG);
                pane->child_pid = -1;
              }
              int any_pane_alive = 0;
              for (int q = 0; q < tab->num_panes; q++)
                if (tab->panes[q].master_fd >= 0)
                  any_pane_alive = 1;
              if (!any_pane_alive) {
                tab->active = 0;
                int any_tab_active = 0;
                for (int j = 0; j < g_app.num_tabs; j++)
                  if (g_app.tabs[j].active)
                    any_tab_active = 1;
                if (!any_tab_active) {
                  g_shutdown = 1;
                  break;
                }
                if (i == g_app.active_tab) {
                  for (int j = 0; j < g_app.num_tabs; j++)
                    if (g_app.tabs[j].active) {
                      g_app.active_tab = j;
                      break;
                    }
                }
              }
              need_render = 1;
              break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            if (errno == EINTR)
              continue;
            break;
          }
          if (pane->vtscreen)
            vterm_screen_flush_damage(pane->vtscreen);
        }
        if (pfds[slot].revents & POLLERR) {
          pane->master_fd = -1;
          need_render = 1;
        }
      }
    }

    if (g_shutdown)
      break;

    if (pfds[PFD_STDIN_IDX].revents & POLLIN) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n > 0) {
        TabSession *tab = &g_app.tabs[g_app.active_tab];
        if (tab->active) {
          PaneSession *pane = &tab->panes[tab->active_pane];
          if (pane->master_fd >= 0)
            write_all(pane->master_fd, buf, (size_t)n);
        }
      }
    }

    if (pfds[PFD_IPC_IDX].revents & POLLIN) {
      if (ipc_accept_and_handle())
        need_render = 1;
    }

    if (need_render && !g_shutdown) {
      TabSession *active = &g_app.tabs[g_app.active_tab];
      if (active->active)
        render_screen(&g_app.hw, active, &g_app.cfg);
    }
  }

  if (g_last_signal)
    LOG_INFO("Exiting due to signal %d.\n", (int)g_last_signal);
  LOG_INFO("Main loop exited. Cleanup via atexit().\n");
  return EXIT_SUCCESS;
}
