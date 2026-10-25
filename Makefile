CC      = gcc
CXX     = g++
TARGET  = cwatch
HELPER  = cwatch-stream
SRCDIR  = src
BUILDDIR = build
PREFIX ?= /usr/local

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

PKGS = sdl2 SDL2_ttf SDL2_image libcurl json-c
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))
LT_CFLAGS  := $(shell pkg-config --cflags libtorrent-rasterbar)
LT_LIBS    := $(shell pkg-config --libs libtorrent-rasterbar)

CFLAGS   = -std=c11 -Wall -Wextra -Wno-format-truncation -O2 $(PKG_CFLAGS)
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 $(LT_CFLAGS)
LDFLAGS  = $(PKG_LIBS) -lpthread -lm

.PHONY: all clean install install-desktop

all: $(BUILDDIR) $(TARGET) $(HELPER)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(wildcard $(SRCDIR)/*.h) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(HELPER): stream/cwatch_stream.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LT_LIBS) -lpthread

clean:
	rm -rf $(BUILDDIR) $(TARGET) $(HELPER)

install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm755 $(HELPER) $(DESTDIR)$(PREFIX)/bin/$(HELPER)

install-desktop: install
	install -Dm644 cwatch.desktop $(DESTDIR)$(PREFIX)/share/applications/cwatch.desktop
