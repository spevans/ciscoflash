# $Id: Makefile,v 1.2 2002-07-04 15:22:57 spse Exp $

VERSION := 0.03

cffs: cffs.c fileheader.h
	$(CC) -Wall -ggdb -DVERSION="\"${VERSION}\"" -o cffs cffs.c

tgz:
	rm -rf cffs-${VERSION}
	mkdir cffs-${VERSION}
	cp Makefile cffs.c fileheader.h cffs-${VERSION}
	tar zcvf cffs-${VERSION}.tgz cffs-${VERSION}

clean:
	rm -rf *.o *~ cffs cffs-*