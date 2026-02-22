/*
 * evdev_test.c — Phase 7: The Input Translation Engine
 *
 * Standalone test binary that:
 *   1. Dynamically discovers the keyboard in /dev/input/
 *   2. Grabs exclusive control via libevdev
 *   3. Translates raw keycodes → UTF-8 via libxkbcommon
 *   4. Converts special keys → ANSI escape sequences
 *   5. Maintains Alt+Tab/Arrow tab-switching logic (Phase 6)
 *   6. Exits cleanly on Ctrl+C
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -o evdev_test evdev_test.c \
 *       $(pkg-config --cflags --libs libevdev xkbcommon)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/*
 * evdev keycodes are offset by 8 from XKB/X11 keycodes.
 * Every evdev code must be shifted before passing to xkbcommon.
 */
#define EVDEV_TO_XKB(code) ((code) + 8)

/* ── helpers ─────────────────────────────────────────────────────── */

static const char *key_name(unsigned int code) {
  const char *name = libevdev_event_code_get_name(EV_KEY, code);
  return name ? name : "UNKNOWN";
}

/*
 * get_ansi_sequence — convert terminal-specific evdev keycodes
 * into their ANSI escape sequence equivalents.
 *
 * Returns NULL if the key should be handled by xkbcommon instead.
 */
static const char *get_ansi_sequence(unsigned int code) {
  switch (code) {
  case KEY_UP:
    return "\x1b[A";
  case KEY_DOWN:
    return "\x1b[B";
  case KEY_RIGHT:
    return "\x1b[C";
  case KEY_LEFT:
    return "\x1b[D";
  case KEY_HOME:
    return "\x1b[H";
  case KEY_END:
    return "\x1b[F";
  case KEY_INSERT:
    return "\x1b[2~";
  case KEY_DELETE:
    return "\x1b[3~";
  case KEY_PAGEUP:
    return "\x1b[5~";
  case KEY_PAGEDOWN:
    return "\x1b[6~";
  case KEY_BACKSPACE:
    return "\x7f";
  case KEY_ENTER:
    return "\r";
  case KEY_ESC:
    return "\x1b";
  case KEY_TAB:
    return "\t";
  case KEY_F1:
    return "\x1bOP";
  case KEY_F2:
    return "\x1bOQ";
  case KEY_F3:
    return "\x1bOR";
  case KEY_F4:
    return "\x1bOS";
  case KEY_F5:
    return "\x1b[15~";
  case KEY_F6:
    return "\x1b[17~";
  case KEY_F7:
    return "\x1b[18~";
  case KEY_F8:
    return "\x1b[19~";
  case KEY_F9:
    return "\x1b[20~";
  case KEY_F10:
    return "\x1b[21~";
  case KEY_F11:
    return "\x1b[23~";
  case KEY_F12:
    return "\x1b[24~";
  default:
    return NULL;
  }
}

/*
 * Print a byte buffer as hex for debug visibility.
 * e.g. "\x1b[A" → "1b 5b 41"
 */
static void print_hex(const char *buf, int len) {
  for (int i = 0; i < len; i++)
    printf("%02x ", (unsigned char)buf[i]);
}

/* ── dynamic keyboard discovery ──────────────────────────────────── */

static int find_keyboard(struct libevdev **out_dev) {
  const char *input_dir = "/dev/input";
  DIR *dir = opendir(input_dir);
  if (!dir) {
    perror("opendir /dev/input");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", input_dir, entry->d_name);

    struct stat st;
    if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode))
      continue;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
      close(fd);
      continue;
    }

    if (libevdev_has_event_type(dev, EV_KEY) &&
        libevdev_has_event_code(dev, EV_KEY, KEY_A)) {
      fprintf(stderr, "[INFO] Found keyboard: %s (%s)\n",
              libevdev_get_name(dev), path);
      closedir(dir);
      *out_dev = dev;
      return fd;
    }

    libevdev_free(dev);
    close(fd);
  }

  closedir(dir);
  fprintf(stderr, "[ERROR] No keyboard found in %s\n", input_dir);
  return -1;
}

/* ── xkbcommon initialisation ────────────────────────────────────── */

/*
 * Set up the full xkbcommon pipeline:
 *   context → keymap (from system defaults) → state
 *
 * Returns 0 on success, -1 on failure.
 */
static int xkb_init(struct xkb_context **out_ctx,
                    struct xkb_keymap **out_keymap,
                    struct xkb_state **out_state) {
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!ctx) {
    fprintf(stderr, "[ERROR] Failed to create xkb_context\n");
    return -1;
  }

  /*
   * NULL rules = system default layout.
   * This respects XKB_DEFAULT_LAYOUT, XKB_DEFAULT_MODEL etc.
   */
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) {
    fprintf(stderr, "[ERROR] Failed to load default xkb_keymap\n");
    xkb_context_unref(ctx);
    return -1;
  }

  struct xkb_state *state = xkb_state_new(keymap);
  if (!state) {
    fprintf(stderr, "[ERROR] Failed to create xkb_state\n");
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    return -1;
  }

  *out_ctx = ctx;
  *out_keymap = keymap;
  *out_state = state;
  return 0;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(void) {
  struct libevdev *dev = NULL;

  /* 1. Discover the keyboard */
  int fd = find_keyboard(&dev);
  if (fd < 0)
    return EXIT_FAILURE;

  /* 2. Initialise xkbcommon */
  struct xkb_context *xkb_ctx = NULL;
  struct xkb_keymap *xkb_kmap = NULL;
  struct xkb_state *xkb_st = NULL;

  if (xkb_init(&xkb_ctx, &xkb_kmap, &xkb_st) < 0) {
    libevdev_free(dev);
    close(fd);
    return EXIT_FAILURE;
  }

  /* 3. Grab exclusive access */
  int rc = libevdev_grab(dev, LIBEVDEV_GRAB);
  if (rc < 0) {
    fprintf(stderr, "[ERROR] Failed to grab keyboard: %s\n", strerror(-rc));
    goto cleanup;
  }

  fprintf(stderr, "╔══════════════════════════════════════════════════╗\n"
                  "║  Phase 7 — Input Translation Engine              ║\n"
                  "║                                                  ║\n"
                  "║  Keyboard grabbed exclusively.                   ║\n"
                  "║  xkbcommon loaded with system default layout.    ║\n"
                  "║                                                  ║\n"
                  "║  Try: typing letters (shift/caps aware),         ║\n"
                  "║       arrow keys, F-keys, Alt+Tab, Alt+Arrows.   ║\n"
                  "║  Press Ctrl+C to exit cleanly.                   ║\n"
                  "╚══════════════════════════════════════════════════╝\n\n");

  /* ── modifier state (manual tracking for Alt combos) ── */
  int alt_held = 0;
  int ctrl_held = 0;

  /* 4. Event loop */
  while (1) {
    struct input_event ev;
    rc = libevdev_next_event(
        dev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);

    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
      while (rc == LIBEVDEV_READ_STATUS_SYNC)
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
      continue;
    }

    if (rc != LIBEVDEV_READ_STATUS_SUCCESS)
      continue;

    if (ev.type != EV_KEY)
      continue;

    /*
     * ── Always update XKB state first ──
     * This keeps Shift, Caps Lock, Ctrl, Alt, etc. accurate
     * inside xkbcommon regardless of our manual tracking.
     */
    xkb_state_update_key(xkb_st, EVDEV_TO_XKB(ev.code),
                         ev.value ? XKB_KEY_DOWN : XKB_KEY_UP);

    /* ── Manual modifier tracking (for our tab/exit logic) ── */
    if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
      alt_held = (ev.value != 0);
      continue;
    }
    if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
      ctrl_held = (ev.value != 0);
      continue;
    }

    /* ── Key release: just log it ── */
    if (ev.value == 0) {
      printf("[RELEASE] %s (%u)\n", key_name(ev.code), ev.code);
      fflush(stdout);
      continue;
    }

    /* ─────────────────────────────────────────────────────────
     * From here: ev.value == 1 (press) or 2 (repeat)
     * ───────────────────────────────────────────────────────── */

    /* ── Exit: Ctrl+C ── */
    if (ctrl_held && ev.code == KEY_C) {
      fprintf(stderr, "\n[EXIT] Ctrl+C detected — releasing grab.\n");
      goto cleanup;
    }

    /* ── Phase 6 Alt combos (bypass translation) ── */
    if (alt_held) {
      switch (ev.code) {
      case KEY_TAB:
        printf("[ACTION] Next Tab triggered.\n");
        break;
      case KEY_RIGHT:
        printf("[ACTION] Move Tab Right triggered.\n");
        break;
      case KEY_LEFT:
        printf("[ACTION] Move Tab Left triggered.\n");
        break;
      case KEY_UP:
        printf("[ACTION] Move Tab Up triggered.\n");
        break;
      case KEY_DOWN:
        printf("[ACTION] Move Tab Down triggered.\n");
        break;
      default:
        printf("[ALT+KEY] Alt + %s\n", key_name(ev.code));
        break;
      }
      fflush(stdout);
      continue;
    }

    /* ── Check for ANSI special key first ── */
    const char *ansi = get_ansi_sequence(ev.code);
    if (ansi) {
      int len = (int)strlen(ansi);
      printf("[ANSI] %-12s → bytes: ", key_name(ev.code));
      print_hex(ansi, len);
      printf("  (%d byte%s)\n", len, len == 1 ? "" : "s");
      fflush(stdout);
      continue;
    }

    /* ── Standard key: translate via xkbcommon ── */
    char utf8_buf[64];
    int utf8_len = xkb_state_key_get_utf8(xkb_st, EVDEV_TO_XKB(ev.code),
                                          utf8_buf, sizeof(utf8_buf));

    if (utf8_len > 0) {
      printf("[TEXT] Translated to: \"%s\"  (", utf8_buf);
      print_hex(utf8_buf, utf8_len);
      printf(")\n");
    } else {
      /* No text output — modifier-only or dead key */
      xkb_keysym_t sym =
          xkb_state_key_get_one_sym(xkb_st, EVDEV_TO_XKB(ev.code));
      char sym_name[64];
      xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));
      printf("[KEY]  %s (keysym: %s, no text output)\n", key_name(ev.code),
             sym_name);
    }

    fflush(stdout);
  }

cleanup:
  libevdev_grab(dev, LIBEVDEV_UNGRAB);
  libevdev_free(dev);
  close(fd);
  xkb_state_unref(xkb_st);
  xkb_keymap_unref(xkb_kmap);
  xkb_context_unref(xkb_ctx);
  fprintf(stderr, "[INFO] Cleanup complete. Bye.\n");
  return EXIT_SUCCESS;
}
