CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
PKG     := sdl2 SDL2_image

SDL_CFLAGS := $(shell pkg-config --cflags $(PKG))
SDL_LIBS   := $(shell pkg-config --libs $(PKG))

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

all: rv rv-msg

rv: rv.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS) -lm

rv-msg: rv-msg.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f rv rv-msg

install: all
	install -Dm755 rv $(DESTDIR)$(BINDIR)/rv
	install -Dm755 rv-msg $(DESTDIR)$(BINDIR)/rv-msg

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rv $(DESTDIR)$(BINDIR)/rv-msg

.PHONY: all clean install uninstall
