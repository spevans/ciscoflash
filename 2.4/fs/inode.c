
/*
 * $Id: inode.c,v 1.2 2002-05-21 14:39:58 spse Exp $
 */


#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/locks.h>

#include <linux/mtd/compatmac.h>
#include <linux/mtd/mtd.h>                                             

#include "infoblock.h"
#include "fileheader.h"

#if CONFIG_MODVERSION==1
#define MODVERSIONS
#include <linux/modversions.h>
#endif  


static int pc_debug = 2;
#undef DEBUG
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG "ciscoffs: " args)

static void ciscoffs_read_inode(struct inode *i);
static int ciscoffs_statfs(struct super_block *sb, struct statfs *buf);
static int ciscoffs_readdir(struct file *filp, void *dirent, filldir_t filldir);
static int ciscoffs_readpage(struct file *file, struct page *page);
static struct dentry *
ciscoffs_lookup(struct inode *dir, struct dentry *dentry);

typedef struct {
	int	magic;		/* 0xBAD00B1E */
	int	length;		/* file length in bytes */
	short	crc;		/* CRC16 */
	short	flags;
	int	date;		/* Unix date format */
	char	name[48];	/* filename */
} cb_filehdr;




static struct super_operations ciscoffs_ops = {
	read_inode:  ciscoffs_read_inode,
	statfs:      ciscoffs_statfs,
};

static struct file_operations ciscoffs_dir_ops = {
	read:		generic_read_dir,
	readdir:	ciscoffs_readdir,
};

static struct inode_operations ciscoffs_dir_inode_ops = {
	lookup:		ciscoffs_lookup,
};

static struct address_space_operations ciscoffs_aops = {
	readpage: ciscoffs_readpage,
};

/* Headers are on 4 byte boundries */
#define NEXT_HEADER(x)  (((x) + 3) & ~3)


/* Called by the VFS at mount time to initialize the whole file system.  */
static struct super_block *
ciscoffs_read_super(struct super_block *sb, void *data, int silent)
{		
	kdev_t dev = sb->s_dev;
	uint32_t magic;
	int magiclen = 0;
	struct mtd_info *mtd;

	DEBUG(1, "Trying to mount device %s.\n", kdevname(dev));
	if (MAJOR(dev)!=MTD_BLOCK_MAJOR) {
		printk(KERN_WARNING "ciscoffs: Trying to mount non-mtd device.\n");
		return 0;
	}

	/* Get the device */
	DEBUG(1, "getting mtd device\n");
	mtd = get_mtd_device(NULL, MINOR(dev));
	DEBUG(1, "done, mtd = %p\n", mtd);
	if(!mtd) {
		printk(KERN_WARNING "ciscoffs: Cant get MTD device major = %d minor = %d\n", MAJOR(dev), MINOR(dev));
		goto mount_err;
	}

	/* Read the magic */
	if(!mtd->read)
		goto mount_err;
	DEBUG(1, "reading magic\n");
	if((mtd->read(mtd, 0, 4, &magiclen, (char *)&magic) != 0) || magiclen != 4) {
		printk(KERN_WARNING "ciscoffs: cant read magic");
		goto mount_err;
	}
	DEBUG(1, "magic read\n");
	magic = ntohl(magic);
	DEBUG(1, "Found magic: %08X\n", magic);
  
	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->u.generic_sbp = mtd;
	sb->s_magic = magic;
	sb->s_flags |= MS_RDONLY;
	sb->s_op = &ciscoffs_ops;
	DEBUG(1, "Getting root dentry\n");
	sb->s_root = d_alloc_root(iget(sb, 0xfffffff0));
	DEBUG(1, "sb setup @ %p, mounted ok\n", sb);
	DEBUG(1, "mtd @ %p\n", sb->u.generic_sbp);
	return sb;

 mount_err:
	DEBUG(1, "mount error");
	if(mtd)
		put_mtd_device(mtd);
	MOD_DEC_USE_COUNT;
	printk(KERN_WARNING "ciscoffs: Failed to mount device %s.\n",
	       kdevname(dev));
	return 0;

}

static int ciscoffs_readpage(struct file *file, struct page *page)
{
	unsigned long offset, avail, readlen;
	void *buf;
	struct inode *inode = page->mapping->host;
	struct mtd_info *mtd = inode->i_sb->u.generic_sbp;   
	int result = -EIO;

	DEBUG(1, __FUNCTION__ ": inode = %ld, page offset = %ld\n",
	      inode->i_ino, page->index);
  
	page_cache_get(page);
	buf = kmap(page);
	if (!buf)
		goto err_out;
  
	/* 32 bit warning -- but not for us :) */
	offset = page->index << PAGE_CACHE_SHIFT;
	if (offset < inode->i_size) {
		int len;
		avail = inode->i_size-offset;
		readlen = min_t(unsigned long, avail, PAGE_SIZE);
		offset += inode->i_ino + sizeof(cb_filehdr);
		DEBUG(1, __FUNCTION__ ": offset = %ld readlen = %ld\n", offset, readlen);


		if ((mtd->read(mtd, offset, readlen, &len, buf) == 0) &&
		    len == readlen) {
			if (readlen < PAGE_SIZE) {
				memset(buf + readlen,0,PAGE_SIZE-readlen);
			}
			SetPageUptodate(page);
			result = 0;
		}
	}
	DEBUG(1, __FUNCTION__ ": result = %d\n", result);
	if (result) {
		memset(buf, 0, PAGE_SIZE);
		SetPageError(page);
	}
	flush_dcache_page(page);
  
	UnlockPage(page);
  
	kunmap(page);
 err_out:
	page_cache_release(page);
	return result; 
}



static void ciscoffs_read_inode(struct inode *i)
{
	struct mtd_info *mtd = i->i_sb->u.generic_sbp; 
	cb_filehdr fh;
	int hdrlen;
	DEBUG(1, __FUNCTION__ "\n");
	DEBUG(1, "Inode number wanted: %lu\n", i->i_ino);

	switch(i->i_ino) {

	case 0xfffffff0:
		/* Fake dir */
		i->i_nlink = 1;
		i->i_size = 16;
		i->i_mtime = i->i_atime = i->i_ctime = 0;
		i->i_uid = i->i_gid = 0; 
		i->i_op = &ciscoffs_dir_inode_ops;
		i->i_fop = &ciscoffs_dir_ops;
		i->i_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
		break;

	default:
		if((mtd->read(mtd, i->i_ino, sizeof(cb_filehdr), &hdrlen, (char *)&fh) != 0) 
		   || hdrlen != sizeof(cb_filehdr)) {
			printk(KERN_WARNING "ciscoffs: cant read magic");
		}
		if(ntohl(fh.magic) != CISCO_FH_MAGIC) {
			printk(KERN_WARNING "ciscoffs: bad inode %ld\n", i->i_ino);
		}
		i->i_nlink = 1;
		i->i_size = ntohl(fh.length);
		i->i_mtime = i->i_atime = i->i_ctime = ntohl(fh.date);
		i->i_uid = i->i_gid = 0; 
		i->i_fop = &generic_ro_fops;
		i->i_data.a_ops = &ciscoffs_aops;
		i->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;   
		DEBUG(1, __FUNCTION__ ": found inode %ld\n", i->i_ino);
		break;
    
	}
}

static int ciscoffs_statfs(struct super_block *sb, struct statfs *buf)
{
	struct mtd_info *mtd = sb->u.generic_sbp;

	DEBUG(1, __FUNCTION__ "\n");
	buf->f_type = CISCO_FH_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_bfree = buf->f_bavail = 0;
	buf->f_blocks = mtd->size >> sb->s_blocksize;
	buf->f_namelen = 48;

	return 0;
}


static int ciscoffs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *i = filp->f_dentry->d_inode; 
	struct super_block *sb = i->i_sb;
	struct mtd_info *mtd = sb->u.generic_sbp; 
	cb_filehdr fh;
	int hdrlen;

	int stored = 0;

	DEBUG(1, __FUNCTION__ " inode = %lu filp->f_pos = %lld sb = %p mtd = %p\n",
	      i->i_ino, filp->f_pos, sb, mtd);

	if(i->i_ino == 0xfffffff0 && filp->f_pos == 0xffffffff) {
		return 0;
	}

	/* Fake up . and .. */
	if(i->i_ino == 0xfffffff0 && !filp->f_pos) {
		if(filldir(dirent, ".", 1, 0, 0xfffffff0, DT_DIR) < 0)
			return 0;

		stored++;
		filp->f_pos = 1;
	}

	if(i->i_ino == 0xfffffff0 && filp->f_pos == 1) {
		if(filldir(dirent, "..", 2, 0, filp->f_dentry->d_parent->d_inode->i_ino, DT_DIR)< 0)
			return stored;

		stored++;
		filp->f_pos = 2;
	}

	if(filp->f_pos >= 2) {
		if(filp->f_pos == 2) {
			filp->f_pos = 0;
		} else {
			filp->f_pos = NEXT_HEADER(filp->f_pos);
		}

		if((mtd->read(mtd, filp->f_pos, sizeof(cb_filehdr), &hdrlen, (char *)&fh) != 0) 
		   || hdrlen != sizeof(cb_filehdr)) {
			printk(KERN_WARNING "ciscoffs: cant read magic");
			return 0;
		}
		DEBUG(1, __FUNCTION__ " : magic = 0x%8X\n", ntohl(fh.magic));
		while(ntohl(fh.magic) == CISCO_FH_MAGIC) {
			DEBUG(1, __FUNCTION__ " :found file %s len = %d f_pos = %lu\n", 
			      fh.name, ntohl(fh.length), (unsigned long)filp->f_pos);
			if(filldir(dirent, fh.name, strlen(fh.name)+1, 0, 
				   filp->f_pos+sizeof(cb_filehdr), 1) < 0)
				return stored;
      
			stored++;
			filp->f_pos += ntohl(fh.length) + sizeof(cb_filehdr);
      
			if(filp->f_pos >= mtd->size)
				return stored;
      
			if((mtd->read(mtd, filp->f_pos, sizeof(cb_filehdr), &hdrlen, (char *)&fh) != 0)
			   || hdrlen != sizeof(cb_filehdr)) {
				printk(KERN_WARNING "ciscoffs: cant read magic");
				filp->f_pos = 0xffffffff;
			}
		}
	}

	return 0;
}

static struct dentry *
ciscoffs_lookup(struct inode *dir, struct dentry *dentry)
{
	cb_filehdr fh;
	int hdrlen;
	struct mtd_info *mtd = dir->i_sb->u.generic_sbp; 
	unsigned long offset = dir->i_ino;
	int res = -EACCES;
	struct inode *inode;

	DEBUG(1, __FUNCTION__ " looking for file %s in dir inode %ld, mtd = %p\n",
	      dentry->d_name.name, offset, mtd);
	if(offset == 0xfffffff0)
		offset = 0;


	if((mtd->read(mtd, offset, sizeof(cb_filehdr), &hdrlen, (char *)&fh) != 0) 
	   || hdrlen != sizeof(cb_filehdr)) {
		printk(KERN_WARNING "ciscoffs: cant read magic");
		return ERR_PTR(res);
	}
	while(ntohl(fh.magic) == CISCO_FH_MAGIC) {
		if(!strcmp(fh.name, dentry->d_name.name)) {
			inode = iget(dir->i_sb, offset);
			d_add(dentry, inode);
			return ERR_PTR(0);
		}

		offset += sizeof(cb_filehdr) + ntohl(fh.length);
		offset = NEXT_HEADER(offset);
		if(offset >= mtd->size)
			goto nofile;

		if((mtd->read(mtd, offset, sizeof(cb_filehdr), &hdrlen, (char *)&fh) != 0) 
		   || hdrlen != sizeof(cb_filehdr)) {
			printk(KERN_WARNING "ciscoffs: cant read magic");
			goto err;
		}
	}

 nofile:
	d_add(dentry, NULL);
	return ERR_PTR(0);

 err:
	return ERR_PTR(res);
}


static DECLARE_FSTYPE_DEV(ciscoffs_fs_type, "ciscoffs", ciscoffs_read_super);


mod_init_t init_ciscoffs_fs(void)
{
	printk("ciscoffs: $Revision: 1.2 $\n");
	return register_filesystem(&ciscoffs_fs_type);
}


mod_exit_t exit_ciscoffs_fs(void)
{
	unregister_filesystem(&ciscoffs_fs_type);
}


module_init(init_ciscoffs_fs);
module_exit(exit_ciscoffs_fs);
EXPORT_NO_SYMBOLS;
MODULE_LICENSE("GPL");

