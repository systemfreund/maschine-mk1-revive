# mk1-linux: Maschine MK1 userspace MIDI driver for Linux
#   make            build ./mk1-linux
#   make test       run the protocol unit tests (no hardware needed)
#   make install    install binary, udev rule and systemd user unit

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wshadow -Isrc
PKGS     = libusb-1.0 alsa
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

PREFIX  ?= /usr/local
DESTDIR ?=

CORE_SRC = src/mk1_input.c src/mk1_leds.c src/mk1_display.c src/mk1_mackie.c
APP_SRC  = src/main.c src/mk1_usb.c src/mk1_midi.c $(CORE_SRC)
HEADERS  = $(wildcard src/*.h)

all: mk1-linux

mk1-linux: $(APP_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(APP_SRC) $(PKG_LIBS) -lpthread -lm

tests/test_protocol: tests/test_protocol.c $(CORE_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ tests/test_protocol.c $(CORE_SRC)

test: tests/test_protocol
	./tests/test_protocol

install: mk1-linux
	install -Dm755 mk1-linux $(DESTDIR)$(PREFIX)/bin/mk1-linux
	install -Dm644 packaging/70-maschine-mk1.rules $(DESTDIR)/etc/udev/rules.d/70-maschine-mk1.rules
	install -Dm644 packaging/mk1-linux.service $(DESTDIR)$(PREFIX)/lib/systemd/user/mk1-linux.service

clean:
	rm -f mk1-linux tests/test_protocol

.PHONY: all test install clean
