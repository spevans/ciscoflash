/*
 * $Id: fileheader.h,v 1.1 2002-05-21 13:50:35 spse Exp $
 *
 * Structure for file header on Cisco flash card
 *
 * Simon Lockhart February 2001
 */

#define CISCO_FH_EXT_MAGIC 0x07158805
#define CISCO_FH_EXT_MAGIC_SWAP 0x15070588

typedef struct {
	int	magic;		/* 0x07178805 */
	int	filenum;	/* 0x00000001 */
	char	name[64];	/* filename */
	int	length;		/* length in bytes */
	int	seek;		/* location of next file */
	int	crc;		/* File CRC */
	int	type;		/* ? 1 = config, 2 = image */
	int	date;		/* Unix type format */
	int	unk;
	int	flag1;		/* 0xFFFFFFF8 */
	int	flag2;		/* 0xFFFEFFFF is deleted file, all F's isn't */
	char	pad[128-104];
} ciscoflash_filehdr_ext;

#define CISCO_FH_MAGIC 0xBAD00B1E
#define CICSO_FH_MAGIC_SWAP 0xD0BA1E0B

typedef struct {
	int	magic;		/* 0xBAD00B1E */
	int	length;		/* file length in bytes */
	short	crc;		/* CRC16 */
	short	flags;
	int	date;		/* Unix date format */
	char	name[48];	/* filename */
} ciscoflash_filehdr;
