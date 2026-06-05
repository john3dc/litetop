# litetop - minimal terminal system monitor
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

all: litetop

litetop: litetop.c
	$(CC) $(CFLAGS) -o $@ litetop.c $(LDFLAGS)

# Smallest possible binary (strip + size opt). Try musl for a static build:
#   make small CC=musl-gcc LDFLAGS=-static
small: litetop.c
	$(CC) -Os -s -Wall -o litetop litetop.c $(LDFLAGS)

install: litetop
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 litetop $(DESTDIR)$(BINDIR)/litetop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/litetop

clean:
	rm -f litetop

.PHONY: all small install uninstall clean
