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

#if 0
#define POLY (1<<12 | 1<< 5 | 1)
uint16_t calc_crc16(uint8_t *buf, int len, uint16_t wcrc)
{
	uint16_t crc = 0xffff;
	uint8_t byte, bit;
	int idx;
	uint16_t poly;
	uint8_t *p = buf;

	for(poly = 0; poly != 0xffff; poly++) {
		buf = p;
		for (idx = 0; idx < len; idx++) {
			for (byte = *buf++, bit = 0; bit < 8; bit++, byte >>= 1)
				crc = (crc >> 1) ^ (((crc ^ byte) & 1) ? poly : 0);
		}
		if(crc == ~wcrc) {
			printf("Found CRC with poly = 0x%4.4X\n", (uint16_t)poly);
			//return crc;
		}
	}
	return 0;
}
#else

uint16_t calc_crc16(uint8_t *buf, int len, uint16_t wcrc)
{
	uint32_t crc = 0;
	uint16_t d, *data;
	
	data = (uint16_t *)buf;

	printf("len = %d\n", len);
	while(len & ~1) {
		d = ~ntohs(*(data++));
		crc += (uint16_t)d;
		crc = (crc & 0xffff) + (crc >> 16);
		len -= 2;
	}
#if 1
	if(len) {
		crc += (uint16_t)~(*buf << 8);
/*
		d = (*buf << 8) & 0xffff;
		printf("Handling odd byte crc = 0x%8.8X d=0x%4.4X, ~d=0x%4.4X\n", crc, d, ~d);
		
		//d &= 0xff;
		crc += (uint16_t)~d;
*/
		crc = (crc & 0xffff) + (crc >> 16);
	}
#endif
		
	//printf("crc = 0x%8.8X, ~crc = 0x%8.8X\n", crc, ~crc);
	return (uint16_t)crc;
}
#endif

void swap_header(ciscoflash_filehdr *h)
{
	h->length = ntohl(h->length);
	h->crc = ntohs(h->crc);
	h->flags = ntohs(h->flags);
	h->date = ntohl(h->date);
}


void dump_header(ciscoflash_filehdr *h)
{
	time_t t = (time_t)h->date;
	printf("magic: 0x%8.8X length: %u crc: 0x%4.4X flags: 0x%4.4X\n",
	       h->magic, h->length, (uint16_t)h->crc, (uint16_t)h->flags);
	printf("date: %s name: %s\n", ctime(&t), h->name);
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

	while(read(fd, &buf, sizeof(magic)) == sizeof(magic)) {
		int len;
		magic = (uint32_t *)buf;
		*magic = ntohl(*magic);
		if(*magic == CISCO_FH_MAGIC || ntohl(*magic) == CISCO_FH_MAGIC)
			len = sizeof(ciscoflash_filehdr) - sizeof(magic);
		else if(*magic == CISCO_FH_EXT_MAGIC || *magic == CISCO_FH_EXT_MAGIC_SWAP)
			len = sizeof(ciscoflash_filehdr_ext) - sizeof(magic);
		else {
			fprintf(stderr, "Bad magic: 0x%8.8X\n", *magic);
			goto error;
		}
		if(read(fd, buf+sizeof(*magic), len) == len) {
			uint16_t crc;
			if(*magic == CISCO_FH_MAGIC || ntohl(*magic)== CISCO_FH_MAGIC) {
				ciscoflash_filehdr *hdr = (ciscoflash_filehdr *)buf;
				//if(ntohl(*magic)== CISCO_FH_MAGIC)
					swap_header(hdr);
				dump_header(hdr);
				p = malloc(hdr->length+3);
				if(!p)
					goto error;
				memset(p, 0, hdr->length+3);
				if(read(fd, p, hdr->length) != hdr->length)
					goto error;
				crc = (uint16_t)calc_crc16(p, hdr->length, hdr->crc);
				if(crc != hdr->crc)
					printf("Bad CRC 0x%4.4X != 0x%4.4X\n", crc, hdr->crc);
				free(p);
				printf("\n\n");
				lseek(fd, ((hdr->length +3) & ~3) - hdr->length, SEEK_CUR);
						
			}
			else if(*magic == CISCO_FH_EXT_MAGIC || *magic == CISCO_FH_EXT_MAGIC_SWAP)
				dump_header_ext((ciscoflash_filehdr_ext *)buf);
		}
	}
	

 error:
	if(fd != -1)
		close(fd);
	return 0;
}
