/* 
 * $Id: pcmcia_mtd.c,v 1.16 2002-05-27 12:56:22 spse Exp $
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

#define PCMCIA_DEBUG


#ifdef PCMCIA_DEBUG
static int pc_debug = 1;
MODULE_PARM(pc_debug, "i");
#undef DEBUG
#define DEBUG(n, args...) if (pc_debug>=(n)) printk("pcmcia_mtd:" __FUNCTION__ "(): " args)
static char *version ="pcmcia_mtd.c $Revision: 1.16 $";
#else
#define DEBUG(n, args...)
const int pc_debug = 0;
#endif

/*====================================================================*/

/* Parameters that can be set with 'insmod' */

/* 2 = do 16-bit transfers, 1 = do 8-bit transfers */
static int buswidth = 2;

/* Speed of memory accesses, in ns */
static int mem_speed = 0;

/* Force the size of an SRAM card */
static int force_size = 0;

/* Force Vcc */
static int vcc = 0;

/* Force Vpp */
static int vpp = 0;

/* Force card to be treated as ROM or RAM */
static int mem_type = 0;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Evans <spse@secret.org.uk>");
MODULE_DESCRIPTION("PCMCIA Flash memory card driver");
MODULE_PARM(buswidth, "i");
MODULE_PARM_DESC(buswidth, "Set buswidth (1 = 8bit, 2 = 16 bit, default = 2)");
MODULE_PARM(mem_speed, "i");
MODULE_PARM_DESC(mem_speed, "Set memory access speed in ns");
MODULE_PARM(force_size, "i");
MODULE_PARM_DESC(force_size, "Force size of card in MB (1-64)");
MODULE_PARM(vcc, "i");
MODULE_PARM_DESC(vcc, "Set Vcc in 1/10ths eg 33 = 3.3V 120 = 12V (Dangerous)");
MODULE_PARM(vpp, "i");
MODULE_PARM_DESC(vpp, "Set Vpp in 1/10ths eg 33 = 3.3V 120 = 12V (Dangerous)");
MODULE_PARM(mem_type, "i");
MODULE_PARM_DESC(mem_type, "Set Memory type (0 = Flash, 1 = RAM, 2 = ROM, default = 0");

/* Maximum number of separate memory devices we'll allow */
#define MAX_DEV		4

/* Size of the PCMCIA address space: 26 bits = 64 MB */
#define MAX_PCMCIA_ADDR	0x4000000

#define PCMCIA_BYTE_MASK(x)  (x-1)
#define PCMCIA_WORD_MASK(x)  (x-2)

static void memory_release(u_long arg);
static int memory_event(event_t event, int priority,
			event_callback_args_t *args);

static dev_link_t *memory_attach(void);
static void memory_detach(dev_link_t *);

typedef struct memory_dev_t {
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


static dev_info_t dev_info = "memory_mtd";
static dev_link_t *dev_table[MAX_DEV] = { NULL, /* ... */ };

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
		DEBUG(2, "Remapping window from 0x%8.8x to 0x%8.8x\n", 
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
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x\n", ofs,
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
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x\n", ofs,
	      (dev->win_base)+(ofs & PCMCIA_BYTE_MASK(dev->win_size)), d);
  
	return d;
}


static void pcmcia_copy_from_remap(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(3, "to = %p from = %lu len = %u\n", to, from, len);
	while(len) {
		int toread = dev->win_size - (from & PCMCIA_BYTE_MASK(dev->win_size));
		int tocpy;
		if(toread > len) 
			toread = len;

		DEBUG(4, "from = 0x%8.8lx len = %u toread = %ld offset = 0x%8.8lx\n",
		      (long)from, (unsigned int)len, (long)toread, from & ~PCMCIA_BYTE_MASK(dev->win_size));

		if(remap_window(dev, win, from) == -1)
			return;

		// Handle odd byte on 16bit bus
		if((from & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "reading word from %p\n", dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)));
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

		DEBUG(4, "memcpy from %p to %p len = %d\n", 
		      dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)), to, tocpy);
		memcpy_fromio(to, (dev->win_base) + (from & PCMCIA_BYTE_MASK(dev->win_size)), tocpy);
		len -= tocpy;
		to += tocpy;
		from += tocpy;

		// Handle even byte on 16bit bus
		if((toread & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "reading word from %p\n", dev->win_base + (from & PCMCIA_WORD_MASK(dev->win_size)));
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

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x\n", adr, 
	      (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)), d);
	writeb(d, (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)));
}


static void pcmcia_write16_remap(struct map_info *map, __u16 d, unsigned long adr)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	if(remap_window(dev, win, adr) == -1)
		return;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x\n", adr, 
	      (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)), d);
	writew(d, (dev->win_base)+(adr & PCMCIA_BYTE_MASK(dev->win_size)));
}


static void pcmcia_copy_to_remap(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(3, "to = %lu from = %p len = %u\n", to, from, len);
	while(len) {
		int towrite = dev->win_size - (to & PCMCIA_BYTE_MASK(dev->win_size));
		int tocpy;
		if(towrite > len) 
			towrite = len;

		DEBUG(4, "to = 0x%8.8lx len = %u towrite = %d offset = 0x%8.8lx\n",
		      to, len, towrite, to & ~PCMCIA_BYTE_MASK(dev->win_size));

		if(remap_window(dev, win, to) == -1)
			return;

		// Handle odd byte on 16bit bus
		if((to & 1) && (map->buswidth == 2)) {
			__u16 data;

			DEBUG(4, "writing word to %p\n", dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)));
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


		DEBUG(4, "memcpy from %p to %p len = %d\n", 
		      from, dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)), tocpy);
		memcpy_toio((dev->win_base) + (to & PCMCIA_BYTE_MASK(dev->win_size)), from, tocpy);
		len -= tocpy;
		to += tocpy;
		from += tocpy;

		// Handle even byte on 16bit bus
		if((towrite & 1) && (map->buswidth ==2)) {
			__u16 data;

			DEBUG(4, "writing word to %p\n", dev->win_base + (to & PCMCIA_WORD_MASK(dev->win_size)));
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
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%02x\n", ofs, win_base + ofs, d);
	return d;
}


static __u16 pcmcia_read16(struct map_info *map, unsigned long ofs)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	__u16 d;

	d = readw(win_base +ofs);
	DEBUG(3, "ofs = 0x%08lx (%p) data = 0x%04x\n", ofs, win_base + ofs, d);  
	return d;
}


static void pcmcia_copy_from(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	unsigned int win_size = map->map_priv_2;
	__u16 data;
	ssize_t tocpy;

	DEBUG(3, "to = %p from = %lu len = %u\n", to, from, len);
	// Handle odd byte on 16bit bus
	if((from & 1) && (map->buswidth == 2)) {
		DEBUG(4, "reading word from %p\n", win_base + from);
		data = readw(win_base + from);
		*(__u8 *)to = (__u8)(data >> 8);
		to++;
		from++;
		len--;
	}
	tocpy = len;
	if(map->buswidth == 2)
		tocpy &= PCMCIA_WORD_MASK(win_size);

	DEBUG(4, "memcpy from %p to %p len = %d\n", win_base + (from & PCMCIA_WORD_MASK(win_size)), to, tocpy);
	memcpy_fromio(to, win_base + (from & PCMCIA_BYTE_MASK(win_size)), tocpy);
	len -= tocpy;
	to += tocpy;
	from += tocpy;

	// Handle even byte on 16bit bus
	if((len & 1) && (map->buswidth == 2)) {
		DEBUG(4, "reading word from %p\n", win_base + (from & PCMCIA_WORD_MASK(win_size)));
		data = readw(win_base + (from & PCMCIA_WORD_MASK(win_size)));
		*(__u8 *)to = (__u8)(data & 0xff);
	}			
}


static void pcmcia_write8(struct map_info *map, __u8 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%02x\n", adr, win_base + adr, d);
	writeb(d, win_base + adr);
}


static void pcmcia_write16(struct map_info *map, __u16 d, unsigned long adr)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;

	DEBUG(3, "adr = 0x%08lx (%p)  data = 0x%04x\n", adr, win_base + adr, d);
	writew(d, win_base + adr);
}


static void pcmcia_copy_to(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	caddr_t win_base = (caddr_t)map->map_priv_1;
	unsigned int win_size = map->map_priv_2;
	__u16 data;
	ssize_t tocpy;

	DEBUG(3, "to = %lu from = %p len = %u\n", to, from, len);
	// Handle odd byte on 16bit bus
	if((to & 1) && (map->buswidth == 2)) {
		DEBUG(4, "writing word to %p\n", win_base + to);
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
	
	DEBUG(4, "memcpy from %p to %p len = %d\n", from, win_base + (to & PCMCIA_WORD_MASK(win_size)), tocpy);
	memcpy_toio(win_base + (to & PCMCIA_BYTE_MASK(win_size)), from, tocpy);
	len -= tocpy;
	to += tocpy;
	from += tocpy;

	// Handle even byte on 16bit bus
	if((len & 1) && (map->buswidth ==2)) {
		DEBUG(4, "writing word to %p\n", win_base + (to & PCMCIA_WORD_MASK(win_size)));
		data = readw(win_base + (to & PCMCIA_WORD_MASK(win_size)));
		data &= 0xff00;
		data |= *(__u8 *)from;
		writew(data, win_base + (to & PCMCIA_WORD_MASK(win_size)));
	}
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
	int i, ret;
    
	DEBUG(0, "memory_attach()\n");
	printk("memory_attach()\n");
	for (i = 0; i < MAX_DEV; i++)
		if (dev_table[i] == NULL) break;
	if (i == MAX_DEV) {
		printk(KERN_NOTICE "memory_mtd: no devices available\n");
		return NULL;
	}
    
	/* Create new memory card device */
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) return NULL;
	DEBUG(1, "memory_attach: dev = %p\n", dev);

	memset(dev, 0, sizeof(*dev));
	link = &dev->link; link->priv = dev;

	link->release.function = &memory_release;
	link->release.data = (u_long)link;
	dev_table[i] = link;
	//init_waitqueue_head(&dev->erase_pending);

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
	printk("Calling RegisterClient\n");
	ret = CardServices(RegisterClient, &link->handle, &client_reg);
	if (ret != 0) {
		cs_error(link->handle, RegisterClient, ret);
		memory_detach(link);
		return NULL;
	}
    
	return link;
} /* memory_attach */

/*======================================================================

This deletes a driver "instance".  The device is de-registered
with Card Services.  If it has been released, all local data
structures are freed.  Otherwise, the structures will be freed
when the device is released.

======================================================================*/

static void memory_detach(dev_link_t *link)
{
	memory_dev_t *dev = link->priv;

	DEBUG(0, "memory_detach(0x%p)\n", link);
    
#if 0
	del_timer(&link->release);
	if (link->state & DEV_CONFIG) {
		memory_release((u_long)link);
		if (link->state & DEV_STALE_CONFIG) {
			link->state |= DEV_STALE_LINK;
			return;
		}
	}
#endif
	if (link->handle)
		CardServices(DeregisterClient, link->handle);
	return;
	/* Unlink device structure, free bits */
	kfree(dev);
    
} /* memory_detach */


#define WIN_TYPE(a)  ((a) ? WIN_MEMORY_TYPE_AM : WIN_MEMORY_TYPE_CM)
#define WIN_WIDTH(w) ((w) ? WIN_DATA_WIDTH_16 : WIN_DATA_WIDTH_8)


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
	struct mtd_info *mtd;
	cs_status_t status;
	win_req_t req;
	int last_ret = 0, last_fn = 0;
	int i,j;
	config_info_t t;
	config_req_t r;


	//static char *probes[] = { "jedec_probe", "cfi_probe", "sharp", "amd_flash", "jedec" };
	static char *probes[] = { "sharp_probe", "jedec_probe" };

	mtd  = dev->mtd_info;
	DEBUG(0, "memory_config(0x%p)\n", link);

	/* Configure card */
	link->state |= DEV_CONFIG;


#ifdef CISTPL_FORMAT_MEM
	/* This is a hack, not a complete solution */
	{
		int rc;
		tuple_t tuple;
		cisparse_t parse;
		u_char buf[64];
		tuple.Attributes = TUPLE_RETURN_COMMON;
		tuple.TupleData = (cisdata_t *)buf;
		tuple.TupleDataMax = sizeof(buf);
		tuple.TupleOffset = 0;
		tuple.DesiredTuple = RETURN_FIRST_TUPLE;
		rc = CardServices(GetFirstTuple, link->handle, &tuple);
		while(rc == CS_SUCCESS) {
			CS_CHECK(GetTupleData, link->handle, &tuple);
			CS_CHECK(ParseTuple, link->handle, &tuple, &parse);

			switch(tuple.TupleCode) {
			case  CISTPL_FORMAT: {
				cistpl_format_t *t = &parse.format;
				DEBUG(2, "memory_mtd: Format type: %u, Error Detection: %u, offset = %u, length =%u\n",
				      t->type, t->edc, t->offset, t->length);
				break;

			}

			case CISTPL_DEVICE: {
				cistpl_device_t *t = &parse.device;
				int i;
				DEBUG(1, "Common memory:\n");
				dev->pcmcia_map.size = t->dev[0].size;
				for(i = 0; i < t->ndev; i++) {
					DEBUG(2, "Region %d, type = %u\n", i, t->dev[i].type);
					DEBUG(2, "Region %d, wp = %u\n", i, t->dev[i].wp);
					DEBUG(2, "Region %d, speed = %u ns\n", i, t->dev[i].speed);
					DEBUG(2, "Region %d, size = %u bytes\n", i, t->dev[i].size);
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
				DEBUG(2, "Found name: %s\n", dev->mtd_name);
				break;
			}

			case CISTPL_JEDEC_C: {
				cistpl_jedec_t *t = &parse.jedec;
				int i;
				for(i = 0; i < t->nid; i++) {
					DEBUG(2, "JEDEC: 0x%02x 0x%02x\n", t->id[i].mfr, t->id[i].info);
				}
				break;
			}

			case CISTPL_DEVICE_GEO: {
				cistpl_device_geo_t *t = &parse.device_geo;
				int i;
				dev->pcmcia_map.buswidth = t->geo[0].buswidth;
				for(i = 0; i < t->ngeo; i++) {
					DEBUG(2, "region: %d buswidth = %u\n", i, t->geo[i].buswidth);
					DEBUG(2, "region: %d erase_block = %u\n", i, t->geo[i].erase_block);
					DEBUG(2, "region: %d read_block = %u\n", i, t->geo[i].read_block);
					DEBUG(2, "region: %d write_block = %u\n", i, t->geo[i].write_block);
					DEBUG(2, "region: %d partition = %u\n", i, t->geo[i].partition);
					DEBUG(2, "region: %d interleave = %u\n", i, t->geo[i].interleave);
				}
				break;
			}
			
			default:
				DEBUG(1, "Unknown tuple code %d\n", tuple.TupleCode);
			}
				
			rc = CardServices(GetNextTuple, link->handle, &tuple, &parse);
		}
	}
#endif

	dev->pcmcia_map.read8 = pcmcia_read8_remap;
	dev->pcmcia_map.read16 = pcmcia_read16_remap;
	dev->pcmcia_map.copy_from = pcmcia_copy_from_remap;
	dev->pcmcia_map.write8 = pcmcia_write8_remap;
	dev->pcmcia_map.write16 = pcmcia_write16_remap;
	dev->pcmcia_map.copy_to = pcmcia_copy_to_remap;

	if(!dev->pcmcia_map.size)
		dev->pcmcia_map.size = MAX_PCMCIA_ADDR;

	if(!dev->pcmcia_map.buswidth)
		dev->pcmcia_map.buswidth = 2;

	if(force_size) {
		dev->pcmcia_map.size = force_size << 20;
		DEBUG(2, "size fored to %dM\n", force_size);

	}

	if(buswidth) {
		dev->pcmcia_map.buswidth = buswidth;
		DEBUG(2, "buswidth forced to %d\n", buswidth);
	}		


	dev->pcmcia_map.name = dev->mtd_name;
	if(!dev->mtd_name[0])
		strcpy(dev->mtd_name, "PCMCIA Memory card");


	DEBUG(1, "Device: Size: %lu Width:%d Name: %s\n",
	      dev->pcmcia_map.size, dev->pcmcia_map.buswidth << 3, dev->mtd_name);
    
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
		DEBUG(2, "requesting window with size = %dKB memspeed = %d\n",
		      req.Size >> 10, req.AccessSpeed);
		link->win = (window_handle_t)link->handle;
		ret = CardServices(RequestWindow, &link->win, &req);
		DEBUG(2, "ret = %d dev->win_size = %d\n", ret, dev->win_size);
		if(ret) {
			cs_error(link->handle, RequestWindow, ret);
			req.Size >>= 1;
		} else {
			DEBUG(2, "Got window of size %dKB\n", req.Size >> 10);
			dev->win_size = req.Size;
			break;
		}
	} while(req.Size >= 0x10000);
	DEBUG(2, "dev->win_size = %d\n", dev->win_size);
	if(!dev->win_size)
		goto cs_failed;

	/* Get write protect status */
	CS_CHECK(GetStatus, link->handle, &status);
	DEBUG(1, "status value: 0x%x\n", status.CardState);
	DEBUG(2, "Window handle = 0x%8.8lx\n", (unsigned long)link->win);
	dev->win_base = ioremap(req.Base, req.Size);
	DEBUG(1, "mapped window dev = %p req.base = 0x%lx base = %p size = 0x%x\n",
	      dev, req.Base, dev->win_base, req.Size);
	dev->win_size = req.Size;
	dev->cardsize = 0;
	dev->offset = 0;

	dev->pcmcia_map.map_priv_1 = (unsigned long)dev;
	dev->pcmcia_map.map_priv_2 = (unsigned long)link->win;

	DEBUG(2, "Getting configuration\n");
	CS_CHECK(GetConfigurationInfo, link->handle, &t);
	DEBUG(2, "Vcc = %d Vpp1 = %d Vpp2 = %d\n", t.Vcc, t.Vpp1, t.Vpp2);
	
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
	DEBUG(2, "Setting Configuration\n");
	CS_CHECK(RequestConfiguration, link->handle ,&r);
	
	DEBUG(2, "Getting configuration\n");
	CS_CHECK(GetConfigurationInfo, link->handle, &t);
	DEBUG(2, "Vcc = %d Vpp1 = %d Vpp2 = %d\n", t.Vcc, t.Vpp1, t.Vpp2);

	/* Dump 256 bytes from card */
	if(pc_debug > 4) {
		char *p = dev->win_base;
		for(i = 0; i < 16; i++) {
			printk("memory_mtd: 0x%4.4x: ", i << 4);
			for(j = 0; j < 8; j++) {
				printk("0x%2.2x ", readw(p));
				p += 2;
			}
			printk("\n");
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
			DEBUG(1, "Trying %s\n", probes[i]);
			mtd = do_map_probe(probes[i], &dev->pcmcia_map);
			if(mtd) 
				break;
			
			DEBUG(1, "FAILED: %s\n", probes[i]);
		}
	}
	
	if(!mtd) {
		DEBUG(1, "Cant find an MTD\n");
		memory_release((u_long)link);
		return;
	}

	dev->mtd_info = mtd;
	mtd->module = THIS_MODULE;
	dev->cardsize = mtd->size;

	/* If the memory found is fits completely into the mapped PCMCIA window, 
	   use the faster non-remapping read/write functions */
	if(dev->cardsize <= dev->win_size) {
		DEBUG(1, "Using non remapping memory functions\n");
		dev->pcmcia_map.map_priv_1 = (unsigned long)dev->win_base;
		dev->pcmcia_map.map_priv_2 = (unsigned long)dev->win_size;
		dev->pcmcia_map.read8 = pcmcia_read8;
		dev->pcmcia_map.read16 = pcmcia_read16;
		dev->pcmcia_map.copy_from = pcmcia_copy_from;
		dev->pcmcia_map.write8 = pcmcia_write8;
		dev->pcmcia_map.write16 = pcmcia_write16;
		dev->pcmcia_map.copy_to = pcmcia_copy_to;
	}

	if(add_mtd_device(mtd)) {
		printk("memory_mtd: Couldnt register MTD device\n");
		memory_release((u_long)link);
		return;
	} 
	DEBUG(1, "memory_config: mtd added @ %p mtd->priv = %p\n", mtd, mtd->priv);

	return;

 cs_failed:
	cs_error(link->handle, last_fn, last_ret);
	printk("memory_mtd: CS Error, exiting\n");
	memory_release((u_long)link);
	return;
} /* memory_config */

/*======================================================================

After a card is removed, memory_release() will unregister the 
device, and release the PCMCIA configuration.  If the device is
still open, this will be postponed until it is closed.
    
======================================================================*/

static void memory_release(u_long arg)
{
	dev_link_t *link = (dev_link_t *)arg;
	memory_dev_t *dev = link->priv;
	DEBUG(0, "memory_release(0x%p)\n", link);

	del_mtd_device(dev->mtd_info);
	link->dev = NULL;

	if (link->win) {
		iounmap(dev->win_base);
		printk("ReleaseWindow() called\n");
		CardServices(ReleaseWindow, link->win);
	}

	link->state &= ~DEV_CONFIG;
    
	if (link->state & DEV_STALE_LINK)
		memory_detach(link);
    
} /* memory_release */

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

	DEBUG(1, "memory_event(0x%06x)\n", event);
    
	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG)
			mod_timer(&link->release, jiffies + HZ/20);
		break;
	case CS_EVENT_CARD_INSERTION:
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		memory_config(link);
		break;
	case CS_EVENT_ERASE_COMPLETE:
		break;

	case CS_EVENT_PM_SUSPEND:
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		/* get_lock(link); */
		break;
	case CS_EVENT_PM_RESUME:
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		/* free_lock(link); */
		break;
	}
	return 0;
} /* memory_event */


/*====================================================================*/

static int __init init_memory_mtd(void)
{
	servinfo_t serv;
    
	DEBUG(0, "%s\n", version);
    
	CardServices(GetCardServicesInfo, &serv);
	if (serv.Revision != CS_RELEASE_CODE) {
		printk(KERN_NOTICE "memory_mtd: Card Services release "
		       "does not match!\n");
		//return -1;
	}
	register_pccard_driver(&dev_info, &memory_attach, &memory_detach);

	if(buswidth && buswidth != 1 && buswidth != 2) {
		printk("bad buswidth (%d), using default\n", buswidth);
		buswidth = 2;
	}
	if(force_size && (force_size < 1 || force_size > 64)) {
		printk("bad force_size (%d), using default\n", force_size);
		force_size = 0;
	}
	if(mem_type && mem_type != 1 && mem_type != 2) {
		printk("bad mem_type (%d), using default\n", mem_type);
		mem_type = 0;
	}

	return 0;
}


static void __exit exit_memory_mtd(void)
{
	int i;
	dev_link_t *link;

	DEBUG(0, "memory_mtd: unloading\n");
	unregister_pccard_driver(&dev_info);
	for (i = 0; i < MAX_DEV; i++) {
		link = dev_table[i];
		if (link) {
			if (link->state & DEV_CONFIG)
				memory_release((u_long)link);
			memory_detach(link);
		}
	}
}

module_init(init_memory_mtd);
module_exit(exit_memory_mtd);
