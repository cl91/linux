#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/cdev.h>
#include <linux/security.h>
#include <linux/securebits.h>
#include <linux/user_namespace.h>
#include <init.h>
#include <host_ops.h>

static struct kmem_cache *filp_cachep __ro_after_init;

static struct group_info init_groups = { .usage = REFCOUNT_INIT(2) };

struct ucounts init_ucounts = {
        .ns    = &init_user_ns,
        .uid   = GLOBAL_ROOT_UID,
        .count = RCUREF_INIT(1),
};

static struct cred full_cred = {
        .usage                  = ATOMIC_INIT(4),
        .uid                    = GLOBAL_ROOT_UID,
        .gid                    = GLOBAL_ROOT_GID,
        .suid                   = GLOBAL_ROOT_UID,
        .sgid                   = GLOBAL_ROOT_GID,
        .euid                   = GLOBAL_ROOT_UID,
        .egid                   = GLOBAL_ROOT_GID,
        .fsuid                  = GLOBAL_ROOT_UID,
        .fsgid                  = GLOBAL_ROOT_GID,
        .securebits             = SECUREBITS_DEFAULT,
        .cap_inheritable        = CAP_EMPTY_SET,
        .cap_permitted          = CAP_FULL_SET,
        .cap_effective          = CAP_FULL_SET,
        .cap_bset               = CAP_FULL_SET,
        .user                   = INIT_USER,
        .user_ns                = &init_user_ns,
        .group_info             = &init_groups,
        .ucounts                = &init_ucounts,
};

void __init init_file_table(void)
{
	struct kmem_cache_args args = {
		.use_freeptr_offset = true,
		.freeptr_offset = offsetof(struct file, f_freeptr),
	};

	filp_cachep = kmem_cache_create("filp", sizeof(struct file), &args,
					SLAB_HWCACHE_ALIGN | SLAB_PANIC |
					SLAB_ACCOUNT | SLAB_TYPESAFE_BY_RCU);
}

static int init_file(struct file *f, int flags)
{
	const struct cred *cred = &full_cred;
	int error;

	f->f_cred = get_cred(cred);
	error = security_file_alloc(f);
	if (unlikely(error)) {
		put_cred(f->f_cred);
		return error;
	}

	spin_lock_init(&f->f_lock);
	/*
	 * Note that f_pos_lock is only used for files raising
	 * FMODE_ATOMIC_POS and directories. Other files such as pipes
	 * don't need it and since f_pos_lock is in a union may reuse
	 * the space for other purposes. They are expected to initialize
	 * the respective member when opening the file.
	 */
	mutex_init(&f->f_pos_lock);
	memset(&f->__f_path, 0, sizeof(f->f_path));
	memset(&f->f_ra, 0, sizeof(f->f_ra));

	f->f_flags	= flags;
	f->f_mode	= OPEN_FMODE(flags);

	f->f_op		= NULL;
	f->f_mapping	= NULL;
	f->private_data = NULL;
	f->f_inode	= NULL;
	f->f_owner	= NULL;
#ifdef CONFIG_EPOLL
	f->f_ep		= NULL;
#endif

	f->f_iocb_flags = 0;
	f->f_pos	= 0;
	f->f_wb_err	= 0;
	f->f_sb_err	= 0;

	/*
	 * We're SLAB_TYPESAFE_BY_RCU so initialize f_ref last. While
	 * fget-rcu pattern users need to be able to handle spurious
	 * refcount bumps we should reinitialize the reused file first.
	 */
	file_ref_init(&f->f_ref, 1);
	/*
	 * Disable permission and pre-content events for all files by default.
	 * They may be enabled later by fsnotify_open_perm_and_set_mode().
	 */
	file_set_fsnotify_mode(f, FMODE_NONOTIFY_PERM);
	return 0;
}

/* Find an unused file structure and return a pointer to it.
 * Returns an error pointer if some error happend e.g. OOM.
 */
static struct file *alloc_empty_file(int flags)
{
	int error;

	struct file *f = kmem_cache_alloc(filp_cachep, GFP_KERNEL);
	if (unlikely(!f))
		return ERR_PTR(-ENOMEM);

	error = init_file(f, flags);
	if (unlikely(error)) {
		kmem_cache_free(filp_cachep, f);
		return ERR_PTR(error);
	}

	return f;
}

/**
 * d_alloc_pseudo - allocate a dentry (for lookup-less filesystems)
 * @sb: the superblock
 * @name: qstr of the name
 *
 * For a filesystem that just pins its dentries in memory and never
 * performs lookups at all, return an unhashed IS_ROOT dentry.
 * This is used for pipes, sockets et.al. - the stuff that should
 * never be anyone's children or parents.  Unlike all other
 * dentries, these will not have RCU delay between dropping the
 * last reference and freeing them.
 *
 * The only user is alloc_file_pseudo() and that's what should
 * be considered a public interface.  Don't use directly.
 */
static struct dentry *d_alloc_pseudo(struct super_block *sb, const char *name)
{
	struct dentry *dentry = kzalloc(sizeof(struct dentry), GFP_KERNEL);
        if (!dentry)
                return NULL;
	dentry->__d_name.name = kstrdup(name, GFP_KERNEL);
	dentry->__d_name.len = strlen(name);
	dentry->d_op = sb->__s_d_op;
        dentry->d_flags = sb->s_d_flags;
	lockref_init(&dentry->d_lockref);
	INIT_HLIST_BL_NODE(&dentry->d_hash);
        INIT_LIST_HEAD(&dentry->d_lru);
        INIT_HLIST_HEAD(&dentry->d_children);
        INIT_HLIST_NODE(&dentry->d_u.d_alias);
        INIT_HLIST_NODE(&dentry->d_sib);
        if (dentry->d_op && dentry->d_op->d_init) {
                int err = dentry->d_op->d_init(dentry);
                if (err) {
			kfree(dentry->__d_name.name);
                        kfree(dentry);
                        return NULL;
                }
        }
	return dentry;
}

static inline int alloc_path_pseudo(const char *name, struct inode *inode,
                                    struct vfsmount *mnt, struct path *path)
{
        path->dentry = d_alloc_pseudo(mnt->mnt_sb, name);
        if (!path->dentry)
                return -ENOMEM;
        path->mnt = mntget(mnt);
        d_instantiate(path->dentry, inode);
        return 0;
}

/**
 * file_init_path - initialize a 'struct file' based on path
 *
 * @file: the file to set up
 * @path: the (dentry, vfsmount) pair for the new file
 * @fop: the 'struct file_operations' for the new file
 */
static void file_init_path(struct file *file, const struct path *path,
                           const struct file_operations *fop)
{
        file->__f_path = *path;
        file->f_inode = path->dentry->d_inode;
        file->f_mapping = path->dentry->d_inode->i_mapping;
        if (fop->llseek)
                file->f_mode |= FMODE_LSEEK;
        if ((file->f_mode & FMODE_READ) &&
             likely(fop->read || fop->read_iter))
                file->f_mode |= FMODE_CAN_READ;
        if ((file->f_mode & FMODE_WRITE) &&
             likely(fop->write || fop->write_iter))
                file->f_mode |= FMODE_CAN_WRITE;
        file->f_iocb_flags = iocb_flags(file);
        file->f_mode |= FMODE_OPENED;
        file->f_op = fop;
        if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
                i_readcount_inc(path->dentry->d_inode);
}

/**
 * alloc_file - allocate and initialize a 'struct file'
 *
 * @path: the (dentry, vfsmount) pair for the new file
 * @flags: O_... flags with which the new file will be opened
 * @fop: the 'struct file_operations' for the new file
 */
static struct file *alloc_file(const struct path *path, int flags,
			       const struct file_operations *fop)
{
        struct file *file = alloc_empty_file(flags);
        if (!IS_ERR(file))
                file_init_path(file, path, fop);
        return file;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt,
                               const char *name, int flags,
                               const struct file_operations *fops)
{
        int ret;
        struct path path;
        struct file *file;

        ret = alloc_path_pseudo(name, inode, mnt, &path);
        if (ret)
                return ERR_PTR(ret);

        file = alloc_file(&path, flags, fops);
        if (IS_ERR(file)) {
                ihold(inode);
                path_put(&path);
                return file;
        }
        /*
         * Disable all fsnotify events for pseudo files by default.
         * They may be enabled by caller with file_set_fsnotify_mode().
         */
        file_set_fsnotify_mode(file, FMODE_NONOTIFY);
        return file;
}
EXPORT_SYMBOL(alloc_file_pseudo);

int get_file_mode(void *nt_file_object)
{
	fmode_t mode = 0;
	if (ntos_is_file_readable(nt_file_object)) {
		mode |= FMODE_READ;
	}
	if (ntos_is_file_writable(nt_file_object)) {
		mode |= FMODE_WRITE;
	}
	/* Most drivers assume seekable char devices */
	mode |= FMODE_LSEEK;
	return mode;
}

struct file *dentry_open(const struct path *path, int flags, const struct cred *cred)
{
	if (!path || !path->dentry)
		return ERR_PTR(-EINVAL);

	struct inode *inode = d_inode(path->dentry);
	if (!inode)
		return ERR_PTR(-ENXIO);

	struct file *f = alloc_empty_file(flags);
	if (IS_ERR(f))
		return f;

	f->__f_path = *path;
	/* Increment refcounts on dentry and vfsmount */
	path_get(&f->f_path);
	f->f_inode = inode;

	f->f_op = fops_get(inode->i_fop);
	f->f_flags = flags;

	int error;
	if (f->f_op && f->f_op->open) {
		error = f->f_op->open(inode, f);
		if (error) {
			goto err_cleanup;
		}
	}

	return f;

err_cleanup:
	path_put(&f->f_path);
	fput(f);
	return ERR_PTR(error);
}
EXPORT_SYMBOL(dentry_open);

static inline void file_free(struct file *f)
{
	security_file_free(f);
	kmem_cache_free(filp_cachep, f);
}

void fput(struct file *file)
{
	struct inode *inode = file->f_inode;
	fmode_t mode = file->f_mode;

	if (unlikely(!(file->f_mode & FMODE_OPENED)))
		goto out;

	might_sleep();

	security_file_release(file);
	if (unlikely(file->f_flags & FASYNC)) {
		if (file->f_op->fasync)
			file->f_op->fasync(-1, file, 0);
	}
	if (file->f_op->release)
		file->f_op->release(inode, file);
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL &&
		     !(mode & FMODE_PATH))) {
		cdev_put(inode->i_cdev);
	}
	fops_put(file->f_op);
out:
	if (file->f_inode) {
		iput(file->f_inode);
	}
	file_free(file);
}
EXPORT_SYMBOL(fput);

static DEFINE_IDR(global_fd_idr);
static DEFINE_MUTEX(global_fd_lock);

int get_unused_fd_flags(unsigned flags)
{
	idr_preload(GFP_KERNEL);
	mutex_lock(&global_fd_lock);
	/* Allocate an ID slot. We insert a dummy pointer (like NULL or an error pointer)
	 * to reserve the slot until fd_install attaches the real file structure. */
	int fd = idr_alloc(&global_fd_idr, NULL, 0, INT_MAX, GFP_NOWAIT);
	mutex_unlock(&global_fd_lock);
	idr_preload_end();
	/* idr_alloc returns standard errnos (like -ENOSPC if limit is reached) */
	return fd;
}
EXPORT_SYMBOL(get_unused_fd_flags);

void put_unused_fd(unsigned int fd)
{
	mutex_lock(&global_fd_lock);
	idr_remove(&global_fd_idr, fd);
	mutex_unlock(&global_fd_lock);
}
EXPORT_SYMBOL(put_unused_fd);

/**
 * fd_install - install a file pointer in the fd array
 * @fd: file descriptor to install the file in
 * @file: the file to install
 *
 * This consumes the "file" refcount, so callers should treat it
 * as if they had called fput(file).
 */
void fd_install(unsigned int fd, struct file *file)
{
	mutex_lock(&global_fd_lock);

	/* Replace the placeholder with the actual file pointer
	 * idr_replace returns the old pointer on success (which should be NULL here) */
	idr_replace(&global_fd_idr, file, fd);

	mutex_unlock(&global_fd_lock);
}
EXPORT_SYMBOL(fd_install);

struct file *fget(unsigned int fd)
{
	mutex_lock(&global_fd_lock);

	struct file *file = idr_find(&global_fd_idr, fd);
	if (file) {
		get_file(file);
	}

	mutex_unlock(&global_fd_lock);
	return file;
}
EXPORT_SYMBOL(fget);

struct fd fdget(unsigned int fd)
{
	struct file *file = fget(fd);
	if (!file)
		return EMPTY_FD;
	return CLONED_FD(file);
}
EXPORT_SYMBOL(fdget);

/**
 * get_file_active - try go get a reference to a file
 * @f: the file to get a reference on
 *
 * Return: Returns @f with the reference count increased or NULL.
 */
struct file *get_file_active(struct file **f)
{
        return get_file(*f);
}
EXPORT_SYMBOL_GPL(get_file_active);
