# ─────────────────────────────────────────────────────────────────
#  opalterm Makefile — Release
# ─────────────────────────────────────────────────────────────────

CC       := gcc
CFLAGS   := -Wall -Wextra -Wpedantic -O2
LDFLAGS  :=

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

# pkg-config flags
FT2_CFLAGS  := $(shell pkg-config --cflags freetype2)
FT2_LIBS    := $(shell pkg-config --libs freetype2)
DRM_CFLAGS  := $(shell pkg-config --cflags libdrm)
DRM_LIBS    := $(shell pkg-config --libs libdrm)

# ── Targets ──────────────────────────────────────────────────────

.PHONY: all clean install uninstall

all: opalterm

opalterm: opalterm.c
	$(CC) $(CFLAGS) $(FT2_CFLAGS) $(DRM_CFLAGS) \
		-o $@ $< \
		-lutil -lvterm $(FT2_LIBS) $(DRM_LIBS)

install: opalterm
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 opalterm $(DESTDIR)$(BINDIR)/opalterm

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/opalterm

clean:
	rm -f opalterm
