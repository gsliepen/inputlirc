SBIN = inputlircd
MAN8 = inputlircd.8

CC ?= gcc
CFLAGS ?= -Wall -g -O2 -pipe
PREFIX ?= /usr
INSTALL ?= install
SBINDIR ?= $(PREFIX)/sbin
MANDIR ?= $(SHAREDIR)/man

all: $(SBIN)

names.h: /usr/include/linux/input.h gennames
	./gennames $< > $@

inputlircd: inputlircd.c input.h names.h
	$(CC) $(CFLAGS) -o $@ $<

install: install-sbin install-man

install-sbin: $(SBIN)
	mkdir -p $(DESTDIR)$(SBINDIR)
	$(INSTALL) $(SBIN) $(DESTDIR)$(SBINDIR)/

install-man: $(MAN1) $(MAN5) $(MAN8)
	mkdir -p $(DESTDIR)$(MANDIR)/man8/
	$(INSTALL) -m 644 $(MAN8) $(DESTDIR)$(MANDIR)/man8/

clean:
	rm -f $(SBIN) names.h
