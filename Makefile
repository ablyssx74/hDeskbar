# hDeskbar: the SnakeTracker Deskbar for Wayland.
#
#   make                build ./hdeskbar
#   sudo make install   install into $(PREFIX) (default /usr)
#
# Requires (Arch / CachyOS package names): wayland cairo pango librsvg libpulse curl

CXX      ?= g++
PREFIX   ?= /usr
DESTDIR  ?=
TARGET    = hdeskbar

PROTO_DIR = protocols
GEN_DIR   = build

PROTOCOLS = xdg-shell wlr-layer-shell-unstable-v1 plasma-window-management wlr-foreign-toplevel-management-unstable-v1 viewporter fractional-scale-v1 xdg-decoration-unstable-v1

PKGS = wayland-client cairo pangocairo librsvg-2.0 gdk-pixbuf-2.0 gio-unix-2.0 libpulse libpulse-mainloop-glib libcurl

PROTO_HEADERS = $(patsubst %,$(GEN_DIR)/%-client-protocol.h,$(PROTOCOLS))
PROTO_SOURCES = $(patsubst %,$(GEN_DIR)/%-protocol.c,$(PROTOCOLS))
PROTO_OBJECTS = $(PROTO_SOURCES:.c=.o)

APP_VERSION := $(shell tr -d ' \r\n' < VERSION)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-unused-parameter -I$(GEN_DIR) -DAPP_VERSION='"$(APP_VERSION)"' \
            $(shell pkg-config --cflags $(PKGS))
CFLAGS   ?= -O2
LIBS      = $(shell pkg-config --libs $(PKGS)) -pthread

SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(patsubst src/%.cpp,$(GEN_DIR)/%.o,$(SOURCES))

all: $(TARGET)

$(GEN_DIR):
	mkdir -p $@

$(GEN_DIR)/%-client-protocol.h: $(PROTO_DIR)/%.xml | $(GEN_DIR)
	wayland-scanner client-header $< $@

$(GEN_DIR)/%-protocol.c: $(PROTO_DIR)/%.xml | $(GEN_DIR)
	wayland-scanner private-code $< $@

$(GEN_DIR)/%-protocol.o: $(GEN_DIR)/%-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

# the icons are compiled in as byte arrays (od + sed: no xxd needed)
$(GEN_DIR)/icon-data.h: data/snaketracker.svg | $(GEN_DIR)
	{ echo 'static const unsigned char kSnakeTrackerSvg[] = {'; \
	  od -An -v -tx1 $< | sed 's/\([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; } > $@

$(GEN_DIR)/about-icon-data.h: data/snaketracker-about.svg | $(GEN_DIR)
	{ echo 'static const unsigned char kAboutSvg[] = {'; \
	  od -An -v -tx1 $< | sed 's/\([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; } > $@

$(GEN_DIR)/%.o: src/%.cpp $(PROTO_HEADERS) $(GEN_DIR)/icon-data.h $(GEN_DIR)/about-icon-data.h | $(GEN_DIR)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

$(TARGET): $(OBJECTS) $(PROTO_OBJECTS)
	$(CXX) -o $@ $^ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/hdeskbar
	sed 's|@BINDIR@|$(PREFIX)/bin|g' data/hdeskbar.desktop.in > $(GEN_DIR)/hdeskbar.desktop
	install -Dm644 $(GEN_DIR)/hdeskbar.desktop $(DESTDIR)$(PREFIX)/share/applications/hdeskbar.desktop
	install -Dm644 data/snaketracker.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/hdeskbar.svg

clean:
	rm -rf $(GEN_DIR) $(TARGET)

-include $(OBJECTS:.o=.d)
.PHONY: all install clean
