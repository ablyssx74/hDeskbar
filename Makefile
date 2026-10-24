# hDeskbar: the SnakeTracker Deskbar for Wayland.
#
#   make                build ./hdeskbar
#   sudo make install   install into $(PREFIX) (default /usr)
#
# Requires (Arch / CachyOS package names): wayland cairo pango

CXX      ?= g++
PREFIX   ?= /usr
DESTDIR  ?=
TARGET    = hdeskbar

PROTO_DIR = protocols
GEN_DIR   = build

PROTOCOLS = xdg-shell wlr-layer-shell-unstable-v1

PKGS = wayland-client cairo pangocairo

PROTO_HEADERS = $(patsubst %,$(GEN_DIR)/%-client-protocol.h,$(PROTOCOLS))
PROTO_SOURCES = $(patsubst %,$(GEN_DIR)/%-protocol.c,$(PROTOCOLS))
PROTO_OBJECTS = $(PROTO_SOURCES:.c=.o)

APP_VERSION := $(shell tr -d ' \r\n' < VERSION)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-unused-parameter -I$(GEN_DIR) -DAPP_VERSION='"$(APP_VERSION)"' \
            $(shell pkg-config --cflags $(PKGS))
CFLAGS   ?= -O2
LIBS      = $(shell pkg-config --libs $(PKGS))

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

$(GEN_DIR)/%.o: src/%.cpp $(PROTO_HEADERS) | $(GEN_DIR)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

$(TARGET): $(OBJECTS) $(PROTO_OBJECTS)
	$(CXX) -o $@ $^ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/hdeskbar

clean:
	rm -rf $(GEN_DIR) $(TARGET)

-include $(OBJECTS:.o=.d)
.PHONY: all install clean
