#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <init.h>

/* Keep the page in page cache instead of truncating it */
static int shmem_error_remove_folio(struct address_space *mapping,
				    struct folio *folio)
{
	return 0;
}

static int simple_read_folio(struct file *file, struct folio *folio)
{
	folio_zero_range(folio, 0, folio_size(folio));
	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

static const struct address_space_operations shmem_aops = {
	.read_folio	= simple_read_folio,
	.dirty_folio	= noop_dirty_folio,
	.error_remove_folio = shmem_error_remove_folio,
};

static const struct file_operations shmem_file_operations;

bool shmem_mapping(const struct address_space *mapping)
{
	return mapping->a_ops == &shmem_aops;
}
EXPORT_SYMBOL_GPL(shmem_mapping);

/**
 * shmem_file_setup - get an unlinked file living in tmpfs
 * @name: name for dentry (to be seen in /proc/<pid>/maps)
 * @size: size to be set for the file
 * @flags: VMA_NORESERVE_BIT suppresses pre-accounting of the entire object size
 */
struct file *shmem_file_setup(const char *name, loff_t size, vma_flags_t flags)
{
	struct inode *inode = new_inode_pseudo(&pseudo_sb);
	if (!inode) {
		return ERR_PTR(-ENOMEM);
	}
	inode->i_mode = S_IFREG | S_IRWXUGO;
	inode->i_size = size;
	inode->i_mapping->a_ops = &shmem_aops;

	return alloc_file_pseudo(inode, &pseudo_vfsmount, name,
				 FMODE_READ | FMODE_WRITE, &shmem_file_operations);
}
EXPORT_SYMBOL_GPL(shmem_file_setup);

/**
 * shmem_read_folio_gfp - read into page cache, using specified page allocation flags.
 * @mapping:    the folio's address_space
 * @index:      the folio index
 * @gfp:        the page allocator flags to use if allocating
 *
 * This behaves as a tmpfs "read_cache_page_gfp(mapping, index, gfp)",
 * with any new page allocations done using the specified allocation flags.
 * But read_cache_page_gfp() uses the ->read_folio() method: which does not
 * suit tmpfs, since it may have pages in swapcache, and needs to find those
 * for itself; although drivers/gpu/drm i915 and ttm rely upon this support.
 *
 * i915_gem_object_get_pages_gtt() mixes __GFP_NORETRY | __GFP_NOWARN in
 * with the mapping_gfp_mask(), to avoid OOMing the machine unnecessarily.
 */
struct folio *shmem_read_folio_gfp(struct address_space *mapping,
				   pgoff_t index, gfp_t gfp)
{
	return mapping_read_folio_gfp(mapping, index, gfp);
}
EXPORT_SYMBOL_GPL(shmem_read_folio_gfp);

struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
                                         pgoff_t index, gfp_t gfp)
{
        struct folio *folio = shmem_read_folio_gfp(mapping, index, gfp);
        struct page *page;

        if (IS_ERR(folio))
                return &folio->page;

        page = folio_file_page(folio, index);
        if (PageHWPoison(page)) {
                folio_put(folio);
                return ERR_PTR(-EIO);
        }

        return page;
}
EXPORT_SYMBOL_GPL(shmem_read_mapping_page_gfp);

void shmem_truncate_range(struct inode *inode, loff_t lstart, uoff_t lend)
{
	/* TODO */
}
EXPORT_SYMBOL_GPL(shmem_truncate_range);

/**
 * shmem_writeout - Write the folio to swap
 * @folio: The folio to write
 * @plug: swap plug
 * @folio_list: list to put back folios on split
 *
 * Move the folio from the page cache to the swap cache.
 */
int shmem_writeout(struct folio *folio, struct swap_iocb **plug,
		   struct list_head *folio_list)
{
	/* TODO: this is only called when ttm backs up a page when the system is
	 * under memory pressure. The NTOS server has not implemented swapping yet
	 * so for now we return error. */
	return -1;
}
EXPORT_SYMBOL_GPL(shmem_writeout);
