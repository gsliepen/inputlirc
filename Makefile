# inputlircd -- zeroconf LIRC daemon that reads from /dev/input/event devices
# Copyright (C) 2006  Guus Sliepen <guus@sliepen.eu.org>
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of version 2 of the GNU General Public License as published
# by the Free Software Foundation.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

SBIN = inputlircd
MAN8 = inputlircd.8

CC ?= gcc
CFLAGS ?= -Wall -g -O2 -pipe
PREFIX ?= /usr/local
INSTALL ?= install
SBINDIR ?= $(PREFIX)/sbin
SHAREDIR ?= $(PREFIX)/share
MANDIR ?= $(SHAREDIR)/man

all: $(SBIN)

names.h: /usr/include/linux/input.h gennames
	./gennames $< > $@

inputlircd: inputlircd.c /usr/include/linux/input.h names.h
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
