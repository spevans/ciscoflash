/* 
 * $Id: infoblock.h,v 1.1 2002-05-21 13:50:35 spse Exp $
 *
 * Info Block on Cisco flash card
 *
 * Simon Lockhart Feb 2001
 */

#define CISCO_IB_MAGIC 0x06887635
#define CISCO_IB_MAGIC_SWAP 0x88063576

typedef struct {
	int	magic;		/* 0x06887635 */
	int	length;		/* Size of card */
	int	sectorsize;	/* ??? = 0x00020000 */
	int	prog_mode;	/* Programming Algo = 4 */
	int	erasestate;	/* What erased blocks are set to */
	int	filesysver;	/* 0x00010000 */
	int	fsoffset;	/* Where filesystem starts on flash */
	int	fslength;	/* How big the filesystem is */
	int	monliboffset;	/* Where MONLIB is */
	int	monliblength;	/* How big MONLIB is */
	int	unk2;		/* 0 */
	int	badsecoffset;	/* Where Bad Sector Map is */
	int	badseclength;	
	int	squeezelogoffset; /* Where Squeeze Log is */
	int	squeezeloglength;
	int	squeezebufoffset; /* Where Squeeze Buffer is */
	int	squeezebuflength;
	char	slotname[48];	/* Seems to be name of slot was formatted in */
        char	pad[256-116];
} ciscoflash_infoblock;


	
