#include <linux/fs.h>
#include <linux/splice.h>

#include "../../../fs/internal.h"

/*
 * This is used by subsystems that don't want seekable
 * file descriptors. The function is not supposed to ever fail, the only
 * reason it returns an 'int' and not 'void' is so that it can be plugged
 * directly into file_operations structure.
 */
int nonseekable_open(struct inode *inode, struct file *filp)
{
        filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
        return 0;
}
EXPORT_SYMBOL(nonseekable_open);

bool file_seek_cur_needs_f_lock(struct file *file)
{
	if (!(file->f_mode & FMODE_ATOMIC_POS) && !file->f_op->iterate_shared)
		return false;

	/*
	 * Note that we are not guaranteed to be called after fdget_pos() on
	 * this file obj, in which case the caller is expected to provide the
	 * appropriate locking.
	 */

	return true;
}

/**
 * file_ns_capable - Determine if the file's opener had a capability in effect
 * @file:  The file we want to check
 * @ns:  The usernamespace we want the capability in
 * @cap: The capability to be tested for
 *
 * Return true if task that opened the file had a capability in effect
 * when the file was opened.
 *
 * This does not set PF_SUPERPRIV because the caller may not
 * actually be privileged.
 */
bool file_ns_capable(const struct file *file, struct user_namespace *ns,
                     int cap)
{
	return true;
}
EXPORT_SYMBOL(file_ns_capable);

/**
 * __put_cred - Destroy a set of credentials
 * @cred: The record to release
 *
 * Destroy a set of credentials on which no references remain.
 */
void __put_cred(struct cred *cred)
{
	/* Do nothing */
}
EXPORT_SYMBOL(__put_cred);

int kill_pid_usb_asyncio(int sig, int errno, sigval_t addr,
                         struct pid *pid, const struct cred *cred)
{
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(kill_pid_usb_asyncio);
