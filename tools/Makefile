# $Id: Makefile,v 1.3 2002-07-04 16:32:38 spse Exp $

VERSION := 0.03

all: cffs

cffs: cffs.c fileheader.h
	$(CC) -Wall -O2 -DVERSION="\"${VERSION}\"" -o cffs cffs.c

debug: cffs.c fileheader.h
	$(CC) -Wall -g -DVERSION="\"${VERSION}\"" -o cffs cffs.c

tgz:
	rm -rf cffs-${VERSION}
	mkdir cffs-${VERSION}
	cp Makefile cffs.c fileheader.h COPYING README cffs-${VERSION} 
	tar zcvf cffs-${VERSION}.tgz cffs-${VERSION}

clean:
	rm -rf *.o *~ cffs cffs-*