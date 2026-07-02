#include <linux/export.h>
#include <linux/stringhash.h>
#include <linux/path.h>
#include <linux/fs.h>

/* Return the hash of a string of known length */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
        unsigned long hash = init_name_hash(salt);
        while (len--)
                hash = partial_name_hash((unsigned char)*name++, hash);
        return end_name_hash(hash);
}
EXPORT_SYMBOL(full_name_hash);

/**
 * d_path - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns a pointer into the buffer or an error code if the path was
 * too long. Note: Callers should use the returned pointer, not the passed
 * in buffer, to use the name! The implementation often starts at an offset
 * into the buffer, and may leave 0 bytes at the start.
 *
 * "buflen" should be positive.
 */
char *d_path(const struct path *path, char *buf, int buflen)
{
	/*
	 * We have various synthetic filesystems that never get mounted.  On
	 * these filesystems dentries are never used for lookup purposes, and
	 * thus don't need to be hashed.  They also don't need a name until a
	 * user wants to identify the object in /proc/pid/fd/.  The little hack
	 * below allows us to generate a name for these objects on demand:
	 *
	 * Some pseudo inodes are mountable.  When they are mounted
	 * path->dentry == path->mnt->mnt_root.  In that case don't call d_dname
	 * and instead have d_path return the mounted path.
	 */
	return dentry_path(path->dentry, buf, buflen);
}
EXPORT_SYMBOL(d_path);

char *dentry_path(const struct dentry *dentry, char *buf, int buflen)
{
	if (dentry->d_op && dentry->d_op->d_dname)
		return dentry->d_op->d_dname((void *)dentry, buf, buflen);
	else
		return "(nil)";
}

char *file_path(struct file *filp, char *buf, int buflen)
{
	return d_path(&filp->f_path, buf, buflen);
}
EXPORT_SYMBOL(file_path);

/*
 * Helper function for dentry_operations.d_dname() members
 */
char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...)
{
        va_list args;
        char temp[64];
        int sz;

        va_start(args, fmt);
        sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
        va_end(args);

        if (sz > sizeof(temp) || sz > buflen)
                return ERR_PTR(-ENAMETOOLONG);

        buffer += buflen - sz;
        return memcpy(buffer, temp, sz);
}
