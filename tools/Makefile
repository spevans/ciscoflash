# $Id: Makefile,v 1.6 2002-07-11 12:37:06 spse Exp $

VERSION := 0.05
CFLAGS := -Wall -O2

INSTALL = /usr/bin/install -c
INSTALL_PROGRAM = ${INSTALL}
INSTALL_DATA = ${INSTALL} -m 644

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
mandir = ${prefix}/man
man1dir = $(mandir)/man1


all: cffs

cffs: cffs.c fileheader.h
	$(CC) $(CFLAGS) -DVERSION="\"${VERSION}\"" -o cffs cffs.c

install: cffs cffs.1
	$(INSTALL) -d $(bindir) $(man1dir)
	$(INSTALL_PROGRAM) cffs $(bindir)
	$(INSTALL_DATA) cffs.1 $(man1dir)

tgz:
	rm -rf cffs-${VERSION}
	mkdir cffs-${VERSION}
	cp Makefile cffs.c cffs.1 fileheader.h COPYING README cffs-${VERSION} 
	tar zcvf cffs-${VERSION}.tgz cffs-${VERSION}

clean:
	rm -rf *.o *~ cffs cffs-*
