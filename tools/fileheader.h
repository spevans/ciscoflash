/*
 * $Id: fileheader.h,v 1.2 2002-07-07 14:39:19 spse Exp $
 *
 * Structure for file header on Cisco flash card
 *
 */

/* Magic numbers */
#define CISCO_CLASSA 0x07158805
#define CISCO_CLASSB 0xBAD00B1E


/* Class A file header */

struct ca_hdr {
	uint32_t	magic;		/* CISCO_CLASSA */
	uint32_t	filenum;	/* 0x00000001 */
	char		name[64];	/* filename */
	uint32_t	length;		/* length in bytes */
	uint32_t	seek;		/* location of next file */
	uint32_t	crc;		/* File CRC */
	uint32_t	type;		/* ? 1 = config, 2 = image */
	uint32_t	date;		/* Unix type format */
	uint32_t	unk;
	uint32_t	flag1;		/* 0xFFFFFFF8 */
	uint32_t	flag2;		/* 0xFFFEFFFF is deleted file, all F's isn't */
	uint8_t		pad[128-104];
};


/* Class B file header */

struct cb_hdr {
	uint32_t	magic;		/* CISCO_CLASSB */
	uint32_t	length;		/* file length in bytes */
	uint16_t	chksum;		/* Chksum */
	uint16_t	flags;
	uint32_t	date;		/* Unix date format */
	char		name[48];	/* filename */
};

/* Class B Flags */

#define FLAG_DELETED   1
#define FLAG_HASDATE   2


struct cffs_hdr {
	uint32_t magic;
	off_t pos;
	union {
		struct ca_hdr cafh;	/* Class A file */
		struct cb_hdr cbfh;	/* Class B file */
	} hdr;
};


