/* 
 * $Id: pcmciamtd.c,v 1.7 2002-05-22 14:52:08 spse Exp $
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

//#include <pcmcia/config.h>
//#include <pcmcia/k_compat.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/major.h>
#include <asm/io.h>
#include <asm/system.h>
#include <stdarg.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>


#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>

#define PCMCIA_DEBUG


#ifdef PCMCIA_DEBUG
static int pc_debug = 1;
MODULE_PARM(pc_debug, "i");
MODULE_LICENSE("GPL");
#undef DEBUG
#define DEBUG(n, args...) if (pc_debug>(n)) printk("pcmcia_mtd:" __FUNCTION__ "(): " args)
static char *version ="pcmcia_mtd.c $Revision: 1.7 $";
#else
#define DEBUG(n, args...)
#endif

/*====================================================================*/

/* Parameters that can be set with 'insmod' */

/* 1 = do 16-bit transfers, 0 = do 8-bit transfers */
static int word_width = 1;

/* Speed of memory accesses, in ns */
static int mem_speed = 0;

/* Force the size of an SRAM card */
static int force_size = 0;

/* Force buswidth */
static int buswidth = 0;

MODULE_PARM(word_width, "i");
MODULE_PARM(mem_speed, "i");
MODULE_PARM(force_size, "i");
MODULE_PARM(buswidth, "i");

/*====================================================================*/

/* Maximum number of separate memory devices we'll allow */
#define MAX_DEV		4

/* Maximum number of partitions per memory space */
#define MAX_PART	4

/* Maximum number of outstanding erase requests per socket */
#define MAX_ERASE	8

/* Sector size -- shouldn't need to change */
#define SECTOR_SIZE	512

/* Size of the PCMCIA address space: 26 bits = 64 MB */
#define HIGH_ADDR	0x4000000

static void memory_config(dev_link_t *link);
static void memory_release(u_long arg);
static int memory_event(event_t event, int priority,
			event_callback_args_t *args);

static dev_link_t *memory_attach(void);
static void memory_detach(dev_link_t *);

typedef struct memory_dev_t {
	dev_link_t		link;
	struct mtd_info       *mtd_info;
	caddr_t		Base;		/* ioremapped address of PCMCIA window */	
	u_int		Size;		/* size of window (usually 64K) */
	u_int		cardsize;	/* size of whole card */
	u_int		offset;		/* offset into card the window currently points at */
} memory_dev_t;


static dev_info_t dev_info = "memory_mtd";
static dev_link_t *dev_table[MAX_DEV] = { NULL, /* ... */ };

static void cs_error(client_handle_t handle, int func, int ret)
{
	error_info_t err = { func, ret };
	CardServices(ReportError, handle, &err);
}


/* Map driver */


static inline int remap_window(memory_dev_t *dev, window_handle_t win, unsigned long to)
{
	memreq_t mrq;
	int ret;

	mrq.CardOffset = to & ~0xffff;
	if(mrq.CardOffset != dev->offset) {
		DEBUG(2, "Remapping window from 0x%8.8x to 0x%8.8x\n", 
		      dev->offset, mrq.CardOffset);
		mrq.Page = 0;
		if( (ret = CardServices(MapMemPage, win, &mrq)) != CS_SUCCESS) {
			DEBUG(1, "cant mapmempage ret = %d\n", ret);
			return -1;
		}
		dev->offset = mrq.CardOffset;
	}
	return 0;
}		


static __u8 pcmcia_read8(struct map_info *map, unsigned long ofs)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;
	__u8 d;

	if(remap_window(dev, win, ofs) == -1)
		return 0;

	d = readb((dev->Base)+(ofs & 0xffff));
	DEBUG(2, "ofs = 0x%08lx (%p) data = 0x%02x\n", ofs,
	      (dev->Base)+(ofs & 0xffff), d);
  
	return d;
}


static __u16 pcmcia_read16(struct map_info *map, unsigned long ofs)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;
	__u16 d;

	if(remap_window(dev, win, ofs) == -1)
		return 0;

	d = readw((dev->Base)+(ofs & 0xffff));
	DEBUG(2, "ofs = 0x%08lx (%p) data = 0x%04x\n", ofs,
	      (dev->Base)+(ofs & 0xffff), d);
  
	return d;
}


static void pcmcia_copy_from(struct map_info *map, void *to, unsigned long from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(2, "to = %p from = %lu len = %u\n", to, from, len);
	while(len) {
		int toread = 0x10000 - (from & 0xffff);
		int tocpy;
		if(toread > len) 
			toread = len;

		DEBUG(3, "from = 0x%8.8lx len = %u toread = %ld offset = 0x%8.8lx\n",
		      (long)from, (unsigned int)len, (long)toread, from & ~0xffff);

		if(remap_window(dev, win, from) == -1)
			return;

		// Handle odd byte on 16bit bus
		if((from & 1) && word_width) {
			__u16 data;

			DEBUG(2, "reading word from %p\n", dev->Base + (from & 0xfffe));
			data = readw(dev->Base + (from & 0xfffe));
			*(__u8 *)to = (__u8)(data >> 8);
			to++;
			from++;
			len--;
			toread--;
		}
		tocpy = toread;
		if(word_width)
			tocpy &= 0xfffe;

		DEBUG(2, "memcpy from %p to %p len = %d\n", 
		      dev->Base + (from & 0xfffe), to, tocpy);
		memcpy_fromio(to, (dev->Base) + (from & 0xffff), tocpy);
		len -= tocpy;
		to += tocpy;
		from += tocpy;

		// Handle eben byte on 16bit bus
		if(word_width && (toread & 1)) {
			__u16 data;

			DEBUG(2, "reading word from %p\n", dev->Base + (from & 0xfffe));
			data = readw((dev->Base) + (from & 0xfffe));
			*(__u8 *)to = (__u8)(data & 0xff);
			to++;
			from++;
			toread--;
			len--;
		}			
	}

}


void pcmcia_write8(struct map_info *map, __u8 d, unsigned long adr)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	if(remap_window(dev, win, adr) == -1)
		return;

	DEBUG(2, "adr = 0x%08lx (%p)  data = 0x%02x\n", adr, 
	      (dev->Base)+(adr & 0xffff), d);
	writeb(d, (dev->Base)+(adr & 0xffff));
}


void pcmcia_write16(struct map_info *map, __u16 d, unsigned long adr)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	if(remap_window(dev, win, adr) == -1)
		return;

	DEBUG(2, "adr = 0x%08lx (%p)  data = 0x%04x\n", adr, 
	      (dev->Base)+(adr & 0xffff), d);
	writew(d, (dev->Base)+(adr & 0xffff));
}


void pcmcia_copy_to(struct map_info *map, unsigned long to, const void *from, ssize_t len)
{
	memory_dev_t *dev = (memory_dev_t *)map->map_priv_1;
	window_handle_t win = (window_handle_t)map->map_priv_2;

	DEBUG(2, "to = %lu from = %p len = %u\n", to, from, len);
	while(len) {
		int towrite = 0x10000 - (to & 0xffff);
		if(towrite > len) 
			towrite = len;

		DEBUG(3, "to = 0x%8.8lx len = %u towrite = %d offset = 0x%8.8lx\n",
		      to, len, towrite, to & ~0xffff);

		if(remap_window(dev, win, to) == -1)
			return;

		memcpy_toio((dev->Base) + (to & 0xffff), from, towrite);
		len -= towrite;
		to += towrite;
		from += towrite;
	}
}


struct map_info pcmcia_map = {
	name:      "PCMCIA Memory card",
	size:      16<<20,
	buswidth:  2,
	read8:     pcmcia_read8,
	read16:    pcmcia_read16,
	copy_from: pcmcia_copy_from,
	write8:    pcmcia_write8,
	write16:   pcmcia_write16,
	copy_to:   pcmcia_copy_to
};
  


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


static void  dump_region(int num, region_info_t *region) 
{
	DEBUG(1, "memory_mtd: region %d\n", num);
	DEBUG(1, "memory_mtd: Attributes = %u\n", region->Attributes);
	DEBUG(1, "memory_mtd: CardOffset = %X\n", region->CardOffset);
	DEBUG(1, "memory_mtd: RegionSize = %u\n", region->RegionSize);
	DEBUG(1, "memory_mtd: AccessSpeed = %u ns\n", region->AccessSpeed);
	DEBUG(1, "memory_mtd: BlockSize = %u\n", region->BlockSize);
	DEBUG(1, "memory_mtd: PartMultiple = %u\n", region->PartMultiple);
	DEBUG(1, "memory_mtd: Jedec ID = 0x%02x 0x%02x\n", region->JedecMfr, region->JedecInfo);
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
	struct mtd_info *mtd;
	region_info_t region;
	cs_status_t status;
	win_req_t req;
	int nd, last_ret, last_fn, ret;
	int i,j;

	//static char *probes[] = { "jedec_probe", "cfi_probe", "sharp", "amd_flash", "jedec" };
	static char *probes[] = { "jedec_probe", "sharp" };

	mtd  = dev->mtd_info;
	DEBUG(0, "memory_config(0x%p)\n", link);

	/* Configure card */
	link->state |= DEV_CONFIG;

	for (nd = 0; nd < MAX_DEV; nd++)
		if (dev_table[nd] == link) break;
    
	/* Allocate a small memory window for direct access */
	//req.Attributes = WIN_DATA_WIDTH_8 | WIN_ENABLE;
	if(!word_width) {
		req.Attributes = WIN_DATA_WIDTH_8 | WIN_MEMORY_TYPE_CM | WIN_ENABLE;
		pcmcia_map.buswidth = 1;
	} else {
		req.Attributes = WIN_DATA_WIDTH_16 | WIN_MEMORY_TYPE_CM | WIN_ENABLE;
	}
	if(buswidth) {
		pcmcia_map.buswidth = buswidth;
	}
	if(force_size) {
		pcmcia_map.size = force_size << 20;
		DEBUG(1, "size forced to %dM\n", force_size);
	}
	req.Base = 0;
	req.Size = 0x10000;
	req.AccessSpeed = mem_speed;
	link->win = (window_handle_t)link->handle;
	DEBUG(1, "requesting window with memspeed = %d\n", req.AccessSpeed);
	CS_CHECK(RequestWindow, &link->win, &req);
	/* Get write protect status */
	CS_CHECK(GetStatus, link->handle, &status);
	DEBUG(1, "status value: 0x%x\n", status.CardState);

	dev->Base = ioremap(req.Base, req.Size);
	DEBUG(1, "mapped window dev = %p req.base = 0x%lx base = %p size = 0x%x\n",
	      dev, req.Base, dev->Base, req.Size);
	dev->Size = req.Size;
	dev->cardsize = 0;
	dev->offset = 0;

	/* Dump 256 bytes from card */
	if(pc_debug > 4) {
		char *p = dev->Base;
		for(i = 0; i < 16; i++) {
			printk("memory_mtd: 0x%4.4x: ", i << 4);
			for(j = 0; j < 16; j++)
				printk("0x%2.2x ", readb(p));
			printk("\n");
		}
	}
				   
	for(i = 0; i < 2; i++) {
		region.Attributes = i ? REGION_TYPE_AM : REGION_TYPE_CM;
		ret = CardServices(GetFirstRegion, link->handle, &region);
		while (ret == CS_SUCCESS) {
			dump_region(0, &region);
			DEBUG(1, "Found region: Attr = 0x%8.8x offset = 0x%8.8x size = 0x%8.8x speed = %dns\n",
			      region.Attributes, region.CardOffset, region.RegionSize,
			      region.AccessSpeed);
			DEBUG(1, "blksz = 0x%8.8x multiple = %d mfr,info = (0x%2.2x, 0x%2.2x)\n",
			      region.BlockSize, region.PartMultiple, region.JedecMfr, region.JedecInfo);
		}
	}
    
	link->dev = NULL;
	link->state &= ~DEV_CONFIG_PENDING;
    
	/* Setup the mtd_info struct */


#if 1
	pcmcia_map.map_priv_1 = (unsigned long)dev;
	pcmcia_map.map_priv_2 = (unsigned long)link->win;
	DEBUG(1, "map_priv_1 = 0x%lx\n", pcmcia_map.map_priv_1);

	for(i = 0; i < sizeof(probes) / sizeof(char *); i++) {
		DEBUG(1, "Trying %s\n", probes[i]);
		mtd = do_map_probe(probes[i], &pcmcia_map);
		if(mtd) 
			break;
	    
		DEBUG(1, "FAILED: %s\n", probes[i]);
	}

	if(!mtd) {
		DEBUG(1, "Cant find an MTD\n");
		memory_release((u_long)link);
		return;
	}

	dev->mtd_info = mtd;
	mtd->module = THIS_MODULE;
#endif    

#if 0
	mtd->type = MTD_RAM;
	mtd->flags = MTD_CAP_RAM;
	mtd->size = 16<<20;
	mtd->erasesize = 64<<10;
	mtd->name = dev->mtd_name;
	strcpy(mtd->name, "PCMCIA Memory card");
	mtd->numeraseregions = 0;
	mtd->eraseregions = NULL;
	mtd->erase = mtd_erase;
	mtd->read = mtd_read;
	mtd->write = mtd_write;
	mtd->priv = dev;
	mtd->module = THIS_MODULE;			
#endif



#ifdef CISTPL_FORMAT_MEM
	/* This is a hack, not a complete solution */
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
			CS_CHECK(GetTupleData, link->handle, &tuple);
			CS_CHECK(ParseTuple, link->handle, &tuple, &parse);
			DEBUG(1, "memory_mtd: found tuple code: %d\n", tuple.TupleCode);
			if(tuple.TupleCode == CISTPL_FORMAT) {
				cistpl_format_t *t = &parse.format;
				//dev->minor.offset = parse.format.offset;
				DEBUG(1, "memory_mtd: Format type: %u, Error Detection: %u, offset = %u, length =%u\n",
				      t->type, t->edc, t->offset, t->length);

			}
			if(tuple.TupleCode == CISTPL_DEVICE) {
				cistpl_device_t *t = &parse.device;
				int i;
				DEBUG(1, "memory_mtd: Common memory:\n");
				for(i = 0; i < t->ndev; i++) {
					DEBUG(1, "memory_mtd: Region %d, type = %u\n", i, t->dev[i].type);
					DEBUG(1, "memory_mtd: Region %d, wp = %u\n", i, t->dev[i].wp);
					DEBUG(1, "memory_mtd: Region %d, speed = %u ns\n", i, t->dev[i].speed);
					DEBUG(1, "memory_mtd: Region %d, size = %u bytes\n", i, t->dev[i].size);
				}
			}
#if 0
			if(tuple.TupleCode == CISTPL_VERS_1) {
				cistpl_vers_1_t *t = &parse.version_1;
				int i;
				if(t->ns) {
					mtd->name[0] = '\0';
					for(i = 0; i < t->ns; i++) {
						strcat(mtd->name, t->str+t->ofs[i]);
						strcat(mtd->name, " ");
					}
				}
			}
#endif
			if(tuple.TupleCode == CISTPL_JEDEC_C) {
				cistpl_jedec_t *t = &parse.jedec;
				int i;
				for(i = 0; i < t->nid; i++) {
					DEBUG(1, "memory_mtd: JEDEC: 0x%02x 0x%02x\n", t->id[i].mfr, t->id[i].info);
				}
			}
			if(tuple.TupleCode == CISTPL_DEVICE_GEO) {
				cistpl_device_geo_t *t = &parse.device_geo;
				int i;
				for(i = 0; i < t->ngeo; i++) {
					DEBUG(1, "memory_mtd: region: %d buswidth = %u\n", i, t->geo[i].buswidth);
					DEBUG(1, "memory_mtd: region: %d erase_block = %u\n", i, t->geo[i].erase_block);
					DEBUG(1, "memory_mtd: region: %d read_block = %u\n", i, t->geo[i].read_block);
					DEBUG(1, "memory_mtd: region: %d write_block = %u\n", i, t->geo[i].write_block);
					DEBUG(1, "memory_mtd: region: %d partition = %u\n", i, t->geo[i].partition);
					DEBUG(1, "memory_mtd: region: %d interleave = %u\n", i, t->geo[i].interleave);
				}
			}
			rc = CardServices(GetNextTuple, link->handle, &tuple, &parse);
		}
	}
#endif

#if 0
	printk(KERN_INFO "memory_mtd: mem%d:", nd);
	if ((nr[0] == 0) && (nr[1] == 0)) {
		cisinfo_t cisinfo;
		if ((CardServices(ValidateCIS, link->handle, &cisinfo)
		     == CS_SUCCESS) && (cisinfo.Chains == 0)) {
			dev->direct.cardsize =
				force_size ? force_size : get_size(link,&dev->direct);
			printk(" anonymous: ");
			if (dev->direct.cardsize == 0) {
				dev->direct.cardsize = HIGH_ADDR;
				printk("unknown size");
			} else {
				print_size(dev->direct.cardsize);
			}
		} else {
			printk(" no regions found.");
		}
	} else {
		for (attr = 0; attr < 2; attr++) {
			minor = dev->minor + attr*MAX_PART;
			if (attr && nr[0] && nr[1])
				printk(",");
			if (nr[attr])
				printk(" %s", attr ? "attribute" : "common");
			for (i = 0; i < nr[attr]; i++) {
				printk(" ");
				print_size(minor[i].region.RegionSize);
			}
		}
	}
	printk("\n");

#endif
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
		iounmap(dev->Base);
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
