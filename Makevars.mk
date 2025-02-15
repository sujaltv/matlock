# matlock - Matrix Lock
# See LICENCE file for copyright and licence details.

# matlock version and binary name
VERSION = 1.0.0
RELEASE = 5
ARCH=x86_64
BIN_FILE = matlock

# build and installation directories (absolute paths)
BUILD_DIR = $(TMPDIR)/$(BIN_FILE)
__RELEASE_FILE = $(BIN_FILE)-v$(VERSION)-$(RELEASE)-$(ARCH)
__RELEASE_DIR = $(TMPDIR)/$(__RELEASE_FILE)
PREFIX = /usr

# external libraries
X11INC = -I /usr/include/X11
X11LIB = -L /usr/lib/X11 -l X11 -l Xext -l Xrandr
GENINC = -I /usr/include
GENLIB = -L /use/lib -l c
CRYPTLIB = -l crypt

# includes and libs
INCS = -I . $(GENINC) $(X11INC)
LIBS = $(GENLIB) $(X11LIB) $(CRYPTLIB)

# flags
CPPFLAGS = -D _DEFAULT_SOURCE \
		   -D HAVE_SHADOW_H \
		   -D NAME=\"$(BIN_FILE)\" \
		   -D VERSION=\"$(VERSION)\"
CFLAGS   = -std=c++20 \
		   -Wno-pedantic \
		   -Wall \
		   -Os \
		   $(INCS) \
		   $(CPPFLAGS)

LDFLAGS = -s ${LIBS}

# compiler and linker
CC = g++
