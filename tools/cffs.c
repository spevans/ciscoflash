/*
 * $Id: cffs.c,v 1.18 2002-07-07 14:39:19 spse Exp $
 *
 * cffs - cisco flash filesystem tool
 *
 * Copyright (C) 2002 Simon Evans (spse@secret.org.uk)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Please see the file COPYING for more details
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h> 
#include <sys/ioctl.h>

#include <linux/kdev_t.h>
#include <linux/mtd/mtd.h>


#define _GNU_SOURCE
#ifdef HAVE_GETOPT_LONG
# include <getopt.h>
#else
# include "getopt.h"
#endif


#include "fileheader.h"


#define COPYRIGHT "(C) Simon Evans 2002 (spse@secret.org.uk)"

enum options {	none = 0, bad_options, dir, delete, erase, get, put, fsck, help, version };
	


// Used by getopt
extern char *optarg;
extern int optind, opterr, optopt;  



/* 16bit check sum calculation */
uint16_t calc_chk16(uint8_t *buf, int len)
{
	uint32_t chk = 0;
	uint16_t d, *data;
	
	data = (uint16_t *)buf;

	while(len & ~1) {
		d = ~ntohs(*(data++));
		chk += (uint16_t)d;
		chk = (chk & 0xffff) + (chk >> 16);
		len -= 2;
	}

	if(len) {
		chk += (uint16_t)~(*data << 8);
		chk = (chk & 0xffff) + (chk >> 16);
	}
	return (uint16_t)chk;
}


int file_match(int filecnt, char **files, struct cffs_hdr *header)
{
	char *name;
	if(!filecnt)
		return 0;

	name = header->magic == CISCO_CLASSB ? header->hdr.cbfh.name : header->hdr.cafh.name;

	while(filecnt--) {
		if(!fnmatch(*files, name, 0))
			return 0;
	}
	return 1;
}

int confirm_action(char *action)
{
	int ch;

	if(action) {
		printf("Proceed with %s [Y/n]", action);
		fflush(stdout);
	}
	fflush(stdin);
	ch = getchar();
	fflush(stdin);
	if(ch == 'Y' || ch == 'y' || ch == '\n')
		return 1;
	if(action)
		printf("%s aborted\n", action);
	return 0;
}
	


char *read_file(int fd, struct cffs_hdr *header, int *filelen) 
{
	int len, hlen;
	char *buf;
	
	*filelen = 0;

	if(header->magic == CISCO_CLASSB) {
		len = header->hdr.cbfh.length;
		hlen = sizeof(struct cb_hdr);
	} else {
		len = header->hdr.cafh.length;
		hlen = sizeof(struct ca_hdr);
	}
	if(lseek(fd, header->pos+hlen, SEEK_SET) == -1) {
		perror("lseek: ");
		return NULL;
	}

	buf = malloc(len);
	if(!buf)
		return NULL;

	if(read(fd, buf, len) == -1) {
		perror("read: ");
		free(buf);
		return NULL;
	}
	*filelen = len;
	return buf;
}


int seek_next_header(int fd)
{
	off_t pos;

	pos = lseek(fd, 0, SEEK_CUR);
	if(pos == -1) {
		perror("lseek");
		return -1;
	}

	pos = (pos + 3) & ~3;
	if(lseek(fd, pos, SEEK_SET) == -1) {
		perror("lseek");
		return -1;
	}
	return 0;
}


int next_header_pos(int fd, struct cffs_hdr *header)
{
	off_t newpos = 0;

	if(header->magic == CISCO_CLASSB)
		newpos = sizeof(struct cb_hdr) + header->hdr.cbfh.length;
	else
		newpos = sizeof(struct ca_hdr) + header->hdr.cafh.length;

	newpos += header->pos;
	newpos = (newpos + 3) & ~3;
	if(lseek(fd, newpos, SEEK_SET) == -1) {
		perror("lseek: ");
		return -1;
	}
	return 0;
}
		

int read_header(int fd, struct cffs_hdr *header)
{
	char buf[sizeof(struct cffs_hdr)];

	memset(buf, 0, sizeof(struct cffs_hdr));
	header->pos = lseek(fd, 0, SEEK_CUR);
	if(header->pos == -1)
		return -1;

	if(read(fd, &buf, sizeof(header->magic)) < sizeof(header->magic))
		return -1;
	
	header->magic = ntohl(*(uint32_t *)buf);

	if(header->magic == CISCO_CLASSB) {
		int len = sizeof(struct cb_hdr) - sizeof(header->magic);
		if(read(fd, buf+4, len) < len)
			return -1;

		header->hdr.cbfh.magic = header->magic;
		header->hdr.cbfh.length = ntohl(*(uint32_t *)(buf+4));
		header->hdr.cbfh.chksum = ntohs(*(uint16_t *)(buf+8));
		header->hdr.cbfh.flags = ntohs(*(uint16_t *)(buf+10));
		header->hdr.cbfh.date = ntohl(*(uint32_t *)(buf+12));
		strncpy(header->hdr.cbfh.name, buf+16, 48);
		header->hdr.cbfh.name[47] = '\0';
		return 0;
	} else if(header->magic == CISCO_CLASSA) {
		int len = sizeof(struct ca_hdr) - sizeof(header->magic);
		if(read(fd, buf+4, len) < len)
			return -1;

		header->hdr.cafh.magic = header->magic;
		header->hdr.cafh.filenum = ntohl(*(uint32_t *)(buf+4));
		strncpy(header->hdr.cbfh.name, buf+8, 64);
		header->hdr.cbfh.name[63] = '\0';			
		header->hdr.cafh.length = ntohl(*(uint32_t *)(buf+72));
		header->hdr.cafh.seek = ntohl(*(uint32_t *)(buf+76));
		header->hdr.cafh.crc = ntohl(*(uint32_t *)(buf+80));
		header->hdr.cafh.type = ntohl(*(uint32_t *)(buf+84));
		header->hdr.cafh.date = ntohl(*(uint32_t *)(buf+88));
		header->hdr.cafh.unk = ntohl(*(uint32_t *)(buf+92));
		header->hdr.cafh.flag1 = ntohl(*(uint32_t *)(buf+96));
		header->hdr.cafh.flag2 = ntohl(*(uint32_t *)(buf+100));
		return 0;
	}
	return -1;
}


int write_header(int fd, struct cffs_hdr *header)
{
	char buf[sizeof(struct cffs_hdr)];
	int len = 0;

	memset(buf, 0, sizeof(struct cffs_hdr));

	if(header->magic == CISCO_CLASSB) {
		len = sizeof(struct cb_hdr);
		*(uint32_t *)(buf) = htonl(header->hdr.cbfh.magic);
		*(uint32_t *)(buf+4) = htonl(header->hdr.cbfh.length);
		*(uint16_t *)(buf+8) = htons(header->hdr.cbfh.chksum);
		*(uint16_t *)(buf+10) = htons(header->hdr.cbfh.flags);
		*(uint32_t *)(buf+12) = htonl(header->hdr.cbfh.date);
		memcpy(buf+16, header->hdr.cbfh.name, 48);
	} 
	else if(header->magic == CISCO_CLASSA) {
		len = sizeof(struct ca_hdr);
		*(uint32_t *)(buf) = htonl(header->hdr.cafh.magic);
		*(uint32_t *)(buf+4) = htonl(header->hdr.cafh.filenum);
		memcpy(buf+8, header->hdr.cafh.name, 64);
		*(uint32_t *)(buf+72) = htonl(header->hdr.cafh.length);
		*(uint32_t *)(buf+76) = htonl(header->hdr.cafh.seek);
		*(uint32_t *)(buf+80) = htonl(header->hdr.cafh.crc);
		*(uint32_t *)(buf+84) = htonl(header->hdr.cafh.type);
		*(uint32_t *)(buf+88) = htonl(header->hdr.cafh.date);
		*(uint32_t *)(buf+92) = htonl(header->hdr.cafh.unk);
		*(uint32_t *)(buf+96) = htonl(header->hdr.cafh.flag1);
		*(uint32_t *)(buf+100) = htonl(header->hdr.cafh.flag2);
		memset(buf+104, 0, sizeof(header->hdr.cafh.pad));
	}
	else return -1;
		
	if(lseek(fd, header->pos, SEEK_SET) == -1) {
		perror("lseek: ");
		return -1;
	}
	if(write(fd, &buf, len) != len) {
		perror("write: ");
		return -1;
	}

	return 0;
}
		



int put_file(int fd, char *fname, uint32_t magic)
{
	struct stat sinfo;
	int fd2 = -1;
	char *file = NULL;
	struct cffs_hdr header;


	header.magic = magic;
	header.pos = lseek(fd, 0, SEEK_CUR);
	if(header.pos == -1) {
		perror("lseek: ");
		return -1;
	}

	/* open the file */
	fd2 = open(fname, O_RDONLY);
	if(fd2 == -1) {
		fprintf(stderr, "Cant open %s: %s\n", fname, strerror(errno));
		return -1;
	}

	if(fstat(fd2, &sinfo) == -1) {
		fprintf(stderr, "Cant stat %s: %s\n", fname, strerror(errno));
		close(fd2);
		return -1;
	}

	/* read it in */
	file = malloc(sinfo.st_size);
	if(!file) {
		goto put_err;
	}

	if(read(fd2, file, sinfo.st_size) != sinfo.st_size) {
		fprintf(stderr, "Cant read in all of file %s\n", fname);
		goto put_err;
	}
	close(fd2);

	if(magic == CISCO_CLASSB) {
		time_t now;
		header.hdr.cbfh.magic = magic;
		header.hdr.cbfh.length = sinfo.st_size;
		header.hdr.cbfh.chksum = calc_chk16(file, sinfo.st_size);
		header.hdr.cbfh.flags = 0xFFFF & ~FLAG_HASDATE;
		time(&now);
		header.hdr.cbfh.date = now;
		memset(header.hdr.cbfh.name, 0, 48);
		strncpy(header.hdr.cbfh.name, fname, 48);
		header.hdr.cbfh.name[47] = '\0';
	} else {
		time_t now;
		time(&now);

		header.hdr.cafh.magic = magic;
		header.hdr.cafh.filenum = 1;
		memset(header.hdr.cafh.name, 0, 64);
		strncpy(header.hdr.cafh.name, fname, 64);
		header.hdr.cbfh.name[63] = '\0';
		header.hdr.cafh.length = sinfo.st_size;
		header.hdr.cafh.seek = header.pos+sizeof(struct ca_hdr);
		header.hdr.cafh.crc = 0;
		header.hdr.cafh.type = 1;
		header.hdr.cafh.date = now;
		header.hdr.cafh.unk = 0;
		header.hdr.cafh.flag1 = 0xfffffff8;
		header.hdr.cafh.flag2 = 0xffffffff;
	}
	
	if(write_header(fd, &header) == -1)
		goto put_err;

	if(write(fd, file, sinfo.st_size) != sinfo.st_size) {
		perror("write: ");
		goto put_err;
	}

	close(fd2);
	return 0;


 put_err:
	if(fd2 != -1)
		close(fd2);
	if(file)
		free(file);
	return -1;
}


int get_file(char *buf, int len, struct cffs_hdr *header)
{
	char *name = header->magic == CISCO_CLASSB ? header->hdr.cbfh.name : header->hdr.cafh.name;
	int fd;
	
	/* note - racy */
	if(!access(name, F_OK)) {
		printf("File %s exists, overwrite? [Y/n]", name);
		fflush(stdout);
		if(!confirm_action(NULL))
			return 0;
	}
	fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if(fd == -1) {
		fprintf(stderr, "Error opening %s for writing, %s\n", name, strerror(errno));
		return -1;
	}
	if(write(fd, buf, len) == -1) {
		fprintf(stderr, "Error writing to %s, %s\n", name, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}


int delete_file(int fd, struct cffs_hdr *header)
{
	uint16_t flag;
	off_t pos;

	if(header->magic == CISCO_CLASSB) {
		if(!(header->hdr.cbfh.flags &= ~FLAG_DELETED))
			return 0;
		header->hdr.cbfh.flags &= ~FLAG_DELETED;
		flag = htons(header->hdr.cbfh.flags);
		pos = header->pos + 10;
	} else {
		return -1;
	}
	if(lseek(fd, pos, SEEK_SET) == -1) {
		perror("lseek: ");
		return -1;
	}
	if(write(fd, &flag, sizeof(flag)) == -1) {
		perror("write: ");
		return -1;
	}
	
	if(lseek(fd, header->pos, SEEK_SET) == -1) {
		perror("lseek: ");
		return -1;
	}
	if(read_header(fd, header) == -1)
		return -1;

	if(!(header->hdr.cbfh.flags &= ~FLAG_DELETED))
		return 0;
	return -1;
}



void dump_header(struct cffs_hdr *header, uint16_t chk)
{
	struct cb_hdr *h = &header->hdr.cbfh;
	time_t t = (time_t)h->date;
	struct tm tm;
	char timebuf[16];

	localtime_r(&t, &tm);

	if(!(h->flags & FLAG_HASDATE))
		strftime(timebuf, 15, "%b %d %H:%M", &tm);
	else
		strcpy(timebuf, "  <no date> ");

	printf("%10d %s [%4.4X] [%4.4X] %s %s %s\n", h->length, timebuf, h->chksum, h->flags, h->name,
	       !(h->flags & FLAG_DELETED) ? "[deleted]" : "",
	       (chk != h->chksum) ? "[bad chksum]" : "");
}


void dump_header_ext(struct ca_hdr *h)
{
	time_t t = (time_t)h->date;
	printf("magic: 0x%8.8X filenum: 0x%8.8X name: %s\n", h->magic, h->filenum, h->name);
	printf("length: 0x%8.8X seek: 0x%8.8X crc: 0x%8.8X\n", h->length, h->seek, h->crc);
	printf("type: 0x%8.8X date = %s unk: 0x%8.8X\n", h->type, ctime(&t), h->unk);
	printf("flag1: 0x%8.8X flag2: 0x%8.8X\n", h->flag1, h->flag2);
}


int get_dev_info(int fd, struct mtd_info_user *mtd)
{
	if(ioctl(fd, MEMGETINFO, mtd) == -1) {
		perror("ioctl: MEMGETINFO: ");
		return -1;
	}
	return 0;
}	

int erase_device(int fd)
{
	int blocks, cnt;
	struct erase_info_user erase;
	struct mtd_info_user mtd;

	if(get_dev_info(fd, &mtd) == -1)
		return -1;

	printf("Size = %u erase size = %u\n", mtd.size, mtd.erasesize);
	if(!mtd.size)
		return -1;

	blocks =  mtd.size / mtd.erasesize;
	printf("%d Erase blocks\n", blocks);
	if(!confirm_action("erase"))
		return -1;

	erase.start = 0;
	erase.length = mtd.erasesize;
	for(cnt = 0; cnt < blocks; cnt++) {
		printf("\rErasing block %6d/%d", cnt+1, blocks);
		fflush(stdout);
		if(ioctl(fd, MEMERASE, &erase) == -1) {
			fprintf(stderr, "erase failed\n");
			return -1;
		} 
		erase.start += mtd.erasesize;
	}
	printf("\n");
	return 0;
		
}

void usage()
{
	printf("cffs - cisco flash file system reader\n");
	printf("Version " VERSION "  " COPYRIGHT"\n");
	printf("Usage: cffs <device> <option> [files...]\n");
	printf("\t<device>\tMTD Char device (eg /dev/mtd/0)\n");
	printf("\t-l, --dir\tList files\n");
	printf("\t-d, --delete\tDelete files\n");
	printf("\t-e, --erase\tErase flash\n");
	printf("\t-g, --get\tGet files from flash\n");
	printf("\t-p, --put\tPut files onto flash\n");
	printf("\t-f, --fsck\tCheck file system\n");
	printf("\t-h, --help\tUsage information\n");
	printf("\t-v, --version\tShow version\n");
}


enum options parse_opts(int argc, char **argv, char **device, int *filecnt, char ***files)
{
	static struct option long_options[] = {
		{"dir",		no_argument, NULL, 'l'},
		{"delete",	no_argument, NULL, 'd'},
		{"erase",	no_argument, NULL, 'e'},
		{"get",		no_argument, NULL, 'g'},
		{"put",		no_argument, NULL, 'p'},
		{"fsck",	no_argument, NULL, 'f'},
		{"help",	no_argument, NULL, 'h'},
		{"version",	no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};
	static char *short_opts = "+ldegpfhv";
	int a;
	enum options option = none;

	*files = NULL;
	*filecnt = 0;
	
	if(argc > 1 && **(argv+1) != '-') {
		*device = *(argv+1);
		argc--;
		argv++;
	} else {
		*device = NULL;		// set to "/dev/mtd/0" to set /dev/mtd/0 as default
	}



	while(1) {
		a = getopt_long(argc, argv, short_opts, long_options, NULL);
		if(a == -1)
			break;

		if(option != none) {
			fprintf(stderr, "Error: only one option can be specified\n");
			return bad_options;
		}
		
		switch(a) {
		case 'l':
			option = dir;
			break;

		case 'd':
			option = delete;
			break;
			
		case 'e':
			option = erase;
			break;

		case 'g':
			option = get;
			break;
			
		case 'p':
			option = put;
			break;

		case 'f':
			option = fsck;
			break;

		case 'h':
			option = help;
			break;

		case 'v':
			option = version;
			break;

		default:
			return bad_options;
			break;
		}
	}

	/* default option */

	if(option == none)
		option = dir;

	/* check if a device is specified for the options that need it */

	if(!*device && (option != help && option != version)) {
		fprintf(stderr, "Error: no device specified\n");
		return bad_options;
	}
	/* check for any file names given */
	if(optind < argc) {
		*files = (argv+optind);
		*filecnt = argc - optind;
	}
	return option;
}		


int fsck_device(int fd)
{
	struct cffs_hdr header;
	int eof = 0;
	uint32_t def_magic = 0;
	struct mtd_info_user mtd;
	int to_check;
	uint8_t *blank;
	off_t curpos;
	int free_spc, tested = 0, cnt;

#define TEST_BUF_SZ (16<<10)

	if(get_dev_info(fd, &mtd) == -1)
		return -1;

	while(!eof && read_header(fd, &header) != -1) {
		int len;
		char *buf;

		if(header.magic == 0xffffffff) {
			eof = 1;
			continue;
		}
		if(!def_magic)
			def_magic = header.magic;

		buf = read_file(fd, &header, &len);
		if(buf == NULL)
			return -1;

		switch(header.magic) {
		case CISCO_CLASSB: {
			uint16_t chk = calc_chk16(buf, len);
			printf("[CRC %s] %s \n", (chk == header.hdr.cbfh.chksum) ? "OK " : "BAD",
			       header.hdr.cbfh.name);

			break;
		}

		default:
			fprintf(stderr, "Bad magic: 0x%8.8X\n", header.magic);
			free(buf);
			return -1;
		}
		
		free(buf);
		if(next_header_pos(fd, &header) == -1)
			return -1;
	}
	curpos = lseek(fd, -4, SEEK_CUR);
	if(curpos == -1) {
		perror("lseek: ");
		return -1;
	}
		
	/* Now check the rest of the flash is blank */
	free_spc = to_check = (mtd.size - curpos);
	printf("Free space = %d bytes\n", free_spc);
	blank = malloc(TEST_BUF_SZ);
	if(!blank) {
		perror("malloc: ");
		return -1;
	}
	while(to_check) {
		int len = (to_check > TEST_BUF_SZ) ? TEST_BUF_SZ : to_check;
		int red = read(fd, blank, len);
		if(red != len) {
			perror("read: ");
			free(blank);
			return -1;
		}
		for(cnt = 0; cnt < len; cnt++) {
			if(blank[cnt] != 0xff) {
				fprintf(stderr, "\nFlash is not blank\n");
				free(blank);
				return -1;
			}
		}
		tested += len;
		to_check -= len;
		printf("\rChecking free space is blank: %d%% ",
		       (100*(free_spc-to_check)) /free_spc);
	}
	printf("\nFlash is OK\n");
	free(blank);
	return 0;
}


int main(int argc, char **argv)
{
	char *device;
	int fd = -1;
	struct stat sinfo;
	struct cffs_hdr header;
	char *p;
	int eof = 0;
	enum options options;
	int filecnt;
	char **files;
	uint32_t def_magic = 0;
	int mode;
			
	options = parse_opts(argc, argv, &device, &filecnt, &files);

	if(options == bad_options)
		exit(1);

	switch(options) {
	case bad_options:
		exit(1);

	case help:
		usage();
		exit(1);
		
	case version:
		printf("cffs version " VERSION "  " COPYRIGHT "\n");
		exit(1);
		
	case none:
		exit(1);
		
	default:
		break;

	}
		
	/* Determine open mode */
	if(options == put || options == delete || options == erase)
		mode = O_RDWR;
	else
		mode = O_RDONLY;

	fd = open(device, mode);
	if(fd == -1) {
		fprintf(stderr, "Bad device %s: %s\n", device, strerror(errno));
		exit(1);
	}
	if(fstat(fd, &sinfo) == -1) {
		fprintf(stderr, "Cant stat %s: %s\n", device, strerror(errno));
		close(fd);
		exit(1);
	}

	/* Check it is an MTD char device */
	if(!S_ISCHR(sinfo.st_mode) || (MAJOR(sinfo.st_rdev) != MTD_CHAR_MAJOR)) {
		fprintf(stderr, "%s is not an MTD character device\n", device);
		close(fd);
		exit(1);
	}
	
	if(options == erase) {
		erase_device(fd);
	} else if(options == fsck) {
		fsck_device(fd);
	} else {
		while(!eof && read_header(fd, &header) != -1) {
			int len;
			if(header.magic == 0xffffffff) {
				printf("End of filesystem\n");
				eof = 1;
				continue;
			}
			if(!def_magic)
				def_magic = header.magic;

			if(!file_match(filecnt, files, &header)) {
				if(options == dir || options == get) {
					p = read_file(fd, &header, &len);
					if(!p)
						goto error;
					if(options == dir) {
						dump_header(&header, calc_chk16(p, len));
					} else {
						if(get_file(p, len, &header) == -1) {
							free(p);
							goto error;
						}
					}
					free(p);
				}
				if(options == delete) {
					if(delete_file(fd, &header) == -1)
						goto error;
				}
			}
			if(next_header_pos(fd, &header) == -1)
				goto error;
		}
	}

	
	if(options == put) {
		if(!def_magic)
			def_magic = CISCO_CLASSB;

		if(lseek(fd, -4, SEEK_CUR) == -1) {
			perror("lseek");
			goto error;
		}
		while(filecnt--) {
			printf("Adding file: %s\n", *(files));
			put_file(fd, *(files++), def_magic);
			if(seek_next_header(fd) == -1)
				goto error;
		}
	}


	close(fd);
	exit(0);

 error:
	if(fd != -1)
		close(fd);
	exit(1);
}
