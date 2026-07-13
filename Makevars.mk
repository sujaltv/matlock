# matlock - Matrix Lock
# See LICENCE file for copyright and licence details.

# matlock version and binary name
VERSION = 1.3.2
RELEASE = 0
ARCH=x86_64
BIN_FILE = matlock

# build and installation directories (absolute paths)
BUILD_DIR = $(TMPDIR)/$(BIN_FILE)
__RELEASE_FILE = $(BIN_FILE)-v$(VERSION)-$(RELEASE)-$(ARCH)
__RELEASE_DIR = $(TMPDIR)/$(__RELEASE_FILE)
PREFIX = /usr
SYSCONFDIR = /etc

# external libraries
X11INC = -I /usr/include/X11
X11LIB = -L /usr/lib/X11 -l X11 -l Xext -l Xrandr
GENINC = -I /usr/include
GENLIB = -L /usr/lib -l c
CRYPTLIB = -l crypt

# wayland libraries and protocol sources
WLPKGS    = wayland-client xkbcommon freetype2 fontconfig
WLLIB     = $(shell pkg-config --libs $(WLPKGS))
WLINC     = $(shell pkg-config --cflags $(WLPKGS))
WLPROTO   = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER   = $(shell pkg-config --variable=wayland_scanner wayland-scanner)
LOCK_XML  = $(WLPROTO)/staging/ext-session-lock/ext-session-lock-v1.xml
PROTO_DIR = $(BUILD_DIR)/proto

# includes and libs
INCS = -I . $(GENINC) $(X11INC) $(WLINC) -I $(PROTO_DIR)
LIBS = $(GENLIB) $(X11LIB) $(CRYPTLIB) $(WLLIB)

# flags
CPPFLAGS = -D _DEFAULT_SOURCE \
		   -D HAVE_SHADOW_H \
		   -D NAME=\"$(BIN_FILE)\" \
		   -D VERSION=\"$(VERSION)\"
CFLAGS   = -std=c++20 \
		   -Wno-pedantic \
		   -Wall \
		   -O2 -flto=auto \
		   $(INCS) \
		   $(CPPFLAGS)

LDFLAGS = -s ${LIBS}

# compiler and linker
CC = g++
