/*
 * $Id: pcmcia_mtd.c,v 1.22 2002-06-30 15:54:42 spse Exp $
 *
 * pcmcia_mtd.c - MTD driver for PCMCIA flash memory cards
 *
 * Author: Simon Evans <spse@secret.org.uk>
 *
 * Copyright (C) 2002 Simon Evans
 *
 * Licence: GPL
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/system.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#include <linux/mtd/map.h>

#define CONFIG_MTD_DEBUG
#define CONFIG_MTD_DEBUG_VERBOSE 2


#ifdef CONFIG_MTD_DEBUG
static int debug = CONFIG_MTD_DEBUG_VERBOSE;
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Set Debug Level 0 = quiet, 5 = noisy");
#undef DEBUG
#define DEBUG(n, format, arg...) \
	if (n <= debug) {	 \
		printk(KERN_DEBUG __FILE__ ":" __FUNCTION__ "(): " format "\n", ## arg); \
	}

#else
#define DEBUG(n, arg...)
static const int debug = 0;
#endif

#define err(format, arg...) printk(KERN_ERR __FILE__ ": " format "\n" , ## arg)
#define info(format, arg...) printk(KERN_INFO __FILE__ ": " format "\n" , ## arg)
#define warn(format, arg...) printk(KERN_WARNING __FILE__ ": " format "\n" , ## arg)


#define DRIVER_DESC	"PCMCIA Flash memory card driver"
#define DRIVER_VERSION	"$Revision: 1.22 $"

/* Size of the PCMCIA address space: 26 bits = 64 MB */
#define MAX_PCMCIA_ADDR	0x4000000

#define PCMCIA_BYTE_MASK(x)  (x-1)
#define PCMCIA_WORD_MASK(x)  (x-2)


typedef struct memory_dev_t {
	struct list_head list;
	dev_link_t	link;		/* PCMCIA link */
	caddr_t		win_base;	/* ioremapped address of PCMCIA window */
	unsigned int	win_size;	/* size of window (usually 64K) */
	unsigned int	cardsize;	/* size of whole card */
	unsigned int	offset;		/* offset into card the window currently points at */
	unsigned int	memspeed;	/* memory access speed in ns */
	struct map_info	pcmcia_map;
	struct mtd_info	*mtd_info;
	char		mtd_name[sizeof(struct cistpl_vers_1_t)];
} memory_dev_t;


static dev_info_t dev_info = "pcmcia_mtd";
static LIST_HEAD(dev_list);

/* Module parameters */

/* 2 = do 16-bit transfers, 1 = do 8-bit transfers */
static int buswidth = 2;

/* Speed of memory accesses, in ns */
static int mem_speed;

/* Force the size of an SRAM card */
static int force_size;

/* Force Vcc */
static int vcc;

/* Force Vpp */
static int vpp;

/* Force card to be treated as FLASH, ROM or RAM */
static int mem_type;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Evans <spse@secret.org.uk>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_PARM(buswidth, "i");
MODULE_PARM_DESC(buswidth, "Set buswidth (1 = 8 bit, 2 = 16 bit, default = 2)");
MODULE_PARM(mem_speed, "i");
MODULE_PARM_DESC(mem_speed, "Set memory access speed in ns");
MODULE_PARM(force_size, "i");
MODULE_PARM_DESC(force_size, "Force size of card in MB (1-64)");
MODULE_PARM(vcc, "i");
MODULE_PARM_DESC(vcc, "Set Vcc in 1/10ths eg 33 = 3.3V 120 = 12V (Dangerous)");
MODULE_PARM(vpp, "i");
MODULE_PARM_DESC(vpp, "Set Vpp in 1/10ths eg 33 = 3.3V 120 = 12V (Dangerous)");
MODULE_PARM(mem_type, "i");
MODULE_PARM_DESC(mem_type, "Set Memory type (0 = Flash, 1 = RAM, 2 = ROM, default = 0)");



static void inline cs_error(client_handle_t handle, int func, int ret)
{
	error_info_t err = { func, ret };
	CardServices(ReportError, handle, &err);
}


/* Map driver */


static inline int remap_window(memory_dev_t *dev, window_handle_t win, unsigned long to)
{
	memreq_t mrq;
	int ret;

	mrq.CardOffset = to & ~PCMCIA_BYTE_MASK(dev->win_size);
	if(mrq.CardOffset != dev->offset) {
		DEBUG(2, "Remapping window from 0x%8.8x to 0x%8.8x",
		      dev->offset, mrq.CardOffset);
		mrq.Page = 0;
		if( (ret = CardServices(MapMemPage, win, &mrq)) != CS_SUCCESS) {
			cs_error(dev->link.handle, MapMemPage, ret);
			return -1;
		}
		dev->offset = mrq.CardOffset;
	}
	return 0;
}


static __u8 pcmcia_read8_remap(struct map_info *map, unsigned long ofs)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;
	__u8 d;

	if(remap_window(dev, win, ofs) == -1)
		return 0;

	d = readb((dev->win_base) + (ofs & PCMCIA_BYTE_MASK(dev->win_size)));
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x", ofs,
	      (dev->win_base)+(ofs & PCMCIA_BYTE_MASK(dev->win_size)), d);

	return d;
}


static __u16 pcmcia_read16_remap(struct map_info *map, unsigned long ofs)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;
	__u16 d;

	if(remap_window(dev, win, ofs) == -1)
		return 0;

	d = readw((dev->win_base)+(ofs & PCMCIA_BYTE_MASK(dev->win_size)));
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x", ofs,
	      (dev->win_base)+(ofs & PCMCIA_BYTE_MASK(dev->win_size)), d);

	return d;
}


static void pcmcia_copy_from_remap(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(3, "to = %p from = %lu len = %u", to, from, len);
	while(len) {
		int toread = dev->win_size - (from & PCMCIA_BYTE_MASK(dev->win_size));
		int tocpy;
		if(toread > len)
			toread = len;

		DEBUG(4, "from = 0x%8.8lx len = %u toread = %ld offset = 0x%8.8lx",
		      (long)from, (unsigned int)len, (long)toread, from & ~PCMCIA_BYTE_MASK(dev->win_size));

		if(remap_window(dev, win, from) == -1)
			return;

		// Handle odd byte on 16bit bus
		if((from & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "reading word from %p", dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)));
			data = readw(dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)));
			*(__u8 *)to = (__u8)(data >> 8);
			to++;
			from++;
			len--;
			toread--;
		}
		tocpy = toread;
		if(map->buswidth == 2)
			tocpy &= PCMCIA_WORD_MASK(dev->win_size);

		DEBUG(4, "memcpy from %p to %p len = %d",
		      dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)), to, tocpy);
		memcpy_fromio(to, (dev->win_base) + (from & PCMCIA_BYTE_MASK(dev->win_size)), tocpy);
		len -= tocpy;
		to += tocpy;
		from += tocpy;

		// Handle even byte on 16bit bus
		if((toread & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "reading word from %p", dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)));
			data = readw((dev->win_base) + (from & PCMCIA_WORD_MASK(dev->win_size)));
			*(__u8 *)to = (__u8)(data & 0xff);
			to++;
			from++;
			toread--;
			len--;
		}			
	}

}


static void pcmcia_write8_remap(struct map_info *map, __u8 d, unsigned long adr)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	if(remap_window(dev, win, adr) == -1)
		return;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x", adr,
	      (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)), d);
	writeb(d, (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)));
}


static void pcmcia_write16_remap(struct map_info *map, __u16 d, unsigned long adr)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	if(remap_window(dev, win, adr) == -1)
		return;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x", adr,
	      (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)), d);
	writew(d, (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)));
}


static void pcmcia_copy_to_remap(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(3, "to = %lu from = %p len = %u", to, from, len);
	while(len) {
		int towrite = dev->win_size - (to & PCMCIA_BYTE_MASK(dev->win_size));
		int tocpy;
		if(towrite > len)
			towrite = len;

		DEBUG(4, "to = 0x%8.8lx len = %u towrite = %d offset = 0x%8.8lx",
		      to, len, towrite, to & ~PCMCIA_BYTE_MASK(dev->win_size));

		if(remap_window(dev, win, to) == -1)
			return;

		// Handle odd byte on 16bit bus
		if((to & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "writing word to %p", dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)));
			data = readw(dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)));
			data &= 0x00ff;
			data |= *(__u8 *)from << 8;
			writew(data, (dev->win_base) + (to & PCMCIA_WORD_MASK(dev->win_size)));
			to++;
			from++;
			len--;
			towrite--;
		}

		tocpy = towrite;
		if(map->buswidth == 2)
			tocpy &= PCMCIA_WORD_MASK(dev->win_size);


		DEBUG(4, "memcpy from %p to %p len = %d",
		      from, dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)), tocpy);
		memcpy_toio((dev->win_base) + (to & PCMCIA_BYTE_MASK(dev->win_size)), from, tocpy);
		len -= tocpy;
		to += tocpy;
		from += tocpy;

		// Handle even byte on 16bit bus
		if((towrite & 1) && (map->buswidth ==2)) {
			__u16 data;

			DEBUG(4, "writing word to %p", dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)));
			data = readw((dev->win_base) + (to & PCMCIA_WORD_MASK(dev->win_size)));
			data &= 0xff00;
			data |= *(__u8 *)from;
			writew(data, (dev->win_base) + (to & PCMCIA_WORD_MASK(dev->win_size)));
			to++;
			from++;
			towrite--;
			len--;
		}	

	}
}

/* Non remap versions */

static __u8 pcmcia_read8(struct map_info *map, unsigned long ofs)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	__u8 d;

	d = readb(win_base + ofs);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x", ofs, win_base + ofs, d);
	return d;
}


static __u16 pcmcia_read16(struct map_info *map, unsigned long ofs)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	__u16 d;

	d = readw(win_base +ofs);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x", ofs, win_base + ofs, d);
	return d;
}


static void pcmcia_copy_from(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	unsigned int win_size = map->map_priv_2;
	__u16 data;
	ssize_t tocpy;

	DEBUG(3, "to = %p from = %lu len = %u", to, from, len);
	// Handle odd byte on 16bit bus
	if((from & 1) && (map->buswidth == 2)) {
		DEBUG(4, "reading word from %p", win_base + from);
		data = readw(win_base + from);
		*(__u8 *)to = (__u8)(data >> 8);
		to++;
		from++;
		len--;
	}
	tocpy = len;
	if(map->buswidth == 2)
		tocpy &= PCMCIA_WORD_MASK(win_size);

	DEBUG(4, "memcpy from %p to %p len = %d", win_base + (from & PCMCIA_WORD_MASK(win_size)), to, tocpy);
	memcpy_fromio(to, win_base + (from & PCMCIA_BYTE_MASK(win_size)), tocpy);
	len -= tocpy;
	to += tocpy;
	from += tocpy;

	// Handle even byte on 16bit bus
	if((len & 1) && (map->buswidth == 2)) {
		DEBUG(4, "reading word from %p", win_base + (from & PCMCIA_WORD_MASK(win_size)));
		data = readw(win_base + (from & PCMCIA_WORD_MASK(win_size)));
		*(__u8 *)to = (__u8)(data & 0xff);
	}			
}


static void pcmcia_write8(struct map_info *map, __u8 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x", adr, win_base + adr, d);
	writeb(d, win_base + adr);
}


static void pcmcia_write16(struct map_info *map, __u16 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x", adr, win_base + adr, d);
	writew(d, win_base + adr);
}


static void pcmcia_copy_to(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	unsigned int win_size = map->map_priv_2;
	__u16 data;
	ssize_t tocpy;

	DEBUG(3, "to = %lu from = %p len = %u", to, from, len);
	// Handle odd byte on 16bit bus
	if((to & 1) && (map->buswidth == 2)) {
		DEBUG(4, "writing word to %p", win_base + to);
		data = readw(win_base + to);
		data &= 0x00ff;
		data |= *(__u8 *)from << 8;
		writew(data, win_base + to);
		to++;
		from++;
		len--;
	}

	tocpy = len;
	if(map->buswidth == 2)
		tocpy &= PCMCIA_WORD_MASK(win_size);
	
	DEBUG(4, "memcpy from %p to %p len = %d", from, win_base + (to & PCMCIA_WORD_MASK(win_size)), tocpy);
	memcpy_toio(win_base + (to & PCMCIA_BYTE_MASK(win_size)), from, tocpy);
	len -= tocpy;
	to += tocpy;
	from += tocpy;

	// Handle even byte on 16bit bus
	if((len & 1) && (map->buswidth ==2)) {
		DEBUG(4, "writing word to %p", win_base + (to & PCMCIA_WORD_MASK(win_size)));
		data = readw(win_base + (to & PCMCIA_WORD_MASK(win_size)));
		data &= 0xff00;
		data |= *(__u8 *)from;
		writew(data, win_base + (to & PCMCIA_WORD_MASK(win_size)));
	}
}


/*======================================================================

After a card is removed, memory_release() will unregister the
device, and release the PCMCIA configuration.  If the device is
still open, this will be postponed until it is closed.

======================================================================*/

static void memory_release(u_long arg)
{
	dev_link_t *link = (dev_link_t *)arg;
	memory_dev_t *dev = NULL;
	int ret;
	struct list_head *temp1, *temp2;

	DEBUG(3, "memory_release(0x%p)", link);
	/* Find device in list */
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev = list_entry(temp1, memory_dev_t, list);
		if(link == &dev->link)
			break;
	}
	if(link != &dev->link) {
		DEBUG(1, "Cant find %p in dev_list", link);
		return;
	}

	if(dev) {
		if(dev->mtd_info) {
			del_mtd_device(dev->mtd_info);
			dev->mtd_info = NULL;
			MOD_DEC_USE_COUNT;
		}
		if (link->win) {
			if(dev->win_base) {
				iounmap(dev->win_base);
				dev->win_base = NULL;
			}
			DEBUG(2, "ReleaseWindow() called");
			CardServices(ReleaseWindow, link->win);
		}
		ret = CardServices(ReleaseConfiguration, link->handle);
		if(ret != CS_SUCCESS)
			cs_error(link->handle, ReleaseConfiguration, ret);
			
	}
	link->state &= ~DEV_CONFIG;
}




static void card_settings(memory_dev_t *dev, dev_link_t *link)
{
	int rc;
	tuple_t tuple;
	cisparse_t parse;
	u_char buf[64];
	tuple.Attributes = 0;
	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleDataMax = sizeof(buf);
	tuple.TupleOffset = 0;
	tuple.DesiredTuple = RETURN_FIRST_TUPLE;
	rc = CardServices(GetFirstTuple, link->handle, &tuple);
	while(rc == CS_SUCCESS) {
		rc = CardServices(GetTupleData, link->handle, &tuple);
		if(rc != CS_SUCCESS) {
			cs_error(link->handle, GetTupleData, rc);
			break;
		}
		rc = CardServices(ParseTuple, link->handle, &tuple, &parse);
		if(rc != CS_SUCCESS) {
			cs_error(link->handle, ParseTuple, rc);
			break;
		}
		
		switch(tuple.TupleCode) {
		case  CISTPL_FORMAT: {
			cistpl_format_t *t = &parse.format;
			DEBUG(2, "Format type: %u, Error Detection: %u, offset = %u, length =%u",
			      t->type, t->edc, t->offset, t->length);
			break;
			
		}
			
		case CISTPL_DEVICE: {
			cistpl_device_t *t = &parse.device;
			int i;
			DEBUG(1, "Common memory:");
			dev->pcmcia_map.size = t->dev[0].size;
			for(i = 0; i < t->ndev; i++) {
				DEBUG(2, "Region %d, type = %u", i, t->dev[i].type);
				DEBUG(2, "Region %d, wp = %u", i, t->dev[i].wp);
				DEBUG(2, "Region %d, speed = %u ns", i, t->dev[i].speed);
				DEBUG(2, "Region %d, size = %u bytes", i, t->dev[i].size);
			}
			break;
		}
			
		case CISTPL_VERS_1: {
			cistpl_vers_1_t *t = &parse.version_1;
			int i;
			if(t->ns) {
				dev->mtd_name[0] = '\0';
				for(i = 0; i < t->ns; i++) {
					if(i)
						strcat(dev->mtd_name, " ");
					strcat(dev->mtd_name, t->str+t->ofs[i]);
				}
			}
			DEBUG(2, "Found name: %s", dev->mtd_name);
			break;
		}
			
		case CISTPL_JEDEC_C: {
			cistpl_jedec_t *t = &parse.jedec;
			int i;
			for(i = 0; i < t->nid; i++) {
				DEBUG(2, "JEDEC: 0x%02x 0x%02x", t->id[i].mfr, t->id[i].info);
			}
			break;
		}
			
		case CISTPL_DEVICE_GEO: {
			cistpl_device_geo_t *t = &parse.device_geo;
			int i;
			dev->pcmcia_map.buswidth = t->geo[0].buswidth;
			for(i = 0; i < t->ngeo; i++) {
				DEBUG(2, "region: %d buswidth = %u", i, t->geo[i].buswidth);
				DEBUG(2, "region: %d erase_block = %u", i, t->geo[i].erase_block);
				DEBUG(2, "region: %d read_block = %u", i, t->geo[i].read_block);
				DEBUG(2, "region: %d write_block = %u", i, t->geo[i].write_block);
				DEBUG(2, "region: %d partition = %u", i, t->geo[i].partition);
				DEBUG(2, "region: %d interleave = %u", i, t->geo[i].interleave);
			}
			break;
		}
			
		default:
			DEBUG(1, "Unknown tuple code %d", tuple.TupleCode);
		}
		
		rc = CardServices(GetNextTuple, link->handle, &tuple, &parse);
	}
	if(!dev->pcmcia_map.size)
		dev->pcmcia_map.size = MAX_PCMCIA_ADDR;

	if(!dev->pcmcia_map.buswidth)
		dev->pcmcia_map.buswidth = 2;

	if(force_size) {
		dev->pcmcia_map.size = force_size << 20;
		DEBUG(2, "size fored to %dM", force_size);

	}

	if(buswidth) {
		dev->pcmcia_map.buswidth = buswidth;
		DEBUG(2, "buswidth forced to %d", buswidth);
	}		


	dev->pcmcia_map.name = dev->mtd_name;
	if(!dev->mtd_name[0])
		strcpy(dev->mtd_name, "PCMCIA Memory card");

	DEBUG(1, "Device: Size: %lu Width:%d Name: %s",
	      dev->pcmcia_map.size, dev->pcmcia_map.buswidth << 3, dev->mtd_name);
}


/*======================================================================

memory_config() is scheduled to run after a CARD_INSERTION event
is received, to configure the PCMCIA socket, and to make the
MTD device available to the system.

======================================================================*/

#define CS_CHECK(fn, args...) \
while ((last_ret=CardServices(last_fn=(fn), args))!=0) goto cs_failed

static void memory_config(dev_link_t *link)
{
	memory_dev_t *dev = link->priv;
	struct mtd_info *mtd = NULL;
	cs_status_t status;
	win_req_t req;
	int last_ret = 0, last_fn = 0;
	int ret;
	int i,j;
	config_info_t t;
	config_req_t r;
	static const char *probes[] = { "jedec_probe", "cfi_probe" };
	cisinfo_t cisinfo;

	DEBUG(3, "memory_config(0x%p)", link);

	/* Configure card */
	link->state |= DEV_CONFIG;

	DEBUG(2, "Validating CIS");
	ret = CardServices(ValidateCIS, link->handle, &cisinfo);
	if(ret != CS_SUCCESS) {
		cs_error(link->handle, GetTupleData, ret);
	} else {
		DEBUG(2, "ValidateCIS found %d chains", cisinfo.Chains);
	}


	card_settings(dev, link);

	dev->pcmcia_map.read8 = pcmcia_read8_remap;
	dev->pcmcia_map.read16 = pcmcia_read16_remap;
	dev->pcmcia_map.copy_from = pcmcia_copy_from_remap;
	dev->pcmcia_map.write8 = pcmcia_write8_remap;
	dev->pcmcia_map.write16 = pcmcia_write16_remap;
	dev->pcmcia_map.copy_to = pcmcia_copy_to_remap;


	/* Allocate a small memory window for direct access */
	req.Attributes =  WIN_MEMORY_TYPE_CM | WIN_ENABLE;
	req.Attributes |= (dev->pcmcia_map.buswidth == 1) ? WIN_DATA_WIDTH_8 : WIN_DATA_WIDTH_16;

	/* Request a memory window for PCMCIA. Some architeures can map windows upto the maximum
	   that PCMCIA can support (64Mb) - this is ideal and we aim for a window the size of the
	   whole card - otherwise we try smaller windows until we succeed */

	req.Base = 0;
	req.AccessSpeed = mem_speed;
	link->win = (window_handle_t)link->handle;
	req.Size = MAX_PCMCIA_ADDR;
	if(force_size)
		req.Size = force_size << 20;

	dev->win_size = 0;
	do {
		int ret;
		DEBUG(2, "requesting window with size = %dKB memspeed = %d",
		      req.Size >> 10, req.AccessSpeed);
		link->win = (window_handle_t)link->handle;
		ret = CardServices(RequestWindow, &link->win, &req);
		DEBUG(2, "ret = %d dev->win_size = %d", ret, dev->win_size);
		if(ret) {
			req.Size >>= 1;
		} else {
			DEBUG(2, "Got window of size %dKB", req.Size >> 10);
			dev->win_size = req.Size;
			break;
		}
	} while(req.Size >= 0x1000);

	DEBUG(2, "dev->win_size = %d", dev->win_size);

	if(!dev->win_size) {
		err("Cant allocate memory window");
		memory_release((u_long)link);
		return;
	}
	DEBUG(1, "Allocated a window of %dKB", dev->win_size >> 10);
		
	/* Get write protect status */
	CS_CHECK(GetStatus, link->handle, &status);
	DEBUG(1, "status value: 0x%x", status.CardState);
	DEBUG(2, "Window handle = 0x%8.8lx", (unsigned long)link->win);
	dev->win_base = ioremap(req.Base, req.Size);
	DEBUG(1, "mapped window dev = %p req.base = 0x%lx base = %p size = 0x%x",
	      dev, req.Base, dev->win_base, req.Size);
	dev->cardsize = 0;
	dev->offset = 0;

	dev->pcmcia_map.map_priv_1 = (unsigned long)dev;
	dev->pcmcia_map.map_priv_2 = (unsigned long)link->win;

	DEBUG(2, "Getting configuration");
	CS_CHECK(GetConfigurationInfo, link->handle, &t);
	DEBUG(2, "Vcc = %d Vpp1 = %d Vpp2 = %d", t.Vcc, t.Vpp1, t.Vpp2);
	
	r.Attributes = 0;
	r.Vcc = (vcc) ? vcc : t.Vcc;
	r.Vpp1 = (vpp) ? vpp : t.Vpp1;
	r.Vpp2 = (vpp) ? vpp : t.Vpp2;
	
	r.IntType = INT_MEMORY;
	r.ConfigBase = t.ConfigBase;
	r.Status = t.Status;
	r.Pin = t.Pin;
	r.Copy = t.Copy;
	r.ExtStatus = t.ExtStatus;
	r.ConfigIndex = 0;
	r.Present = t.Present;
	DEBUG(2, "Setting Configuration");
	CS_CHECK(RequestConfiguration, link->handle ,&r);
	
	DEBUG(2, "Getting configuration");
	CS_CHECK(GetConfigurationInfo, link->handle, &t);
	DEBUG(2, "Vcc = %d Vpp1 = %d Vpp2 = %d", t.Vcc, t.Vpp1, t.Vpp2);

	/* Dump 256 bytes from card */
	if(debug > 4) {
		char *p = dev->win_base;
		for(i = 0; i < 16; i++) {
			printk(KERN_DEBUG "pcmcia_mtd: 0x%4.4x: ", i << 4);
			for(j = 0; j < 16; j++) {
				printk(KERN_DEBUG "0x%2.2x ", readb(p));
				p += 1;
			}
			printk(KERN_DEBUG "\n");
		}
	}

	link->dev = NULL;
	link->state &= ~DEV_CONFIG_PENDING;

	if(mem_type == 1) {
		mtd = do_map_probe("map_ram", &dev->pcmcia_map);
	} else if(mem_type == 2) {
		mtd = do_map_probe("map_rom", &dev->pcmcia_map);
	} else {
		for(i = 0; i < sizeof(probes) / sizeof(char *); i++) {
			DEBUG(1, "Trying %s", probes[i]);
			mtd = do_map_probe(probes[i], &dev->pcmcia_map);
			if(mtd)
				break;
			
			DEBUG(1, "FAILED: %s", probes[i]);
		}
	}
	
	if(!mtd) {
		DEBUG(1, "Cant find an MTD");
		memory_release((u_long)link);
		return;
	}

	dev->mtd_info = mtd;
	mtd->module = THIS_MODULE;
	dev->cardsize = mtd->size;

	/* If the memory found is fits completely into the mapped PCMCIA window,
	   use the faster non-remapping read/write functions */
	if(dev->cardsize <= dev->win_size) {
		DEBUG(1, "Using non remapping memory functions");
		dev->pcmcia_map.map_priv_1 = (unsigned long)dev->win_base;
		dev->pcmcia_map.map_priv_2 = (unsigned long)dev->win_size;
		dev->pcmcia_map.read8 = pcmcia_read8;
		dev->pcmcia_map.read16 = pcmcia_read16;
		dev->pcmcia_map.copy_from = pcmcia_copy_from;
		dev->pcmcia_map.write8 = pcmcia_write8;
		dev->pcmcia_map.write16 = pcmcia_write16;
		dev->pcmcia_map.copy_to = pcmcia_copy_to;
	}

	MOD_INC_USE_COUNT;
	if(add_mtd_device(mtd)) {
		dev->mtd_info = NULL;
		MOD_DEC_USE_COUNT;
		err("Couldnt register MTD device");
		memory_release((u_long)link);
		return;
	}
	DEBUG(1, "memory_config: mtd added @ %p mtd->priv = %p", mtd, mtd->priv);

	return;

 cs_failed:
	cs_error(link->handle, last_fn, last_ret);
	err("CS Error, exiting");
	memory_release((u_long)link);
	return;
}


/*======================================================================

The card status event handler.  Mostly, this schedules other
stuff to run after an event is received.  A CARD_REMOVAL event
also sets some flags to discourage the driver from trying
to talk to the card any more.

======================================================================*/

static int memory_event(event_t event, int priority,
			event_callback_args_t *args)
{
	dev_link_t *link = args->client_data;

	DEBUG(1, "memory_event(0x%06x)", event);
	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		DEBUG(2, "EVENT_CARD_REMOVAL");
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG)
			mod_timer(&link->release, jiffies + HZ/20);
		break;
	case CS_EVENT_CARD_INSERTION:
		DEBUG(2, "EVENT_CARD_INSERTION");
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		memory_config(link);
		break;
	case CS_EVENT_PM_SUSPEND:
		DEBUG(2, "EVENT_PM_SUSPEND");
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		DEBUG(2, "EVENT_RESET_PHYSICAL");
		/* get_lock(link); */
		break;
	case CS_EVENT_PM_RESUME:
		DEBUG(2, "EVENT_PM_RESUME");
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		DEBUG(2, "EVENT_CARD_RESET");
		/* free_lock(link); */
		break;
	default:
		DEBUG(2, "Unknown event %d", event);
	}
	return 0;
}

/*======================================================================

This deletes a driver "instance".  The device is de-registered
with Card Services.  If it has been released, all local data
structures are freed.  Otherwise, the structures will be freed
when the device is released.

======================================================================*/

static void memory_detach(dev_link_t *link)
{
	int ret;
	memory_dev_t *dev = NULL;
	struct list_head *temp1, *temp2;

	DEBUG(3, "memory_detach(0x%p)", link);

	/* Find device in list */
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev = list_entry(temp1, memory_dev_t, list);
		if(link == &dev->link)
			break;
	}
	if(link != &dev->link) {
		DEBUG(1, "Cant find %p in dev_list", link);
		return;
	}
	
	del_timer(&link->release);

	if(!dev) {
		DEBUG(3, "dev is NULL");
		return;
	}

	if (link->state & DEV_CONFIG) {
		//memory_release((u_long)link);
		DEBUG(3, "DEV_CONFIG set");
		link->state |= DEV_STALE_LINK;
		return;
	}

	if (link->handle) {
		DEBUG(2, "Deregistering with card services");
		ret = CardServices(DeregisterClient, link->handle);
		if (ret != CS_SUCCESS)
			cs_error(link->handle, DeregisterClient, ret);
	}
	DEBUG(3, "Freeing dev (%p)", dev);
	list_del(&dev->list);
	link->priv = NULL;
	kfree(dev);


}



/*======================================================================

memory_attach() creates an "instance" of the driver, allocating
local data structures for one device.  The device is registered
with Card Services.

======================================================================*/

static dev_link_t *memory_attach(void)
{
	memory_dev_t *dev;
	dev_link_t *link;
	client_reg_t client_reg;
	int ret;

	DEBUG(2, "memory_attach()");

	/* Create new memory card device */
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) return NULL;
	DEBUG(1, "memory_attach: dev = %p", dev);

	memset(dev, 0, sizeof(*dev));
	link = &dev->link; link->priv = dev;

	link->release.function = &memory_release;
	link->release.data = (u_long)link;

	link->conf.Attributes = 0;
	link->conf.IntType = INT_MEMORY;

	list_add(&dev->list, &dev_list);

	/* Register with Card Services */
	client_reg.dev_info = &dev_info;
	client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
	client_reg.EventMask =
		CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
		CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
		CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
	client_reg.event_handler = &memory_event;
	client_reg.Version = 0x0210;
	client_reg.event_callback_args.client_data = link;
	DEBUG(2, "Calling RegisterClient");
	ret = CardServices(RegisterClient, &link->handle, &client_reg);
	if (ret != 0) {
		cs_error(link->handle, RegisterClient, ret);
		memory_detach(link);
		return NULL;
	}

	return link;
}

/*====================================================================*/

static int __init init_pcmcia_mtd(void)
{
	servinfo_t serv;

	info(DRIVER_DESC " " DRIVER_VERSION);
	CardServices(GetCardServicesInfo, &serv);
	if (serv.Revision != CS_RELEASE_CODE) {
		err("Card Services release does not match!");
		return -1;
	}
	register_pccard_driver(&dev_info, &memory_attach, &memory_detach);

	if(buswidth && buswidth != 1 && buswidth != 2) {
		info("bad buswidth (%d), using default", buswidth);
		buswidth = 2;
	}
	if(force_size && (force_size < 1 || force_size > 64)) {
		info("bad force_size (%d), using default", force_size);
		force_size = 0;
	}
	if(mem_type && mem_type != 1 && mem_type != 2) {
		info("bad mem_type (%d), using default", mem_type);
		mem_type = 0;
	}

	return 0;
}


static void __exit exit_pcmcia_mtd(void)
{
	struct list_head *temp1, *temp2;

	DEBUG(1, "unloading");
	unregister_pccard_driver(&dev_info);
	list_for_each_safe(temp1, temp2, &dev_list) {
		dev_link_t *link =&list_entry(temp1, memory_dev_t, list)->link;
		if (link && (link->state & DEV_CONFIG)) {
			memory_release((u_long)link);
			memory_detach(link);
		}
	}
}

module_init(init_pcmcia_mtd);
module_exit(exit_pcmcia_mtd);
