/*
 * $Id: fileheader.h,v 1.2 2002-07-03 21:59:31 spse Exp $
 *
 * Structure for file header on Cisco flash card
 *
 * Simon Lockhart February 2001
 */

#define CISCO_FH_MAGIC 0xBAD00B1E
#define CISCO_FH_EXT_MAGIC 0x07158805


typedef struct {
	uint32_t	magic;		/* 0x07178805 */
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
} ciscoflash_filehdr_ext;



typedef struct {
	uint32_t	magic;		/* 0xBAD00B1E */
	uint32_t	length;		/* file length in bytes */
	uint16_t	chksum;		/* Chksum */
	uint16_t	flags;
	uint32_t	date;		/* Unix date format */
	char		name[48];	/* filename */
} ciscoflash_filehdr;
