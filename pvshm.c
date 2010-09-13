/*
 * Copyright (c) 2005 Bryan W. Lewis <blewis@illposed.net>
 *
 * This program (pvshm) is free software; you can redistribute 
 * it and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Pvshm is an experimental shadow file system that intercepts memory-mapped 
 * operations and replaces them with file read/write calls to another file 
 * system. The pvshm file system only supports mmap operations and does not
 * implement traditional read nor write operations. See the design document
 * for more information.
 *
 * Yes, I know one is not to access userspace file systems from the kernel.
 * I plan to eventually separate userspace file functions out into a daemon,
 * similarly to fuse.
 *
 * OK, I know what you're about to ask: why not just use fuse? The fuse
 * (experimental) writable mmap code is not easy to follow, and is focused
 * on a very general-purpose, cache-consistent model, and I'm not sure if it
 * is still even being developed. We intentionally dispense with consistency 
 * leaving that to the applications anyway, and try to keep things very simple.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>

#define BYTETOBINARY(byte)  \
  (byte & 0x80 ? 1 : 0), \
  (byte & 0x40 ? 1 : 0), \
  (byte & 0x20 ? 1 : 0), \
  (byte & 0x10 ? 1 : 0), \
  (byte & 0x08 ? 1 : 0), \
  (byte & 0x04 ? 1 : 0), \
  (byte & 0x02 ? 1 : 0), \
  (byte & 0x01 ? 1 : 0)

#define PVSHM_MAGIC	0x55566655

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
static inline int
trylock_page (struct page *page)
{
  return (likely (!test_and_set_bit (PG_locked, &page->flags)));
}
#endif

int verbose = 0;

/* Superblock and file inode operations */
// XXX These should really be static const, but causes compatibility
//     problems with older kernels.
struct inode_operations pvshm_dir_inode_operations;
struct inode_operations pvshm_file_inode_operations;
static int pvshm_get_sb (struct file_system_type *fs_type,
                         int flags, const char *dev_name, void *data,
                         struct vfsmount *mnt);
struct inode *pvshm_iget (struct super_block *sp, unsigned long ino);

/* Address space operations */
static int pvshm_writepage (struct page *page, struct writeback_control *wbc);
static int pvshm_readpage (struct file *file, struct page *page);
static int pvshm_set_page_dirty_nobuffers (struct page *page);
static int pvshm_releasepage (struct page *page, gfp_t gfp_flags);

/* File operations */
static int pvshm_file_mmap (struct file *, struct vm_area_struct *);
static int pvshm_sync_file (struct file *, struct dentry *, int);
static ssize_t pvshm_read (struct file *, char __user *, size_t, loff_t *);
ssize_t pvshm_write (struct file *, const char __user *, size_t, loff_t *);

/*
 * path: The target file full path
 * file: The target file stream 
 * Stored in each pvshm inode private field
 */
typedef struct
{
  char *path;
  loff_t max_size;
  struct file *file;
} pvshm_target;


const struct super_operations pvshm_sops = {
  .statfs  = simple_statfs,
};

const struct address_space_operations pvshm_aops = {
  .readpage = pvshm_readpage,
  .writepage = pvshm_writepage,
  .writepages = generic_writepages,
  .set_page_dirty = pvshm_set_page_dirty_nobuffers,
  .releasepage = pvshm_releasepage,
};

const struct file_operations pvshm_file_operations = {
  .mmap = pvshm_file_mmap,
  .fsync = pvshm_sync_file,
  .read = pvshm_read,
  .write = pvshm_write,
};

struct inode_operations pvshm_file_inode_operations = {
//  .setattr = simple_setattr,
  .getattr = simple_getattr,
};

// XXX Not presently used, but may be in the future to disable/adjust readahead.
//static struct backing_dev_info pvshm_backing_dev_info = {
//  .ra_pages = 0,
//  .capabilities = BDI_CAP_SWAP_BACKED;
//};

/* Inode operations */

struct inode *
pvshm_iget (struct super_block *sb, unsigned long ino)
{
  struct inode *inode;
  inode = iget_locked (sb, ino);
  if (!inode)
    return ERR_PTR (-ENOMEM);
  unlock_new_inode (inode);
  return inode;
}


/* File operations */

void
pvshm_read_again (struct file *file, struct address_space *mapping,
                  pgoff_t start, pgoff_t end)
{
  struct pagevec pvec;
  pgoff_t next = start;
  int i;
  pagevec_init (&pvec, 0);
  while (next <= end && pagevec_lookup (&pvec, mapping, next, PAGEVEC_SIZE))
    {
      for (i = 0; i < pagevec_count (&pvec); i++)
        {
          struct page *page = pvec.pages[i];
          int lock_failed;
          pgoff_t index;
          lock_failed = trylock_page (page);
          index = page->index;
          if (index > next)
            next = index;
          next++;
          ClearPageUptodate (page);
          if (verbose)
            printk ("read_again: page->index=%d %s %s\n",
                    (int) page->index,
                    PageUptodate (page) ? "Uptodate" :
                    "Not Uptodate",
                    PageLocked (page) ? "Locked" : "Unlocked");
          if (PageLocked (page))
            unlock_page (page);
          pvshm_readpage (file, page);
          if (next > end)
            break;
        }
      pagevec_release (&pvec);
    }
}

/* Pvshm uses the read function for two purposes:
 * 1. Provide a (somewhat slow) traditional read operation against
 *    the backing file.
 * 2. Provide a mechanism for forcing a page cache update from the
 *    backing file.
 * The second purpose provides a sort of reverse msync to applications,
 * especially useful with parallel file systems. It's triggered exactly
 * like read, but with a NULL buffer. The pages containing the read
 * request will be updated from the backing file to the page cache.
 */
static ssize_t
pvshm_read (struct file *filp, char __user * buf, size_t len, loff_t * skip)
{
  loff_t lstart, lend;
  pgoff_t pstart, pend;
  mm_segment_t old_fs;
  ssize_t ret = -EBADF;
  struct inode *inode = filp->f_mapping->host;
  struct address_space *mapping = inode->i_mapping;
  pvshm_target *pvmd = (pvshm_target *) inode->i_private;
  if (!pvmd)
    goto out;
  lstart = *skip;
  lend = lstart + (loff_t) len;
  pstart = (lstart + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
  pend = (lend >> PAGE_CACHE_SHIFT);
  if (verbose)
    printk ("pvshm_read pstart=%d pend=%d ", (int) pstart, (int) pend);
  if (verbose)
    printk ("nrpages=%d \n", (int) mapping->nrpages);
  mutex_lock (&filp->f_mapping->host->i_mutex);
// I tried simply the following first, but it's not enough. Note that we are
// generally not free to eject the page either as it may be in use by somebody,
// hence the pvshm_read_again function.
//      invalidate_mapping_pages (mapping, pstart, pend); 
  pvshm_read_again (filp, mapping, pstart, pend);
  mutex_unlock (&filp->f_mapping->host->i_mutex);
  if (verbose)
    printk ("pvshm_read cache update OK\n");
// A non-zero buffer address triggers a standard read operation on
// the backing file:
  if (buf)
    {
      old_fs = get_fs ();
      set_fs (KERNEL_DS);
      ret = vfs_read (pvmd->file, (char __user *) buf, len, skip);
      set_fs (old_fs);
      if (verbose)
        printk ("pvshm_read backing file into user buffer\n");
    }
out:
  return ret;
}

/* The pvshm_write function passes the write operation to the backing file. */
ssize_t
pvshm_write (struct file * filp, const char __user * buf, size_t len,
             loff_t * skip)
{
  mm_segment_t old_fs;
  ssize_t ret = -EBADF;
  struct inode *inode = filp->f_mapping->host;
  pvshm_target *pvmd = (pvshm_target *) inode->i_private;
  if (!pvmd)
    goto out;
  if (buf)
    {
      old_fs = get_fs ();
      set_fs (KERNEL_DS);
      ret = vfs_write (pvmd->file, (char __user *) buf, len, skip);
      set_fs (old_fs);
      if (verbose)
        printk ("pvshm_write to backing file %s\n", pvmd->path);
    }
out:
  return ret;
}

static int
pvshm_file_mmap (struct file *f, struct vm_area_struct *v)
{
  int ret = -EBADF;
  pvshm_target *pvmd = (pvshm_target *) f->f_mapping->host->i_private;
  if(!pvmd) goto out;
  if (verbose)
    printk ("pvshm_file_mmap %s\n", pvmd->path);
  ret = generic_file_mmap (f, v);
out:
  return ret;
}

static int
pvshm_sync_file (struct file *f, struct dentry *d, int k)
{
  pvshm_target *pv_tgt;
  int j = -EBADF;
  struct inode *inode = f->f_mapping->host;
  pv_tgt = (pvshm_target *) inode->i_private;
  if(!pv_tgt) goto out;
  if (verbose)
    printk ("pvshm_sync_file %s\n", pv_tgt->path);
  j = filemap_write_and_wait (f->f_mapping);
out:
  return j;
}


/* inode operations */
struct inode *
pvshm_get_inode (struct super_block *sb, int mode, dev_t dev)
{
  struct inode *inode = new_inode (sb);
  if (inode)
    {
      inode->i_mode = mode;
//      inode->i_uid = current->fsuid;
//      inode->i_gid = current->fsgid;
      inode->i_blocks = 0;
      inode->i_mapping->a_ops = &pvshm_aops;
//      inode->i_mapping->backing_dev_info = &pvshm_backing_dev_info;
      inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
      switch (mode & S_IFMT)
        {
        default:
          init_special_inode (inode, mode, dev);
          break;
        case S_IFREG:
          inode->i_op = &pvshm_file_inode_operations;
          inode->i_fop = &pvshm_file_operations;
          break;
        case S_IFDIR:
          inode->i_op = &pvshm_dir_inode_operations;
          inode->i_fop = &simple_dir_operations;
          inc_nlink (inode);
          break;
        case S_IFLNK:
          inode->i_op = &pvshm_file_inode_operations;
          inode->i_fop = &pvshm_file_operations;
          break;
        }
    }
  if (verbose)
    printk ("pvshm_get_inode capabilities=%d%d%d%d%d%d%d%d\n",
            BYTETOBINARY (inode->i_mapping->backing_dev_info->capabilities));
  return inode;
}

static int
pvshm_mknod (struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
  int error = -ENOSPC;
  struct inode *inode = pvshm_get_inode (dir->i_sb, mode, dev);
  if (verbose)
    printk ("pvshm_mknod d_name=%s\n", dentry->d_name.name);

  if (inode)
    {
      if (dir->i_mode & S_ISGID)
        {
          inode->i_gid = dir->i_gid;
          if (S_ISDIR (mode))
            inode->i_mode |= S_ISGID;
        }
      d_instantiate (dentry, inode);
      dget (dentry);
      error = 0;
      dir->i_mtime = dir->i_ctime = CURRENT_TIME;
    }
  return error;
}

static int
pvshm_mkdir (struct inode *dir, struct dentry *dentry, int mode)
{
  int retval = pvshm_mknod (dir, dentry, mode | S_IFDIR, 0);
  if (!retval)
    inc_nlink (dir);
  return retval;
}

static int
pvshm_create (struct inode *dir, struct dentry *dentry, int mode,
              struct nameidata *nd)
{
  return pvshm_mknod (dir, dentry, mode | S_IFREG, 0);
}

/* pvshm_unlink 
 * Remove the inode, de-allocate housekeeping storage for its target,
 * close the open file descriptor to the target. 
 */
static int
pvshm_unlink (struct inode *dir, struct dentry *d)
{
  struct inode *ino = d->d_inode;
  pvshm_target *pvmd = (pvshm_target *) ino->i_private;
  if (pvmd)
    {
      if (verbose)
        printk ("pvshm_unlink %s\n", pvmd->path);
      if (pvmd->file)
        filp_close (pvmd->file, current->files);
      kfree (pvmd->path);
      kfree (pvmd);
    }
  return simple_unlink (dir, d);
}

/* Create a pvshm entry and set up a mapping between the pvshm file 
 * and the target file in specified by symname.
 * 
 * Open a r/w file stream to the target.
 */
static int
pvshm_symlink (struct inode *dir, struct dentry *dentry, const char *symname)
{
  struct inode *inode;
  int error = -ENOSPC;
  ino_t j = iunique (dir->i_sb, 0);
  pvshm_target *pvmd = (pvshm_target *) kmalloc (sizeof (pvshm_target), 0);

  struct kstat stat;
  mm_segment_t old_fs = get_fs ();
  set_fs (KERNEL_DS);
  error = vfs_stat ((char *) symname, &stat);
  set_fs (old_fs);
  if (error)
    {
      if (verbose)
        printk ("pvshm_symlink can't stat target file\n");
      kfree (pvmd);
      goto end;
    }
  pvmd->max_size = stat.size;

  inode = pvshm_iget (dir->i_sb, j);
//      inode->i_mode= S_IFREG | S_IRWXUGO;
  inode->i_mode = stat.mode;
  inode->i_uid = stat.uid;
  inode->i_gid = stat.gid;
  inode->i_fop = &pvshm_file_operations;
  inode->i_mapping->a_ops = &pvshm_aops;
//  inode->i_mapping->backing_dev_info = &pvshm_backing_dev_info;
  if (verbose)
    printk ("pvshm_symlink d_name=%s, symname=%s\n",
            dentry->d_name.name, symname);
  if (inode)
    {
      int l = strlen (symname) + 1;
// We don't do this: page_symlink(inode, symname, l);
// The standard approach of putting the symlink name in page 0 does not 
// work in this case since we use all pages in the mapping.  We allocate 
// space for the file name and store it in the inode private field.
      error = 0;
      pvmd->path = (char *) kmalloc (l, 0);
      memcpy (pvmd->path, symname, l);
// Open a file stream now
      pvmd->file = filp_open (symname, O_RDWR | O_LARGEFILE, 0644);
      if (!pvmd->file)
        {
          error = -EBADF;
          if (verbose)
            printk ("pvshm_symlink symname=%s fget error\n", symname);
// XXX add code to better handle errors here!
        }

      inode->i_private = pvmd;
      if (!error)
        {
          if (dir->i_mode & S_ISGID)
            inode->i_gid = dir->i_gid;
          inode->i_size = stat.size;
          d_instantiate (dentry, inode);
          dget (dentry);
          dir->i_mtime = dir->i_ctime = CURRENT_TIME;
        }
      else
        iput (inode);
    }

end:
  return error;
}

struct inode_operations pvshm_dir_inode_operations = {
  .create = pvshm_create,
  .link = simple_link,
  .unlink = pvshm_unlink,
  .symlink = pvshm_symlink,
  .mkdir = pvshm_mkdir,
  .rmdir = simple_rmdir,
  .mknod = pvshm_mknod,
  .rename = simple_rename,
  .lookup = simple_lookup,
//  .setattr = simple_setattr,
};

static struct file_system_type pvshm_fs_type = {
  .name = "pvshm",
  .get_sb = pvshm_get_sb,
  .kill_sb = kill_litter_super,
  .owner = THIS_MODULE,
};

static int
pvshm_fill_super (struct super_block *sb, void *data, int silent)
{
  static struct inode *pvshm_root_inode;
  struct dentry *root;

  if (verbose)
    printk ("pvshm_fill_super\n");
  sb->s_maxbytes = MAX_LFS_FILESIZE;
  sb->s_blocksize = PAGE_CACHE_SIZE;
  sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
  sb->s_magic = PVSHM_MAGIC;
  sb->s_op = &pvshm_sops;
  sb->s_type = &pvshm_fs_type;
  sb->s_time_gran = 1;
  pvshm_root_inode = pvshm_get_inode (sb, S_IFDIR | 0755, 0);
  if (!pvshm_root_inode)
    return -ENOMEM;

  root = d_alloc_root (pvshm_root_inode);
  if (!root)
    {
      iput (pvshm_root_inode);
      return -ENOMEM;
    }
  sb->s_root = root;
  return 0;
}

int
pvshm_get_sb (struct file_system_type *fs_type,
              int flags, const char *dev_name, void *data,
              struct vfsmount *mnt)
{
  return get_sb_nodev (fs_type, flags, data, pvshm_fill_super, mnt);
}

static int
pvshm_set_page_dirty_nobuffers (struct page *page)
{
  int j = 0;
  j = __set_page_dirty_nobuffers (page);
  if (verbose)
    printk ("pvshm_spdirty_nb: %d [%s] [%s] [%s] [%s]\n",
            (int) page->index,
            PageUptodate (page) ? "Uptodate" : "Not Uptodate",
            PageDirty (page) ? "Dirty" : "Not Dirty",
            PageWriteback (page) ? "PWrbk Set" : "PWrbk Cleared",
            PageLocked (page) ? "Locked" : "Unlocked");
  return j;
}

static int
pvshm_releasepage (struct page *page, gfp_t gfp_flags)
{
  if (verbose)
    {
      printk ("pvshm_releasepage private = %d\n", PagePrivate (page));
    }
  return 0;
}

static int
pvshm_writepage (struct page *page, struct writeback_control *wbc)
{
  ssize_t j;
//  struct pagevec pv;
  loff_t offset;
  mm_segment_t old_fs;
  struct inode *inode;
  void *page_addr;
  pvshm_target *pvmd;

  inode = page->mapping->host;
  pvmd = (pvshm_target *) inode->i_private;
//  page_addr = kmap (page);
  offset = page->index << PAGE_CACHE_SHIFT;
  j = 1;
  test_set_page_writeback (page);
  if (pvmd->file)
    {
      page_addr = kmap_atomic (page, KM_USER0);
      if (verbose)
        printk ("About to write idx %d pageaddr %p\n", (int) page->index,
                page_addr);
      old_fs = get_fs ();
      set_fs (get_ds ());
      j =
        vfs_write (pvmd->file, (char __user *) page_addr, PAGE_SIZE, &offset);
//      written = do_sync_write (target->file, p, PAGE_SIZE, &offset);
      set_fs (old_fs);
      kunmap_atomic (page_addr, KM_USER0);
    }
//  kunmap (page);
  end_page_writeback (page);
  if (PageError (page))
    ClearPageError (page);
  if (PageLocked (page))
    unlock_page (page);
  if (verbose)
    printk ("pvshm_writepage: %d link=%s [%s] [%s] [%s] [%s] [%s] %d\n",
            (int) page->index,
            (char *) pvmd->path,
            PageUptodate (page) ? "Uptodate" : "Not Uptodate",
            PageDirty (page) ? "Dirty" : "Not Dirty",
            PagePrivate (page) ? "Private" : "Not Private",
            PageReferenced (page) ? "Referenced" : "Not Referenced",
            PageLocked (page) ? "Locked" : "Unlocked", page_count (page));
  return 0;
}

static int
pvshm_readpage (struct file *file, struct page *page)
{
  void *page_addr;
  loff_t offset;
  mm_segment_t old_fs;
  int j;
  struct inode *inode = file->f_mapping->host;
  pvshm_target *pvmd = (pvshm_target *) inode->i_private;
  if (verbose)
    printk ("pvshm_readpage %d %s %ld [%s] [%s] [%s]\n",
            (int) page->index,
            (char *) pvmd->path,
            page->mapping->nrpages,
            PageUptodate (page) ? "Uptodate" : "Not Uptodate",
            PageDirty (page) ? "Dirty" : "Not Dirty",
            PageLocked (page) ? "Locked" : "Unlocked");
//  page_addr = kmap (page);
  page_addr = kmap_atomic (page, KM_USER0);
//  page_addr = page_address (page);
  if (page_addr)
    {
      j = 0;
      offset = page->index << PAGE_CACHE_SHIFT;
      if (verbose)
        printk ("pvshm_readpage offset=%ld, page_addr=%p\n", (long) offset,
                page_addr);
      if (pvmd->file)
        {
          old_fs = get_fs ();
          set_fs (KERNEL_DS);
          j =
            vfs_read (pvmd->file, (char __user *) page_addr, PAGE_SIZE,
                      &offset);
          set_fs (old_fs);
        }
      if (verbose)
        printk ("readpage %d bytes at index %d complete\n", j,
                (int) page->index);
/* XXX It may be that the backing file is not a multiple of the page size,
 * resulting in j < PAGE_SIZE. We should probably clear the remainder of
 * the page here, or prior to this with page_zero...
 */
      kunmap_atomic (page_addr, KM_USER0);
      SetPageUptodate (page);
    }
  if (PageLocked (page))
    unlock_page (page);
//  kunmap (page);
  return 0;
}

static int __init
init_pvshm_fs (void)
{
  if (verbose)
    printk ("\n----------pvshm---danger---------------------------\n");
  return register_filesystem (&pvshm_fs_type);
}

static void __exit
exit_pvshm_fs (void)
{
  if (verbose)
    printk ("\n----------pvshm---relax---------------------------\n");
  unregister_filesystem (&pvshm_fs_type);
}

module_init (init_pvshm_fs);
module_exit (exit_pvshm_fs);

module_param (verbose, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC (verbose, "1 -> verbose on");

MODULE_AUTHOR ("Bryan Wayne Lewis");
MODULE_LICENSE ("GPL");
