#include <linux/fs.h>
#include <linux/file.h>
#include <linux/device.h>
#include <linux/uio.h>
#include <host_ops.h>
#include <init.h>
#include "core.h"

#define CHAR_IO_DISPATCH_ROUTINE_PROLOGUE(ntfile, _filp, _inode)	\
	struct file *_filp = ntos_get_file_extension(ntfile);		\
	BUG_ON(!_filp);							\
	struct inode *_inode = _filp->f_inode;				\
	BUG_ON(!_inode);						\
	BUG_ON(_inode->i_rdev != dev->devt)

static bool match(struct device *dev)
{
	return !!dev->devt;
}

static int chardev_dispatch_create(struct device *dev, void *irp, void *file_object)
{
	struct file *filp = NULL;
	BUG_ON(!dev || !dev->devt);

	/*
	 * Create a synthetic inode.
	 * Pseudo inode is sufficient because chrdev_open only needs i_rdev.
	 */
	int ret;
	struct inode *inode = new_inode_pseudo(&pseudo_sb);
	if (!inode) {
		ret = -ENOMEM;
		goto out;
	}

	inode->i_mode = S_IFCHR;
	inode->i_rdev = dev->devt;
	inode->i_fop  = &def_chr_fops;

	int mode = get_file_mode(file_object);
	const char *name = ntos_get_file_name(file_object);
	filp = alloc_file_pseudo(inode, &pseudo_vfsmount, name, mode, &def_chr_fops);
	if (name) {
		ntos_free_pool((void *)name);
	}
	if (IS_ERR(filp)) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Call the canonical char-device open resolver.
	 *
	 * This will:
	 *  - lookup cdev via cdev_map using inode->i_rdev
	 *  - set inode->i_cdev
	 *  - replace filp->f_op with the real driver fops
	 *  - call the driver's .open() if present
	 */
	ret = filp->f_op->open(inode, filp);
	if (ret) {
		goto err;
	}

	ntos_set_file_extension(file_object, filp);
	ret = 0;
	goto out;

err:
	if (filp) {
		fput(filp); /* drops file and inode refs correctly */
	} else if (inode) {
		iput(inode);
	}
out:
	return ret;
}

static ssize_t call_io_iter(struct file *filp,
			    void *buffer,
			    size_t len,
			    loff_t *pos,
			    bool write)
{
	struct kiocb kiocb;
	struct iov_iter iter;
	struct kvec kvec;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *pos;

	kvec.iov_base = buffer;
	kvec.iov_len  = len;

	iov_iter_kvec(&iter, write ? WRITE: READ, &kvec, 1, len);

	if (write) {
		ret = filp->f_op->write_iter(&kiocb, &iter);
	} else {
		ret = filp->f_op->read_iter(&kiocb, &iter);
	}

	*pos = kiocb.ki_pos;

	return ret;
}

static int chardev_dispatch_read_write(struct device *dev, void *irp, void *file_object,
				       unsigned long long file_offset,
				       void *buffer, unsigned int buffer_length,
				       unsigned long *pfn_db, unsigned int pfn_count,
				       int write, unsigned int *result_length)
{
	CHAR_IO_DISPATCH_ROUTINE_PROLOGUE(file_object, filp, inode);
	int ret = -ENODEV;
	/* TODO: Call kernel_read or kernel_write */
	if (!filp->f_op) {
		return ret;
	}
	loff_t pos = filp->f_pos;
	if (write) {
		if (filp->f_op->write) {
			ret = filp->f_op->write(filp, buffer, buffer_length, &pos);
		} else if (filp->f_op->write_iter) {
			ret = call_io_iter(filp, buffer, buffer_length, &pos, write);
		}
	} else {
		if (filp->f_op->read) {
			ret = filp->f_op->read(filp, buffer, buffer_length, &pos);
		} else if (filp->f_op->read_iter) {
			ret = call_io_iter(filp, buffer, buffer_length, &pos, write);
		}
	}
	if (ret >= 0) {
		filp->f_pos = pos;
		*result_length = ret;
		ret = 0;
	} else {
		*result_length = 0;
	}
	return ret;
}

static int chardev_dispatch_cleanup(struct device *dev, void *irp, void *file_object)
{
	struct file *file = ntos_get_file_extension(file_object);
	if (!file)
		return 0;
	fput(file);
	ntos_set_file_extension(file_object, NULL);
	return 0;
}

static int __init register_char_dev_type(void)
{
	lnxdrv_dev_types[LnxCharDev] = (struct lnxdrv_dev_type) {
		.match = match,
		.create = chardev_dispatch_create,
		.read_write = chardev_dispatch_read_write,
		.cleanup = chardev_dispatch_cleanup
	};
	return 0;
}
fs_initcall(register_char_dev_type);
