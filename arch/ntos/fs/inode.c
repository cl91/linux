#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/user_namespace.h>
#include <init.h>

/* root_user.__count is 1, for init task cred */
struct user_struct root_user = {
        .__count        = REFCOUNT_INIT(1),
        .uid            = GLOBAL_ROOT_UID,
        .ratelimit      = RATELIMIT_STATE_INIT(root_user.ratelimit, 0, 0),
};

struct user_namespace init_user_ns = {
        .ns = NS_COMMON_INIT(init_user_ns),
        .uid_map = {
                {
                        .extent[0] = {
                                .first = 0,
                                .lower_first = 0,
                                .count = 4294967295U,
                        },
                        .nr_extents = 1,
                },
        },
        .gid_map = {
                {
                        .extent[0] = {
                                .first = 0,
                                .lower_first = 0,
                                .count = 4294967295U,
                        },
                        .nr_extents = 1,
                },
        },
        .projid_map = {
                {
                        .extent[0] = {
                                .first = 0,
                                .lower_first = 0,
                                .count = 4294967295U,
                        },
                        .nr_extents = 1,
                },
        },
        .owner = GLOBAL_ROOT_UID,
        .group = GLOBAL_ROOT_GID,
        .flags = USERNS_INIT_FLAGS,
};
EXPORT_SYMBOL_GPL(init_user_ns);
EXPORT_SYMBOL_GPL(init_uts_ns);

static const struct super_operations pseudo_sb_ops = {
	/* everything NULL */
};

static struct file_system_type pseudo_fs_type = {
    .owner  = NULL,
    .name   = "pseudo_fs",
    .kill_sb = NULL,
};

struct super_block pseudo_sb = {
	.s_magic            = 0x4E545053, /* 'NTPS' arbitrary magic */
	.s_blocksize        = 4096,
	.s_blocksize_bits   = 12,
	.s_maxbytes         = MAX_LFS_FILESIZE,
	.s_flags            = SB_NOUSER,
	.s_user_ns          = &init_user_ns,
	.s_op               = &pseudo_sb_ops,
	.s_type             = &pseudo_fs_type
};

static struct dentry pseudo_root_dentry = {
       .d_sb = &pseudo_sb,
};

struct vfsmount pseudo_vfsmount = {
       .mnt_root = &pseudo_root_dentry,
       .mnt_sb   = &pseudo_sb,
};

static struct kmem_cache *inode_cachep __ro_after_init;

/**
 *	alloc_inode 	- obtain an inode
 *	@sb: superblock
 *
 *	Allocates a new inode for given superblock.
 *	Inode wont be chained in superblock s_inodes list
 *	This means :
 *	- fs can't be unmount
 *	- quotas, fsnotify, writeback can't work
 */
struct inode *alloc_inode(struct super_block *sb)
{
	struct inode *inode = alloc_inode_sb(sb, inode_cachep, GFP_KERNEL);
	if (!inode)
		return NULL;
	inode_init_always(sb, inode);
	return inode;
}

static int no_open(struct inode *inode, struct file *file)
{
	return -ENXIO;
}

/*
 * Empty aops. Can be used for the cases where the user does not
 * define any of the address_space operations.
 */
const struct address_space_operations empty_aops = {
};
EXPORT_SYMBOL(empty_aops);

/**
 * inode_init_always_gfp - perform inode structure initialisation
 * @sb: superblock inode belongs to
 * @inode: inode to initialise
 * @gfp: allocation flags
 *
 * These are initializations that need to be done on every inode
 * allocation as the fields are not initialised by slab allocation.
 * If there are additional allocations required @gfp is used.
 */
int inode_init_always_gfp(struct super_block *sb, struct inode *inode, gfp_t gfp)
{
	static const struct inode_operations empty_iops;
	static const struct file_operations no_open_fops = {.open = no_open};
	struct address_space *const mapping = &inode->i_data;

	inode->i_sb = sb;
	inode->i_blkbits = sb->s_blocksize_bits;
	inode->i_flags = 0;
	inode_state_assign_raw(inode, 0);
	atomic64_set(&inode->i_sequence, 0);
	atomic_set(&inode->i_count, 1);
	inode->i_op = &empty_iops;
	inode->i_fop = &no_open_fops;
	inode->i_ino = 0;
	inode->__i_nlink = 1;
	inode->i_opflags = 0;
	if (sb->s_xattr)
		inode->i_opflags |= IOP_XATTR;
	if (sb->s_type->fs_flags & FS_MGTIME)
		inode->i_opflags |= IOP_MGTIME;
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	atomic_set(&inode->i_writecount, 0);
	inode->i_size = 0;
	inode->i_write_hint = WRITE_LIFE_NOT_SET;
	inode->i_blocks = 0;
	inode->i_bytes = 0;
	inode->i_generation = 0;
	inode->i_pipe = NULL;
	inode->i_cdev = NULL;
	inode->i_link = NULL;
	inode->i_dir_seq = 0;
	inode->i_rdev = 0;
	inode->dirtied_when = 0;

#ifdef CONFIG_CGROUP_WRITEBACK
	inode->i_wb_frn_winner = 0;
	inode->i_wb_frn_avg_time = 0;
	inode->i_wb_frn_history = 0;
#endif

	spin_lock_init(&inode->i_lock);
	lockdep_set_class(&inode->i_lock, &sb->s_type->i_lock_key);

	init_rwsem(&inode->i_rwsem);
	lockdep_set_class(&inode->i_rwsem, &sb->s_type->i_mutex_key);

	atomic_set(&inode->i_dio_count, 0);

	mapping->a_ops = &empty_aops;
	mapping->host = inode;
	mapping->flags = 0;
	mapping->wb_err = 0;
	atomic_set(&mapping->i_mmap_writable, 0);
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	atomic_set(&mapping->nr_thps, 0);
#endif
	mapping->i_private_data = NULL;
	mapping->writeback_index = 0;
	init_rwsem(&mapping->invalidate_lock);
	lockdep_set_class_and_name(&mapping->invalidate_lock,
				   &sb->s_type->invalidate_lock_key,
				   "mapping.invalidate_lock");
	inode->i_private = NULL;
	inode->i_mapping = mapping;
	INIT_HLIST_HEAD(&inode->i_dentry);	/* buggered by rcu freeing */
#ifdef CONFIG_FS_POSIX_ACL
	inode->i_acl = inode->i_default_acl = ACL_NOT_CACHED;
#endif

#ifdef CONFIG_FSNOTIFY
	inode->i_fsnotify_mask = 0;
#endif
	inode->i_flctx = NULL;

	return 0;
}
EXPORT_SYMBOL(inode_init_always_gfp);

void iput(struct inode *inode)
{
	kmem_cache_free(inode_cachep, inode);
}
EXPORT_SYMBOL(iput);

/*
 * get additional reference to inode; caller must already hold one.
 */
void ihold(struct inode *inode)
{
        WARN_ON(atomic_inc_return(&inode->i_count) < 2);
}
EXPORT_SYMBOL(ihold);

unsigned int get_next_ino(void)
{
	static unsigned int last_inode;
	return ++last_inode;
}
EXPORT_SYMBOL(get_next_ino);

static void __address_space_init_once(struct address_space *mapping)
{
	xa_init_flags(&mapping->i_pages, XA_FLAGS_LOCK_IRQ | XA_FLAGS_ACCOUNT);
	init_rwsem(&mapping->i_mmap_rwsem);
	INIT_LIST_HEAD(&mapping->i_private_list);
	spin_lock_init(&mapping->i_private_lock);
	mapping->i_mmap = RB_ROOT_CACHED;
}

void address_space_init_once(struct address_space *mapping)
{
	memset(mapping, 0, sizeof(*mapping));
	__address_space_init_once(mapping);
}
EXPORT_SYMBOL(address_space_init_once);

/*
 * These are initializations that only need to be done
 * once, because the fields are idempotent across use
 * of the inode, so let the slab aware of that.
 */
void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_devices);
	INIT_LIST_HEAD(&inode->i_io_list);
	INIT_LIST_HEAD(&inode->i_wb_list);
	INIT_LIST_HEAD(&inode->i_lru);
	INIT_LIST_HEAD(&inode->i_sb_list);
	__address_space_init_once(&inode->i_data);
	i_size_ordered_init(inode);
}
EXPORT_SYMBOL(inode_init_once);

static void init_once(void *foo)
{
	struct inode *inode = (struct inode *) foo;

	inode_init_once(inode);
}

void __init inode_init(void)
{
	/* inode slab cache */
	inode_cachep = kmem_cache_create("inode_cache",
					 sizeof(struct inode),
					 0,
					 (SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|
					 SLAB_ACCOUNT),
					 init_once);
}

/**
 * current_time - Return FS time (possibly fine-grained)
 * @inode: inode.
 *
 * We will simply return the current real time.
 */
struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 now;
	ktime_get_real_ts64(&now);
	return now;
}
EXPORT_SYMBOL(current_time);

/**
 * inode_set_ctime_current - set the ctime to current_time
 * @inode: inode
 *
 * Set the inode's ctime to the current value for the inode. Returns the
 * current value that was assigned. If this is not a multigrain inode, then we
 * set it to the later of the coarse time and floor value.
 *
 * If it is multigrain, then we first see if the coarse-grained timestamp is
 * distinct from what is already there. If so, then use that. Otherwise, get a
 * fine-grained timestamp.
 *
 * After that, try to swap the new value into i_ctime_nsec. Accept the
 * resulting ctime, regardless of the outcome of the swap. If it has
 * already been replaced, then that timestamp is later than the earlier
 * unacceptable one, and is thus acceptable.
 */
struct timespec64 inode_set_ctime_current(struct inode *inode)
{
	struct timespec64 now = current_time(inode);
	inode->i_ctime_sec = now.tv_sec;
	inode->i_ctime_nsec = now.tv_nsec;
	return now;
}
EXPORT_SYMBOL(inode_set_ctime_current);

void inode_set_bytes(struct inode *inode, loff_t bytes)
{
        /* Caller is here responsible for sufficient locking
         * (ie. inode->i_lock) */
        inode->i_blocks = bytes >> 9;
        inode->i_bytes = bytes & 511;
}
EXPORT_SYMBOL(inode_set_bytes);

struct file *anon_inode_getfile(const char *name,
				const struct file_operations *fops,
                                void *priv, int flags)
{
	struct file *file = alloc_file_pseudo(NULL, &pseudo_vfsmount, name, flags, fops);
	if (IS_ERR(file))
		return file;

	file->private_data = priv;
	return file;
}
EXPORT_SYMBOL(anon_inode_getfile);

/**
 * anon_inode_getfd - creates a new file instance by hooking it up to
 *                    an anonymous inode and a dentry that describe
 *                    the "class" of the file
 *
 * @name:    [in]    name of the "class" of the new file
 * @fops:    [in]    file operations for the new file
 * @priv:    [in]    private data for the new file (will be file's private_data)
 * @flags:   [in]    flags
 *
 * Creates a new file by hooking it on a single inode. This is
 * useful for files that do not need to have a full-fledged inode in
 * order to operate correctly.  All the files created with
 * anon_inode_getfd() will use the same singleton inode, reducing
 * memory use and avoiding code duplication for the file/inode/dentry
 * setup.  Returns a newly created file descriptor or an error code.
 */
int anon_inode_getfd(const char *name, const struct file_operations *fops,
                     void *priv, int flags)
{
	int fd = get_unused_fd_flags(0);
	if (fd < 0) {
		return fd;
	}
        struct file *filp = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(filp)) {
		put_unused_fd(fd);
		return PTR_ERR(filp);
	}
	fd_install(fd, filp);
	return fd;
}
EXPORT_SYMBOL_GPL(anon_inode_getfd);

void kill_anon_super(struct super_block *sb)
{
	/* Do nothing */
}
EXPORT_SYMBOL(kill_anon_super);

struct vfsmount *mntget(struct vfsmount *mnt)
{
	/* Do nothing */
	return mnt;
}
EXPORT_SYMBOL(mntget);

/**
 * path_get - get a reference to a path
 * @path: path to get the reference to
 *
 * Given a path increment the reference count to the dentry and the vfsmount.
 */
void path_get(const struct path *path)
{
	mntget(path->mnt);
	dget(path->dentry);
}
EXPORT_SYMBOL(path_get);

void mntput(struct vfsmount *mnt)
{
	/* Do nothing */
}
EXPORT_SYMBOL(mntput);

/**
 * path_put - put a reference to a path
 * @path: path to put the reference to
 *
 * Given a path decrement the reference count to the dentry and the vfsmount.
 */
void path_put(const struct path *path)
{
	dput(path->dentry);
	mntput(path->mnt);
}
EXPORT_SYMBOL(path_put);

/**
 * d_instantiate - fill in inode information for a dentry
 * @entry: dentry to complete
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry.
 *
 * This turns negative dentries into productive full members
 * of society.
 *
 * NOTE! This assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
void d_instantiate(struct dentry *entry, struct inode *inode)
{
        BUG_ON(!hlist_unhashed(&entry->d_u.d_alias));
        if (inode) {
		hlist_add_head(&entry->d_u.d_alias, &inode->i_dentry);
        }
	entry->d_inode = inode;
}
EXPORT_SYMBOL(d_instantiate);

/*
 * dput - release a dentry
 * @dentry: dentry to release
 *
 * Release a dentry. This will drop the usage count and if needed, free the dentry.
 */
void dput(struct dentry *dentry)
{
        if (!dentry)
                return;
	int ret = lockref_put_return(&dentry->d_lockref);
        /*
         * File creation and close are always done in PAGED_CODE, so we ignore locks here.
         */
	if (!ret) {
		if (dentry->d_inode) {
			hlist_del(&dentry->d_u.d_alias);
		}
		kfree(dentry->__d_name.name);
		kfree(dentry);
	}
}
EXPORT_SYMBOL(dput);

struct vfsmount *vfs_kern_mount(struct file_system_type *type,
                                int flags, const char *name,
                                void *data)
{
	if (!type)
		return ERR_PTR(-EINVAL);

	struct vfsmount *mnt = kmalloc(sizeof(*mnt), GFP_KERNEL);
	if (!mnt)
		return ERR_PTR(-ENOMEM);

	mnt->mnt_root = &pseudo_root_dentry;
	mnt->mnt_sb   = &pseudo_sb;
	mnt->mnt_flags = flags;

	if (unlikely(!pseudo_sb.s_type)) {
		pseudo_sb.s_type = type;
	}

	return mnt;
}
EXPORT_SYMBOL_GPL(vfs_kern_mount);

struct vfsmount *kern_mount(struct file_system_type *type)
{
        return vfs_kern_mount(type, SB_KERNMOUNT, type->name, NULL);
}
EXPORT_SYMBOL_GPL(kern_mount);
