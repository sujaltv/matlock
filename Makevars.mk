# matlock - Matrix Lock
# See LICENCE file for copyright and licence details.

# matlock version and binary name
VERSION = 0.0.0
BIN_FILE = matlock

# build and installation directories (absolute paths)
BUILD_DIR = /tmp/$(BIN_FILE)
RELEASE_DIR = /tmp/$(BIN_FILE)-v$(VERSION)
PREFIX = /usr/local
MAN_DIR = /usr/local/share/man
SHARES_DIR = $(PREFIX)/share/$(BIN_FILE)

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
CFLAGS   = -std=c99 \
		   -pedantic \
		   -Wall \
		   -Os \
		   $(INCS) \
		   $(CPPFLAGS)

LDFLAGS = -s ${LIBS}

# compiler and linker
CC = gcc
