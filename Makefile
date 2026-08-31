CC     ?= cc
CFLAGS ?= -Os -Wall -Wextra
LDLIBS  = -lX11
PREFIX ?= /usr/local

all: floe

floe: floe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f floe

install: floe
	install -Dm755 floe $(DESTDIR)$(PREFIX)/bin/floe

.PHONY: all clean install
