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

#include "../fs/fileheader.h"

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


int main(int argc, char **argv)
{
	char *device = "/dev/mtd/0";
	int fd = -1;
	struct stat sinfo;
	uint32_t *magic;
	char buf[sizeof(ciscoflash_filehdr_ext)];
	char *p;
	int eof = 0;
	
	if(argc > 2 && !strcmp(*(argv+1), "-d"))
	   device = *(argv+2);

	printf("Using device %s\n", device);
	
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
	

 error:
	if(fd != -1)
		close(fd);
	return 0;
}
