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
#include <linux/mtd/mtd.h>

#define _GNU_SOURCE
#ifdef HAVE_GETOPT_LONG
# include <getopt.h>
#else
# include "getopt.h"
#endif



#include "../fs/fileheader.h"


#define VERSION "0.01"


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

	name = header->magic == CISCO_FH_MAGIC ? header->hdr.cfh.name : header->hdr.ecfh.name;

	while(filecnt--) {
		if(!fnmatch(*files, name, 0))
			return 0;
	}
	return 1;
}


char *read_file(int fd, struct cffs_hdr *header, int *filelen) 
{
	int len, hlen;
	char *buf;
	
	*filelen = 0;

	if(header->magic == CISCO_FH_MAGIC) {
		len = header->hdr.cfh.length;
		hlen = sizeof(ciscoflash_filehdr);
	} else {
		len = header->hdr.ecfh.length;
		hlen = sizeof(ciscoflash_filehdr_ext);
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


int next_header_pos(int fd, struct cffs_hdr *header)
{
	off_t newpos = 0;

	if(header->magic == CISCO_FH_MAGIC)
		newpos = sizeof(ciscoflash_filehdr) + header->hdr.cfh.length;
	else
		newpos = sizeof(ciscoflash_filehdr_ext) + header->hdr.ecfh.length;

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

	if(header->magic == CISCO_FH_MAGIC) {
		int len = sizeof(ciscoflash_filehdr) - sizeof(header->magic);
		if(read(fd, buf+4, len) < len)
			return -1;

		header->hdr.cfh.length = ntohl(*(uint32_t *)(buf+4));
		header->hdr.cfh.chksum = ntohs(*(uint16_t *)(buf+8));
		header->hdr.cfh.flags = ntohs(*(uint16_t *)(buf+10));
		header->hdr.cfh.date = ntohl(*(uint32_t *)(buf+12));
		strncpy(header->hdr.cfh.name, buf+16, 48);
		header->hdr.cfh.name[47] = '\0';
		return 0;
	} else if(header->magic == CISCO_FH_EXT_MAGIC) {
		int len = sizeof(ciscoflash_filehdr_ext) - sizeof(header->magic);
		if(read(fd, buf+4, len) < len)
			return -1;

		header->hdr.ecfh.filenum = ntohl(*(uint32_t *)(buf+4));
		strncpy(header->hdr.cfh.name, buf+8, 64);
		header->hdr.cfh.name[63] = '\0';			
		header->hdr.ecfh.length = ntohl(*(uint32_t *)(buf+72));
		header->hdr.ecfh.seek = ntohl(*(uint32_t *)(buf+76));
		header->hdr.ecfh.crc = ntohl(*(uint32_t *)(buf+80));
		header->hdr.ecfh.type = ntohl(*(uint32_t *)(buf+84));
		header->hdr.ecfh.date = ntohl(*(uint32_t *)(buf+88));
		header->hdr.ecfh.unk = ntohl(*(uint32_t *)(buf+92));
		header->hdr.ecfh.flag1 = ntohl(*(uint32_t *)(buf+96));
		header->hdr.ecfh.flag2 = ntohl(*(uint32_t *)(buf+100));
		return 0;
	}
	return -1;
}


void dump_header(struct cffs_hdr *header, uint16_t chk)
{
	ciscoflash_filehdr *h = &header->hdr.cfh;
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
	//printf("magic: 0x%8.8X length: %u chk: 0x%4.4X flags: 0x%4.4X\n",
	//       h->magic, h->length, (uint16_t)h->chksum, (uint16_t)h->flags);
	//printf("date: %s name: %s\n", ctime(&t), h->name);
}


void dump_header_ext(ciscoflash_filehdr_ext *h)
{
	time_t t = (time_t)h->date;
	printf("magic: 0x%8.8X filenum: 0x%8.8X name: %s\n", h->magic, h->filenum, h->name);
	printf("length: 0x%8.8X seek: 0x%8.8X crc: 0x%8.8X\n", h->length, h->seek, h->crc);
	printf("type: 0x%8.8X date = %s unk: 0x%8.8X\n", h->type, ctime(&t), h->unk);
	printf("flag1: 0x%8.8X flag2: 0x%8.8X\n", h->flag1, h->flag2);
}


int confirm_action(char *action)
{
	int ch;
	printf("Proceed with %s [Y/n]", action);
	fflush(stdout);
	ch = getchar();
	if(ch == 'Y' || ch == 'y' || ch == '\n')
		return 1;
	printf("\n%s aborted\n", action);
	return 0;
}
	

int erase_device(int fd)
{
	int blocks, cnt;
	struct erase_info_user erase;
	struct mtd_info_user mtd;

	if(ioctl(fd, MEMGETINFO, &mtd) == -1) {
		perror("ioctl");
		return -1;
	}
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
	printf("Version " VERSION "\n");
	printf("Usage: cffs <device> <option> [files...]\n");
	printf("\t-l, --dir\t\tList files\n");
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
		{0, 0, 0, 0}
	};
	static char *short_opts = "+ldegpf";
	int a;
	enum options option = none;

	*files = NULL;
	*filecnt = 0;
	
	if(argc > 1 && **(argv+1) != '-') {
		*device = *(argv+1);
		argc--;
		argv++;
	} else {
		*device = "/dev/mtd/0";
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
		printf("cffs version " VERSION "\n");
		exit(1);
		
	case none:
		exit(1);
		
	default:
		break;

	}
		

	fd = open(device, O_RDONLY);
	if(fd == -1) {
		fprintf(stderr, "Bad device %s: %s\n", device, strerror(errno));
		exit(1);
	}
	if(fstat(fd, &sinfo) == -1) {
		fprintf(stderr, "Cant stat %s: %s\n", device, strerror(errno));
		close(fd);
		exit(1);
	}

	if(options == erase) {
		erase_device(fd);
	} else {
		while(!eof && read_header(fd, &header) != -1) {
			int len;
			if(header.magic == 0xffffffff) {
				printf("End of filesystem\n");
				eof = 1;
				continue;
			}
			p = read_file(fd, &header, &len);
			if(!p)
				goto error;
			if(!file_match(filecnt, files, &header))
				dump_header(&header, calc_chk16(p, len));
			free(p);
			if(next_header_pos(fd, &header) == -1)
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
