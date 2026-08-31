CC           ?= cc
CFLAGS       ?= -Os -Wall -Wextra
LDLIBS        = -lX11
PREFIX       ?= /usr/local
XSESSIONSDIR ?= /usr/share/xsessions

all: floe

floe: floe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f floe

install: floe
	install -Dm755 floe $(DESTDIR)$(PREFIX)/bin/floe
	install -Dm644 floe.desktop $(DESTDIR)$(XSESSIONSDIR)/floe.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/floe
	rm -f $(DESTDIR)$(XSESSIONSDIR)/floe.desktop

.PHONY: all clean install uninstall
