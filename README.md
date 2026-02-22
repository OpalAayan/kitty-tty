# kitty-tty

Bare-metal DRM terminal emulator written in pure C. Renders directly to
the Linux framebuffer using KMS/DRM, with FreeType font rasterization
and libvterm terminal emulation. No X11, no Wayland, no compositor.

## Features

- Direct DRM/KMS framebuffer rendering (no display server)
- FreeType glyph rasterization with Nerd Font support
- Shadow-buffered two-pass rendering (flicker-free)
- Tabbed sessions (up to 8 tabs)
- Vertical split panes (50/50)
- Unix socket IPC for tab/pane control
- Cooperative VT switching (Ctrl+Alt+Fn)
- Strict raw mode (Ctrl+C/Z pass to shell)
- Nord color scheme

## Dependencies

- libdrm
- freetype2
- libvterm
- libutil (glibc)
- A monospace TTF font (auto-detected at runtime)

### Arch Linux

```
sudo pacman -S libdrm freetype2 libvterm ttf-jetbrains-mono-nerd
```

### Debian/Ubuntu

```
sudo apt install libdrm-dev libfreetype-dev libvterm-dev fonts-jetbrains-mono
```

Any of these fonts will be detected automatically: JetBrains Mono, Fira Code,
DejaVu Sans Mono, Liberation Mono.

## Build

```
make
```

## Install

```
sudo make install            # installs to /usr/local/bin
sudo make install PREFIX=/usr # or /usr/bin for AUR/packages
```

To uninstall:

```
sudo make uninstall
```

## Usage

Must run from a Linux TTY (not a terminal emulator). Requires root for
DRM access.

```
sudo ./kitty_tty
```

The first instance starts the server. Subsequent invocations send IPC
commands:

```
./kitty_tty --new-tab       # Open a new tab
./kitty_tty --next          # Switch to next tab
./kitty_tty --prev          # Switch to previous tab
./kitty_tty --split-v       # Split active tab vertically
./kitty_tty --left          # Focus left pane
./kitty_tty --right         # Focus right pane
./kitty_tty --help          # Show help
```

Short flags: `-nt`, `-n`, `-p`, `-s`, `-l`, `-r`, `-h`.

## Configuration

Edit the `AppConfig` struct in `kitty_tty.c`:

- `font_size` -- pixel size (default: 20)
- Colors use 0x00RRGGBB format (Nord palette by default)

Fonts are auto-detected from a built-in fallback list. To change the
priority order, edit the `font_fallbacks[]` array in `kitty_tty.c`.

## Files

| File | Purpose |
|------|---------|
| `kitty_tty.c` | Main terminal emulator |
| `Makefile` | Build configuration |

## Logs

Runtime logs are written to `/tmp/kitty-tty.log`.
IPC socket is per-user at `/tmp/kitty_tty_<uid>.sock`.

## License

See repository for license information.
