#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
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


void swap_header(ciscoflash_filehdr *h)
{
	h->length = ntohl(h->length);
	h->chksum = ntohs(h->chksum);
	h->flags = ntohs(h->flags);
	h->date = ntohl(h->date);
}


void dump_header(ciscoflash_filehdr *h, uint16_t chk)
{
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
		*device = NULL;
		fprintf(stderr, "Error: no device specified\n");
		return bad_options;
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

		default:
			return bad_options;
			break;
		}
	}

	/* default option */

	if(option == none)
		option = dir;

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
	uint32_t *magic;
	char buf[sizeof(ciscoflash_filehdr_ext)];
	char *p;
	int eof = 0;
	enum options options;
	int filecnt;
	char **files;
			
	options = parse_opts(argc, argv, &device, &filecnt, &files);

	if(options == bad_options)
		exit(1);

	if(device)
		printf("device = %s\n", device);

	if(filecnt) {
		printf("File args: ");
		while(filecnt--)
			printf("%s ", *(files++));
		printf("\n");
	}
	
	fd = open(device, O_RDONLY);
	if(fd == -1) {
		fprintf(stderr, "cant open %s: %s\n", device, strerror(errno));
		exit(1);
	}
	if(fstat(fd, &sinfo) == -1) {
		fprintf(stderr, "cant stat %s: %s\n", device, strerror(errno));
		close(fd);
		exit(1);
	}
	lseek(fd, 0, 0);

	while(!eof && read(fd, &buf, sizeof(magic)) == sizeof(magic)) {
		int len;
		magic = (uint32_t *)buf;
		*magic = ntohl(*magic);

		switch(*magic) {
		case CISCO_FH_MAGIC:
			len = sizeof(ciscoflash_filehdr) - sizeof(magic);
			break;

		case CISCO_FH_EXT_MAGIC:
			len = sizeof(ciscoflash_filehdr_ext) - sizeof(magic);
			break;

		case 0xffffffff:
			printf("End of filesystem\n");
			eof = 1;
			continue;
		}

		if(read(fd, buf+sizeof(*magic), len) == len) {
			if(*magic == CISCO_FH_MAGIC) {
				ciscoflash_filehdr *hdr = (ciscoflash_filehdr *)buf;
				swap_header(hdr);
				
				p = malloc(hdr->length+1);
				memset(p, 0, hdr->length+1);
				if(!p)
					goto error;
				if(read(fd, p, hdr->length) != hdr->length)
					goto error;
				dump_header(hdr, calc_chk16(p, hdr->length));
				free(p);
				lseek(fd, ((hdr->length +3) & ~3) - hdr->length, SEEK_CUR);
						
			}
			else if(*magic == CISCO_FH_EXT_MAGIC)
				dump_header_ext((ciscoflash_filehdr_ext *)buf);
		}
	}
	
	close(fd);
	exit(0);

 error:
	if(fd != -1)
		close(fd);
	exit(1);
}
