# Makefile - build system for myfreeze + myshot
#
# Targets:
#   all         - generate protocol glue code and build myfreeze (default)
#   install     - install myfreeze + myshot to ~/.local/bin/
#   uninstall   - remove installed binaries from ~/.local/bin/
#   clean       - remove built binary and generated protocol glue files
#   autoinstall - all + install in one step (make autoinstall)
#   autoclean   - clean + uninstall in one step (make autoclean)
#
# Dependencies (Arch):
#   sudo pacman -S wayland wayland-protocols wayland-utils grim slurp wl-clipboard
#   yay -S wlr-protocols
#
# Build & install:
#   make autoinstall

CC      = cc
TARGETS = myfreeze

# Source files: our code + all generated protocol glue
SRC_FREEZE = myfreeze.c \
             wlr-screencopy-unstable-v1-client-protocol.c \
             wlr-layer-shell-unstable-v1-client-protocol.c \
             fractional-scale-v1-client-protocol.c \
             viewporter-client-protocol.c \
             xdg-shell-client-protocol.c

# Compiler and linker flags via pkg-config (no libpng needed anymore)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS   := $(shell pkg-config --libs   wayland-client)

CFLAGS  = -O2 -Wall -Wextra $(WAYLAND_CFLAGS)
LDFLAGS = $(WAYLAND_LIBS)

# --------------------------------------------------------------------------
# Protocol XML auto-detection
# Searches common install paths so this works on Arch, Ubuntu, Fedora, etc.
# --------------------------------------------------------------------------

# wlr-protocols (wlr-screencopy + wlr-layer-shell)
SCREENCOPY_XML  := $(shell find /usr /opt -name 'wlr-screencopy-unstable-v1.xml'   2>/dev/null | head -1)
LAYERSHELL_XML  := $(shell find /usr /opt -name 'wlr-layer-shell-unstable-v1.xml'  2>/dev/null | head -1)

# wayland-protocols stable/staging
FRACTIONAL_XML  := $(shell find /usr /opt -name 'fractional-scale-v1.xml'           2>/dev/null | head -1)
VIEWPORTER_XML  := $(shell find /usr /opt -name 'viewporter.xml'                    2>/dev/null | head -1)
XDG_XML         := $(shell find /usr /opt -name 'xdg-shell.xml'                     2>/dev/null | head -1)

# wayland-scanner: converts protocol XML -> C header + implementation
SCANNER = wayland-scanner

# Validate all XMLs are found before doing anything
ifeq ($(SCREENCOPY_XML),)
$(error Cannot find wlr-screencopy-unstable-v1.xml - install wlr-protocols)
endif
ifeq ($(LAYERSHELL_XML),)
$(error Cannot find wlr-layer-shell-unstable-v1.xml - install wlr-protocols)
endif
ifeq ($(FRACTIONAL_XML),)
$(error Cannot find fractional-scale-v1.xml - install wayland-protocols)
endif
ifeq ($(VIEWPORTER_XML),)
$(error Cannot find viewporter.xml - install wayland-protocols)
endif
ifeq ($(XDG_XML),)
$(error Cannot find xdg-shell.xml - install wayland-protocols)
endif

.PHONY: all clean install uninstall autoinstall autoclean

# --------------------------------------------------------------------------
# Default target: generate all protocol glue then build
# --------------------------------------------------------------------------
all: wlr-screencopy-unstable-v1-client-protocol.h \
     wlr-screencopy-unstable-v1-client-protocol.c \
     wlr-layer-shell-unstable-v1-client-protocol.h \
     wlr-layer-shell-unstable-v1-client-protocol.c \
     fractional-scale-v1-client-protocol.h \
     fractional-scale-v1-client-protocol.c \
     viewporter-client-protocol.h \
     viewporter-client-protocol.c \
     xdg-shell-client-protocol.h \
     xdg-shell-client-protocol.c \
     $(TARGETS)

# --------------------------------------------------------------------------
# Protocol glue generation (wayland-scanner)
# Each protocol needs a client-header (.h) and private-code (.c)
# --------------------------------------------------------------------------

# wlr-screencopy: direct screen capture without grim
wlr-screencopy-unstable-v1-client-protocol.h: $(SCREENCOPY_XML)
	$(SCANNER) client-header $< $@

wlr-screencopy-unstable-v1-client-protocol.c: $(SCREENCOPY_XML)
	$(SCANNER) private-code $< $@

# wlr-layer-shell: fullscreen overlay surface (OVERLAY layer)
wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYERSHELL_XML)
	$(SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-client-protocol.c: $(LAYERSHELL_XML)
	$(SCANNER) private-code $< $@

# wp-fractional-scale: get exact scale factor for HiDPI displays
fractional-scale-v1-client-protocol.h: $(FRACTIONAL_XML)
	$(SCANNER) client-header $< $@

fractional-scale-v1-client-protocol.c: $(FRACTIONAL_XML)
	$(SCANNER) private-code $< $@

# wp-viewporter: map buffer pixels 1:1 to screen pixels (no blur on HiDPI)
viewporter-client-protocol.h: $(VIEWPORTER_XML)
	$(SCANNER) client-header $< $@

viewporter-client-protocol.c: $(VIEWPORTER_XML)
	$(SCANNER) private-code $< $@

# xdg-shell: required because wlr-layer-shell references xdg_popup_interface
xdg-shell-client-protocol.h: $(XDG_XML)
	$(SCANNER) client-header $< $@

xdg-shell-client-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@

# --------------------------------------------------------------------------
# Compile
# --------------------------------------------------------------------------
myfreeze: $(SRC_FREEZE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --------------------------------------------------------------------------
# Install / uninstall
# --------------------------------------------------------------------------
install: $(TARGETS)
	install -Dm755 myfreeze   $(HOME)/.local/bin/myfreeze
	install -Dm755 myshot.sh  $(HOME)/.local/bin/myshot

uninstall:
	rm -f $(HOME)/.local/bin/myfreeze \
	      $(HOME)/.local/bin/myshot

# --------------------------------------------------------------------------
# Convenience targets
# --------------------------------------------------------------------------

# autoinstall: build everything and install in one command
autoinstall: all install

# autoclean: remove build artifacts and uninstall binaries in one command
autoclean: clean uninstall

# --------------------------------------------------------------------------
# Clean
# --------------------------------------------------------------------------
clean:
	rm -f $(TARGETS) \
	       wlr-screencopy-unstable-v1-client-protocol.h \
	       wlr-screencopy-unstable-v1-client-protocol.c \
	       wlr-layer-shell-unstable-v1-client-protocol.h \
	       wlr-layer-shell-unstable-v1-client-protocol.c \
	       fractional-scale-v1-client-protocol.h \
	       fractional-scale-v1-client-protocol.c \
	       viewporter-client-protocol.h \
	       viewporter-client-protocol.c \
	       xdg-shell-client-protocol.h \
	       xdg-shell-client-protocol.c

