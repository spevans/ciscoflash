#
# Makefile for the linux Cisco Flash FileSystem routines.
#
# $Id: Makefile,v 1.1 2002-05-21 13:50:35 spse Exp $
#

#ciscoffs-objs := ciscoffs.o
#
#O_TARGET := ciscoffs.o
#
#obj-y := $(ciscoffs-objs)
#obj-m := $(O_TARGET)
#
#include $(TOPDIR)/Rules.make



ciscoffs.o: inode.c
	gcc -Wall -O2 -DMODULE -D__KERNEL__ -DLINUX -c inode.c -I/home/spse/src/linux-mtd/include -o ciscoffs.o
                                                          