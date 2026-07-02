#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/rbtree_augmented.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <host_ops.h>
#include "internal.h"

unsigned long vmalloc_base;
bool vmalloc_initialized;

struct vmap_space {
	struct rb_root vmap_area_root;
	struct list_head vmap_area_list;
	unsigned long vaddr_start;
	unsigned long size;
};

#define DECLARE_VMAP_SPACE(name, start, sz)				\
	struct vmap_space name = {					\
		.vmap_area_root = RB_ROOT,				\
		.vmap_area_list = LIST_HEAD_INIT(name.vmap_area_list),	\
		.vaddr_start = start,					\
		.size = sz						\
	}

static DECLARE_VMAP_SPACE(vmalloc_vmap_space, 0, VMALLOC_SIZE);
static DECLARE_VMAP_SPACE(module_vmap_space, MODULES_VADDR, MODULES_LEN);

bool is_vmalloc_addr(const void *x)
{
	unsigned long addr = (unsigned long)kasan_reset_tag(x);

	return addr >= VMALLOC_START && addr < VMALLOC_END;
}
EXPORT_SYMBOL(is_vmalloc_addr);

int is_vmalloc_or_module_addr(const void *x)
{
	unsigned long addr = (unsigned long)kasan_reset_tag(x);
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
	return is_vmalloc_addr(x);
}
EXPORT_SYMBOL_GPL(is_vmalloc_or_module_addr);

static void *vmalloc_internal(unsigned long size,
			      struct vmap_space *space)
{
	size = PAGE_ALIGN(size);
	unsigned long gap;
	struct vmap_area *next_va = NULL;
	unsigned long addr = space->vaddr_start;
	if (list_empty(&space->vmap_area_list)) {
		gap = space->size;
	} else {
		struct vmap_area *tmp;
		list_for_each_entry(tmp, &space->vmap_area_list, list) {
			BUG_ON(tmp->va_start < addr);
			gap = tmp->va_start - addr;
			if (gap >= size) {
				next_va = tmp;
				goto alloc;
			}
			addr = tmp->va_end;
		}
		BUG_ON(addr > space->vaddr_start + space->size);
		if (gap < size) {
			gap = space->vaddr_start + space->size - addr;
		}
	}
	if (gap < size) {
		return NULL;
	}

alloc:
	/* Allocate metadata and commit memory */
	struct vmap_area *va = ntos_allocate_pool(sizeof(*va));
	if (!va) {
		return NULL;
	}

	va->va_start = addr;
	va->va_end = addr + size;

	if (ntos_commit_virtual_memory((void *)addr, size, 0, NULL) != 0) {
		ntos_free_pool(va);
		return NULL;
	}

	if (next_va) {
		BUG_ON(addr >= next_va->va_start);
		/* Insert before next_va */
		list_add_tail(&va->list, &next_va->list);
	} else {
		list_add_tail(&va->list, &space->vmap_area_list);
	}

	struct rb_node *parent = NULL, **p = &space->vmap_area_root.rb_node;
	while (*p) {
		parent = *p;
		struct vmap_area *tmp = rb_entry(parent, struct vmap_area, rb_node);
		if (addr < tmp->va_start) {
			p = &(*p)->rb_left;
		} else {
			p = &(*p)->rb_right;
		}
	}
	rb_link_node(&va->rb_node, parent, p);
	rb_insert_color(&va->rb_node, &space->vmap_area_root);
	return (void *)addr;
}

static void vfree_internal(const void *addr,
			   struct vmap_space *space)
{
	struct vmap_area *va;
	struct rb_node *node = space->vmap_area_root.rb_node;
	unsigned long vaddr = (unsigned long)addr;

	if (unlikely(!addr)) {
		return;
	}

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);
		if (vaddr < va->va_start)
			node = node->rb_left;
		else if (vaddr > va->va_start)
			node = node->rb_right;
		else
			goto found;
	}
	BUG_ON(1);
	return;

found:
	BUG_ON(vaddr != va->va_start);
	unsigned long size = va->va_end - vaddr;
	ntos_free_virtual_memory(addr, size, false);

	list_del(&va->list);
	rb_erase(&va->rb_node, &space->vmap_area_root);
	ntos_free_pool(va);
}

static void *vrealloc_internal(const void *p,
			       unsigned long size,
			       struct vmap_space *space)
{
	size = PAGE_ALIGN(size);
	if (!p) {
		return vmalloc_internal(size, space);
	}
	if (size == 0) {
		vfree(p);
		return NULL;
	}
	struct vmap_area *va;
	struct rb_node *node = space->vmap_area_root.rb_node;
	unsigned long vaddr = (unsigned long)p;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);
		if (vaddr < va->va_start)
			node = node->rb_left;
		else if (vaddr > va->va_start)
			node = node->rb_right;
		else
			goto found;
	}
	BUG_ON(1);
	return NULL;

found:
	unsigned long old_size = va->va_end - vaddr;
	BUG_ON(vaddr != va->va_start);
	if (size == old_size) {
		return (void *)p;
	}
	if (size < old_size) {
		va->va_end = vaddr + size;
		ntos_free_virtual_memory((void *)va->va_end, size - old_size, false);
		return (void *)p;
	}
	void *new_addr = vmalloc(size);
	if (!new_addr) {
		return NULL;
	}
	memcpy(new_addr, p, old_size);
	vfree(p);
	return new_addr;
}

void *vmalloc_modules(unsigned long size)
{
	return vmalloc_internal(size, &module_vmap_space);
}

void vfree_modules(const void *addr)
{
	vfree_internal(addr, &module_vmap_space);
}

void *__vmalloc_noprof(unsigned long size, gfp_t gfp_mask)
{
	return __vmalloc_node_noprof(size, 1, gfp_mask, NUMA_NO_NODE,
				     __builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc_noprof);

/**
 * vmalloc - allocate virtually contiguous memory
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_noprof(unsigned long size)
{
	return __vmalloc_node_noprof(size, 1, GFP_KERNEL, NUMA_NO_NODE,
				     __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_noprof);

/**
 * __vmalloc_node - allocate virtually contiguous memory
 * @size:	    allocation size
 * @align:	    desired alignment
 * @gfp_mask:	    flags for the page level allocator
 * @node:	    node to use for allocation or NUMA_NO_NODE
 * @caller:	    caller's return address
 *
 * Allocate enough pages to cover @size from the page level allocator with
 * @gfp_mask flags.  Map them into contiguous kernel virtual space.
 *
 * Semantics of @gfp_mask (including reclaim/retry modifiers such as
 * __GFP_NOFAIL) are the same as in __vmalloc_node_range_noprof().
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *__vmalloc_node_noprof(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, int node, const void *caller)
{
	return __vmalloc_node_range_noprof(size, align, VMALLOC_START, VMALLOC_END,
					   gfp_mask, PAGE_KERNEL, 0, node, caller);
}

/**
 * __vmalloc_node_range - allocate virtually contiguous memory
 * @size:		  allocation size
 * @align:		  desired alignment
 * @start:		  vm area range start
 * @end:		  vm area range end
 * @gfp_mask:		  flags for the page level allocator
 * @prot:		  protection mask for the allocated pages
 * @vm_flags:		  additional vm area flags (e.g. %VM_NO_GUARD)
 * @node:		  node to use for allocation or NUMA_NO_NODE
 * @caller:		  caller's return address
 *
 * Allocate enough pages to cover @size from the page level
 * allocator with @gfp_mask flags and map them into contiguous
 * virtual range with protection @prot.
 *
 * Supported GFP classes: %GFP_KERNEL, %GFP_ATOMIC, %GFP_NOWAIT,
 * %GFP_NOFS and %GFP_NOIO. Zone modifiers are not supported.
 * Please note %GFP_ATOMIC and %GFP_NOWAIT are supported only
 * by __vmalloc().
 *
 * Retry modifiers: only %__GFP_NOFAIL is supported; %__GFP_NORETRY
 * and %__GFP_RETRY_MAYFAIL are not supported.
 *
 * %__GFP_NOWARN can be used to suppress failure messages.
 *
 * Can not be called from interrupt nor NMI contexts.
 * Return: the address of the area or %NULL on failure
 */
void *__vmalloc_node_range_noprof(unsigned long size, unsigned long align,
				  unsigned long start, unsigned long end, gfp_t gfp_mask,
				  pgprot_t prot, unsigned long vm_flags, int _node,
				  const void *caller)
{
	return vmalloc_internal(size, &vmalloc_vmap_space);
}

/**
 * vzalloc - allocate virtually contiguous memory with zero fill
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_noprof(unsigned long size)
{
        return __vmalloc_node_noprof(size, 1, GFP_KERNEL | __GFP_ZERO, NUMA_NO_NODE,
				     __builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc_noprof);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:       allocation size
 * @node:       numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_node_noprof(unsigned long size, int node)
{
        return __vmalloc_node_noprof(size, 1, GFP_KERNEL | __GFP_ZERO, node,
				     __builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc_node_noprof);

/**
 * vfree - Release memory allocated by vmalloc()
 * @addr:  Memory base address
 *
 * Free the virtually continuous memory area starting at @addr, as obtained
 * from one of the vmalloc() family of APIs.  This will usually also free the
 * physical memory underlying the virtual allocation, but that memory is
 * reference counted, so it will not be freed until the last user goes away.
 *
 * If @addr is NULL, no operation is performed.
 *
 * Context:
 * May sleep if called *not* from interrupt context.
 * Must not be called in NMI context (strictly speaking, it could be
 * if we have CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG, but making the calling
 * conventions for vfree() arch-dependent would be a really bad idea).
 */
void vfree(const void *addr)
{
	vfree_internal(addr, &vmalloc_vmap_space);
}
EXPORT_SYMBOL(vfree);

/**
 * vfree_atomic - release memory allocated by vmalloc()
 * @addr:         memory base address
 *
 * This one is just like vfree() but can be called in any atomic context
 * except NMIs.
 */
void vfree_atomic(const void *addr)
{
	vfree(addr);
}

/**
 * vrealloc_node_align - reallocate virtually contiguous memory; contents
 * remain unchanged
 * @p: object to reallocate memory for
 * @size: the size to reallocate
 * @align: requested alignment
 * @flags: the flags for the page level allocator
 * @nid: node number of the target node
 *
 * If @p is %NULL, vrealloc_XXX() behaves exactly like vmalloc_XXX(). If @size
 * is 0 and @p is not a %NULL pointer, the object pointed to is freed.
 *
 * If the caller wants the new memory to be on specific node *only*,
 * __GFP_THISNODE flag should be set, otherwise the function will try to avoid
 * reallocation and possibly disregard the specified @nid.
 *
 * If __GFP_ZERO logic is requested, callers must ensure that, starting with the
 * initial memory allocation, every subsequent call to this API for the same
 * memory allocation is flagged with __GFP_ZERO. Otherwise, it is possible that
 * __GFP_ZERO is not fully honored by this API.
 *
 * Requesting an alignment that is bigger than the alignment of the existing
 * allocation will fail.
 *
 * In any case, the contents of the object pointed to are preserved up to the
 * lesser of the new and old sizes.
 *
 * This function must not be called concurrently with itself or vfree() for the
 * same memory allocation.
 *
 * Return: pointer to the allocated memory; %NULL if @size is zero or in case of
 *         failure
 */
void *vrealloc_node_align_noprof(const void *p, size_t size, unsigned long align,
				 gfp_t flags, int nid)
{
	return vrealloc_internal(p, size, &vmalloc_vmap_space);
}

/**
 * find_vm_area - find a continuous kernel virtual area
 * @addr:	  base address
 *
 * Search for the kernel VM area starting at @addr, and return it.
 * It is up to the caller to do all required locking to keep the returned
 * pointer valid.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *find_vm_area(const void *addr)
{
	return NULL;
}

/**
 * find_vma() - Find the VMA for a given address, or the next VMA.
 * @mm: The mm_struct to check
 * @addr: The address
 *
 * Returns: The VMA associated with addr, or the next VMA.
 * May return %NULL in the case of no VMA at addr or above.
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	unsigned long index = addr;

        mmap_assert_locked(mm);
        return mt_find(&mm->mm_mt, &index, ULONG_MAX);
}
EXPORT_SYMBOL(find_vma);

static inline LNXDRV_MEMORY_CACHING_TYPE prot_to_lnxdrv_cache_type(pgprot_t prot)
{
	if (pgprot_val(pgprot_writecombine(prot)) == pgprot_val(prot)) {
		return LnxDrvMemWriteCombined;
	} else if (pgprot_val(pgprot_writethrough(prot)) == pgprot_val(prot)) {
		return LnxDrvMemWriteThrough;
	} else if (pgprot_val(pgprot_noncached(prot)) == pgprot_val(prot)) {
		return LnxDrvMemNonCached;
	} else {
		return LnxDrvMemCached;
	}
}

/* You must free the pfn db returned by this routine. */
static unsigned long *pages_to_pfn_db(struct page **pages, unsigned int count,
				      pgprot_t prot, unsigned int *p_pfn_count)
{
	if (!pages || count == 0) {
		return NULL;
	}

	/* Allocate a temporary array to hold the compressed PFN database entries.
	 * Worst-case scenario: No pages are contiguous, requiring 'count' entries. */
	unsigned long *pfn_db = kmalloc_array(count, sizeof(unsigned long), GFP_KERNEL);
	if (!pfn_db) {
		return NULL;
	}

	LNXDRV_MEMORY_CACHING_TYPE cache_type = prot_to_lnxdrv_cache_type(prot);
	unsigned int pfn_count = 0;
	unsigned int i = 0;

	while (i < count) {
		unsigned long start_pfn = page_to_pfn(pages[i]);
		unsigned int run_count = 1;

		/* Loop to find contiguous physical pages up to the max limit of
		 * 1 << LNXDRV_PFN_PAGE_COUNT_BITS pages */
		while ((i + run_count) < count &&
		       run_count < (1 << LNXDRV_PFN_PAGE_COUNT_BITS)) {
			unsigned long next_pfn = page_to_pfn(pages[i + run_count]);
			if (next_pfn == (start_pfn + run_count)) {
				run_count++;
			} else {
				/* Physical discontinuity detected */
				break;
			}
		}

		/* We assume that the memories passed in here are backed by regular
		 * 4K pages and never by large pages (4MB on i686, 2MB on amd64, etc).
		 * The NT-side memory allocation routine guarantees that the pages it
		 * returns are all regular 4K pages. Note that ntos_map_io_space may
		 * use large pages to map MMIO regions. However these mappings are
		 * never tracked by a struct page and therefore cannot appear here. */
		pfn_db[pfn_count++] = LNXDRV_FORM_PFN(start_pfn << PAGE_SHIFT,
						      run_count, cache_type, 0);
		i += run_count;
	}
	*p_pfn_count = pfn_count;
	return pfn_db;
}

/**
 * vmap - map an array of pages into virtually contiguous space
 * @pages: array of page pointers
 * @count: number of pages to map
 * @flags: vm_area->flags
 * @prot: page protection for the mapping
 *
 * Maps @count pages from @pages into contiguous kernel virtual space.
 * If @flags contains %VM_MAP_PUT_PAGES the ownership of the pages array itself
 * (which must be kmalloc or vmalloc memory) and one reference per pages in it
 * are transferred from the caller to vmap(), and will be freed / dropped when
 * vfree() is called on the return value.
 *
 * Return: the address of the area or %NULL on failure
 */
void *vmap(struct page **pages, unsigned int count,
	   unsigned long flags, pgprot_t prot)
{
	unsigned int pfn_count;
	unsigned long *pfn_db = pages_to_pfn_db(pages, count, prot, &pfn_count);
	if (!pfn_db) {
		return NULL;
	}
	void *virt_base = NULL;
	int ret = ntos_map_physical_memory(pfn_db, pfn_count, &virt_base);
	kfree(pfn_db);
	if (!ret) {
		/* Handle the VM_MAP_PUT_PAGES flag if specified by the caller */
		if (flags & VM_MAP_PUT_PAGES) {
			/* Transferring ownership means we must drop references now or
			 * log them tracking system for vfree to clean up later. */
			for (int i = 0; i < count; i++) {
				put_page(pages[i]);
			}
			kvfree(pages);
		}
		return virt_base;
	}
	return NULL;
}
EXPORT_SYMBOL(vmap);

/**
 * vmap_pfn - map an array of PFNs into virtually contiguous space
 * @pfns: array of PFNs
 * @count: number of pages to map
 * @prot: page protection for the mapping
 *
 * Maps @count PFNs from @pfns into contiguous kernel virtual space and returns
 * the start address of the mapping.
 */
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	struct page **pages = kmalloc(count * sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		return NULL;
	}
	for (int i = 0; i < count; i++) {
		pages[i] = pfn_to_page(pfns[i]);
	}
	void *ret = vmap(pages, count, 0, prot);
	kfree(pages);
	return ret;
}
EXPORT_SYMBOL_GPL(vmap_pfn);

int ntos_register_framebuffer(void *va,
			      size_t size,
			      int offset,
			      int width,
			      int height,
			      int pitch,
			      char bits_per_pixel,
			      char blue_index,
			      char green_index,
			      char red_index,
			      bool need_flush)
{
	return ntstatus_to_errno(LnxDrvImportTable.RegisterFramebuffer(va, size,
								       offset, width,
								       height, pitch,
								       bits_per_pixel,
								       blue_index,
								       green_index,
								       red_index,
								       need_flush));
}
EXPORT_SYMBOL(ntos_register_framebuffer);

/**
 * vunmap - release virtual mapping obtained by vmap()
 * @addr:   memory base address
 *
 * Free the virtually contiguous memory area starting at @addr.
 * This address must be from a previous vmalloc.
 *
 * Must not be called in interrupt context.
 */
void vunmap(const void *addr)
{
        if (!addr)
                return;
	/* TODO! */
}
EXPORT_SYMBOL(vunmap);

static BLOCKING_NOTIFIER_HEAD(vmap_notify_list);

int register_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_vmap_purge_notifier);

int unregister_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_vmap_purge_notifier);

static vm_fault_t insert_pfn(struct vm_area_struct *vma, unsigned long addr,
			     unsigned long pfn, pgprot_t prot, bool mkwrite)
{
	// TODO
	return VM_FAULT_NOPAGE;
}

/*
 * Walk a vmap address to the struct page it maps. Huge vmap mappings will
 * return the tail page that corresponds to the base page address, which
 * matches small vmap mappings.
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	/* TODO */
	return NULL;
}
EXPORT_SYMBOL(vmalloc_to_page);

/**
 * vmf_insert_pfn_prot - insert single pfn into user vma with specified pgprot
 * @vma: user vma to map to
 * @addr: target user address of this page
 * @pfn: source kernel pfn
 * @pgprot: pgprot flags for the inserted page
 *
 * This is exactly like vmf_insert_pfn(), except that it allows drivers
 * to override pgprot on a per-page basis.
 *
 * This only makes sense for IO mappings, and it makes no sense for
 * COW mappings.  In general, using multiple vmas is preferable;
 * vmf_insert_pfn_prot should only be used if using multiple VMAs is
 * impractical.
 *
 * pgprot typically only differs from @vma->vm_page_prot when drivers set
 * caching- and encryption bits different than those of @vma->vm_page_prot,
 * because the caching- or encryption mode may not be known at mmap() time.
 *
 * This is ok as long as @vma->vm_page_prot is not used by the core vm
 * to set caching and encryption bits for those vmas (except for COW pages).
 * This is ensured by core vm only modifying these page table entries using
 * functions that don't touch caching- or encryption bits, using pte_modify()
 * if needed. (See for example mprotect()).
 *
 * Also when new page-table entries are created, this is only done using the
 * fault() callback, and never using the value of vma->vm_page_prot,
 * except for page-table entries that point to anonymous pages as the result
 * of COW.
 *
 * Context: Process context.  May allocate using %GFP_KERNEL.
 * Return: vm_fault_t value.
 */
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
			       unsigned long pfn, pgprot_t pgprot)
{
	/*
	 * Technically, architectures with pte_special can avoid all these
	 * restrictions (same for remap_pfn_range).  However we would like
	 * consistency in testing and feature parity among all, so we should
	 * try to keep these invariants in place for everybody.
	 */
	BUG_ON(!(vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP)));
	BUG_ON((vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP)) ==
	       (VM_PFNMAP|VM_MIXEDMAP));
	BUG_ON((vma->vm_flags & VM_PFNMAP) && is_cow_mapping(vma->vm_flags));
	BUG_ON((vma->vm_flags & VM_MIXEDMAP) && pfn_valid(pfn));

	if (addr < vma->vm_start || addr >= vma->vm_end)
		return VM_FAULT_SIGBUS;

	if (!pfn_modify_allowed(pfn, pgprot))
		return VM_FAULT_SIGBUS;

	pfnmap_setup_cachemode_pfn(pfn, &pgprot);

	return insert_pfn(vma, addr, pfn, pgprot, false);
}
EXPORT_SYMBOL(vmf_insert_pfn_prot);

/**
 * vmf_insert_pfn - insert single pfn into user vma
 * @vma: user vma to map to
 * @addr: target user address of this page
 * @pfn: source kernel pfn
 *
 * Similar to vm_insert_page, this allows drivers to insert individual pages
 * they've allocated into a user vma. Same comments apply.
 *
 * This function should only be called from a vm_ops->fault handler, and
 * in that case the handler should return the result of this function.
 *
 * vma cannot be a COW mapping.
 *
 * As this is called only for pages that do not currently exist, we
 * do not need to flush old virtual caches or the TLB.
 *
 * Context: Process context.  May allocate using %GFP_KERNEL.
 * Return: vm_fault_t value.
 */
vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
			  unsigned long pfn)
{
        return vmf_insert_pfn_prot(vma, addr, pfn, vma->vm_page_prot);
}
EXPORT_SYMBOL(vmf_insert_pfn);

/**
 * zap_page_range_single - remove user pages in a given range
 * @vma: vm_area_struct holding the applicable pages
 * @address: starting address of pages to zap
 * @size: number of bytes to zap
 * @details: details of shared cache invalidation
 *
 * The range must fit into one VMA.
 */
void zap_page_range_single(struct vm_area_struct *vma, unsigned long address,
		unsigned long size, struct zap_details *details)
{
	/* TODO */
}

/**
 * zap_vma_ptes - remove ptes mapping the vma
 * @vma: vm_area_struct holding ptes to be zapped
 * @address: starting address of pages to zap
 * @size: number of bytes to zap
 *
 * This function only unmaps ptes assigned to VM_PFNMAP vmas.
 *
 * The entire address range must be fully contained within the vma.
 *
 */
void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
		unsigned long size)
{
	if (!range_in_vma(vma, address, address + size) ||
	    		!(vma->vm_flags & VM_PFNMAP))
		return;

	zap_page_range_single(vma, address, size, NULL);
}
EXPORT_SYMBOL_GPL(zap_vma_ptes);

void remap_pfn_range_prepare(struct vm_area_desc *desc, unsigned long pfn)
{
	/* Do nothing */
}

int remap_pfn_range_complete(struct vm_area_struct *vma, unsigned long addr,
			     unsigned long pfn, unsigned long size, pgprot_t prot)
{
	return remap_pfn_range(vma, addr, pfn, size, prot);
}

/**
 * remap_pfn_range - remap kernel memory to userspace
 * @vma: user vma to map to
 * @addr: target page aligned user address to start at
 * @pfn: page frame number of kernel physical memory address
 * @size: size of mapping area
 * @prot: page protection flags for this mapping
 *
 * Note: this is only safe if the mm semaphore is held when called.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
                    unsigned long pfn, unsigned long size, pgprot_t prot)
{
	/* TODO */
        return -1;
}
EXPORT_SYMBOL(remap_pfn_range);

/*
 * Perform a userland memory mapping into the current process address space.
 *
 * Returns either an error, or the address at which the requested mapping has
 * been performed.
 */
unsigned long vm_mmap(struct file *file, unsigned long addr,
		      unsigned long len, unsigned long prot,
		      unsigned long flag, unsigned long offset)
{
	/* TODO */
	return -EINVAL;
}
EXPORT_SYMBOL(vm_mmap);

/* do_munmap() - Unmap the specified range from the mm_struct
 * @mm: The mm_struct
 * @start: The start address to munmap
 * @len: The length to be munmapped.
 * @uf: The userfaultfd list_head
 *
 * Return: 0 on success, error otherwise.
 */
int do_munmap(struct mm_struct *mm, unsigned long start, size_t len,
	      struct list_head *uf)
{
	/* TODO */
	return -EINVAL;
}

/*
 * Scan a region of virtual memory, filling in page tables as necessary
 * and calling a provided function on each leaf page table.
 */
int apply_to_page_range(struct mm_struct *mm, unsigned long addr,
			unsigned long size, pte_fn_t fn, void *data)
{
	/* TODO */
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(apply_to_page_range);

/**
 * invalidate_mapping_pages - Invalidate all clean, unlocked cache of one inode
 * @mapping: the address_space which holds the cache to invalidate
 * @start: the offset 'from' which to invalidate
 * @end: the offset 'to' which to invalidate (inclusive)
 *
 * This function removes pages that are clean, unmapped and unlocked,
 * as well as shadow entries. It will not block on IO activity.
 *
 * If you want to remove all the pages of one inode, regardless of
 * their use and writeback state, use truncate_inode_pages().
 *
 * Return: The number of indices that had their contents invalidated
 */
unsigned long invalidate_mapping_pages(struct address_space *mapping,
				       pgoff_t start, pgoff_t end)
{
	/* TODO */
        return -1;
}
EXPORT_SYMBOL(invalidate_mapping_pages);

void dump_mapping(const struct address_space *mapping)
{
	struct inode *host;
	const struct address_space_operations *a_ops;
	struct hlist_node *dentry_first;
	struct dentry *dentry_ptr;
	struct dentry dentry;
	char fname[64] = {};
	unsigned long ino;

	/*
	 * If mapping is an invalid pointer, we don't want to crash
	 * accessing it, so probe everything depending on it carefully.
	 */
	if (get_kernel_nofault(host, &mapping->host) ||
	    get_kernel_nofault(a_ops, &mapping->a_ops)) {
		pr_warn("invalid mapping:%px\n", mapping);
		return;
	}

	if (!host) {
		pr_warn("aops:%ps\n", a_ops);
		return;
	}

	if (get_kernel_nofault(dentry_first, &host->i_dentry.first) ||
	    get_kernel_nofault(ino, &host->i_ino)) {
		pr_warn("aops:%ps invalid inode:%px\n", a_ops, host);
		return;
	}

	if (!dentry_first) {
		pr_warn("aops:%ps ino:%lx\n", a_ops, ino);
		return;
	}

	dentry_ptr = container_of(dentry_first, struct dentry, d_u.d_alias);
	if (get_kernel_nofault(dentry, dentry_ptr) ||
	    !dentry.d_parent || !dentry.d_name.name) {
		pr_warn("aops:%ps ino:%lx invalid dentry:%px\n",
				a_ops, ino, dentry_ptr);
		return;
	}

	if (strncpy_from_kernel_nofault(fname, dentry.d_name.name, 63) < 0)
		strscpy(fname, "<invalid>");
	/*
	 * Even if strncpy_from_kernel_nofault() succeeded,
	 * the fname could be unreliable
	 */
	pr_warn("aops:%ps ino:%lx dentry name(?):\"%s\"\n",
		a_ops, ino, fname);
}

/**
 * get_user_pages() - pin user pages in memory
 * @start:      starting user address
 * @nr_pages:   number of pages from start to pin
 * @gup_flags:  flags modifying lookup behaviour
 * @pages:      array that receives pointers to the pages pinned.
 *              Should be at least nr_pages long. Or NULL, if caller
 *              only intends to ensure the pages are faulted in.
 *
 * Since pages are never paged out on NTOS, we simply return success at all times.
 */
long get_user_pages(unsigned long start, unsigned long nr_pages,
                    unsigned int gup_flags, struct page **pages)
{
	start = PAGE_ALIGN_DOWN(start);
	for (unsigned long i = 0; i < nr_pages; i++) {
		pages[i] = virt_to_page((void *)(start + PAGE_SIZE * i));
	}
	return nr_pages;
}
EXPORT_SYMBOL(get_user_pages);

/*
 * get_user_pages_fast() - pin user pages in memory
 * @start:      starting user address
 * @nr_pages:   number of pages from start to pin
 * @gup_flags:  flags modifying pin behaviour
 * @pages:      array that receives pointers to the pages pinned.
 *              Should be at least nr_pages long.
 *
 * This is the same as get_user_pages().
 */
int get_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages)
{
	return get_user_pages(start, nr_pages, gup_flags, pages);
}
EXPORT_SYMBOL_GPL(get_user_pages_fast);

/**
 * pin_user_pages_fast() - pin user pages in memory without taking locks
 *
 * @start:      starting user address
 * @nr_pages:   number of pages from start to pin
 * @gup_flags:  flags modifying pin behaviour
 * @pages:      array that receives pointers to the pages pinned.
 *              Should be at least nr_pages long.
 *
 * See get_user_pages_fast(). */
int pin_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages)
{
	return get_user_pages_fast(start, nr_pages, gup_flags, pages);
}
EXPORT_SYMBOL_GPL(pin_user_pages_fast);

/**
 * unpin_user_page() - release a dma-pinned page
 * @page:            pointer to page to be released
 *
 * Pages that were pinned via pin_user_pages*() must be released via either
 * unpin_user_page(), or one of the unpin_user_pages*() routines. This is so
 * that such pages can be separately tracked and uniquely handled. In
 * particular, interactions with RDMA and filesystems need special handling.
 */
void unpin_user_page(struct page *page)
{
	/* On Neptune OS, this is a no-op. */
}
EXPORT_SYMBOL(unpin_user_page);

/**
 * folio_mark_accessed - Mark a folio as having seen activity.
 * @folio: The folio to mark.
 *
 * This function will perform one of the following transitions:
 *
 * * inactive,unreferenced	->	inactive,referenced
 * * inactive,referenced	->	active,unreferenced
 * * active,unreferenced	->	active,referenced
 *
 * When a newly allocated folio is not yet visible, so safe for non-atomic ops,
 * __folio_set_referenced() may be substituted for folio_mark_accessed().
 *
 * Note: although we don't do any page reclamation right now, in the future
 * when we implement page reclamation we will need these flags so we can track
 * whether a page needs to stay in memory.
 */
void folio_mark_accessed(struct folio *folio)
{
	if (folio_test_dropbehind(folio))
		return;

	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);
	} else if (folio_test_unevictable(folio)) {
		/*
		 * Unevictable pages are on the "LRU_UNEVICTABLE" list. But,
		 * this list is never rotated or maintained, so marking an
		 * unevictable page accessed has no effect.
		 */
	} else if (!folio_test_active(folio)) {
		folio_clear_referenced(folio);
	}
	if (folio_test_idle(folio))
		folio_clear_idle(folio);
}
EXPORT_SYMBOL(folio_mark_accessed);

void mark_page_accessed(struct page *page)
{
	folio_mark_accessed(page_folio(page));
}
EXPORT_SYMBOL(mark_page_accessed);

/*
 * For address_spaces which do not use buffers nor write back.
 */
bool noop_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	if (!folio_test_dirty(folio))
		return !folio_test_set_dirty(folio);
	return false;
}
EXPORT_SYMBOL(noop_dirty_folio);

/**
 * folio_redirty_for_writepage - Decline to write a dirty folio.
 * @wbc: The writeback control.
 * @folio: The folio.
 *
 * When a writepage implementation decides that it doesn't want to write
 * @folio for some reason, it should call this function, unlock @folio and
 * return 0.
 *
 * Return: True if we redirtied the folio.  False if someone else dirtied
 * it first.
 */
bool folio_redirty_for_writepage(struct writeback_control *wbc,
		struct folio *folio)
{
	return false;
}
EXPORT_SYMBOL(folio_redirty_for_writepage);

/**
 * folio_mark_dirty - Mark a folio as being modified.
 * @folio: The folio.
 *
 * The folio may not be truncated while this function is running.
 * Holding the folio lock is sufficient to prevent truncation, but some
 * callers cannot acquire a sleeping lock.  These callers instead hold
 * the page table lock for a page table which contains at least one page
 * in this folio.  Truncation will block on the page table lock as it
 * unmaps pages before removing the folio from its mapping.
 *
 * Return: True if the folio was newly dirtied, false if it was already dirty.
 */
bool folio_mark_dirty(struct folio *folio)
{
	struct address_space *mapping = folio_mapping(folio);

	if (likely(mapping)) {
		/*
		 * readahead/folio_deactivate could remain
		 * PG_readahead/PG_reclaim due to race with folio_end_writeback
		 * About readahead, if the folio is written, the flags would be
		 * reset. So no problem.
		 * About folio_deactivate, if the folio is redirtied,
		 * the flag will be reset. So no problem. but if the
		 * folio is used by readahead it will confuse readahead
		 * and make it restart the size rampup process. But it's
		 * a trivial problem.
		 */
		if (folio_test_reclaim(folio))
			folio_clear_reclaim(folio);
		return mapping->a_ops->dirty_folio(mapping, folio);
	}

	return noop_dirty_folio(mapping, folio);
}
EXPORT_SYMBOL(folio_mark_dirty);

/*
 * folio_mark_dirty() is racy if the caller has no reference against
 * folio->mapping->host, and if the folio is unlocked.  This is because another
 * CPU could truncate the folio off the mapping and then free the mapping.
 *
 * Usually, the folio _is_ locked, or the caller is a user-space process which
 * holds a reference on the inode by having an open file.
 *
 * In other cases, the folio should be locked before running folio_mark_dirty().
 */
bool folio_mark_dirty_lock(struct folio *folio)
{
	bool ret;

	folio_lock(folio);
	ret = folio_mark_dirty(folio);
	folio_unlock(folio);
	return ret;
}
EXPORT_SYMBOL(folio_mark_dirty_lock);

bool set_page_dirty(struct page *page)
{
	return folio_mark_dirty(page_folio(page));
}
EXPORT_SYMBOL(set_page_dirty);

int set_page_dirty_lock(struct page *page)
{
	return folio_mark_dirty_lock(page_folio(page));
}
EXPORT_SYMBOL(set_page_dirty_lock);

/*
 * Clear a folio's dirty flag. Returns true if the folio was previously dirty.
 */
bool folio_clear_dirty_for_io(struct folio *folio)
{
	return folio_test_clear_dirty(folio);
}
EXPORT_SYMBOL(folio_clear_dirty_for_io);

/*
 * The folios which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those folios may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __folio_batch_release() will drain those queues here.
 * folio_batch_move_lru() calls folios_put() directly to avoid
 * mutual recursion.
 */
void __folio_batch_release(struct folio_batch *fbatch)
{
	folios_put(fbatch);
}
EXPORT_SYMBOL(__folio_batch_release);

/**
 * folio_wait_stable() - wait for writeback to finish, if necessary.
 * @folio: The folio to wait on.
 *
 * This function determines if the given folio is related to a backing
 * device that requires folio contents to be held stable during writeback.
 * If so, then it will wait for any pending writeback to complete.
 *
 * Context: Sleeps.  Must be called in process context and with
 * no spinlocks held.  Caller should hold a reference on the folio.
 * If the folio is not locked, writeback may start again after writeback
 * has finished.
 */
void folio_wait_stable(struct folio *folio)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(folio_wait_stable);

/**
 * writeback_iter - iterate folio of a mapping for writeback
 * @mapping: address space structure to write
 * @wbc: writeback context
 * @folio: previously iterated folio (%NULL to start)
 * @error: in-out pointer for writeback errors (see below)
 *
 * This function returns the next folio for the writeback operation described by
 * @wbc on @mapping and  should be called in a while loop in the ->writepages
 * implementation.
 *
 * To start the writeback operation, %NULL is passed in the @folio argument, and
 * for every subsequent iteration the folio returned previously should be passed
 * back in.
 *
 * If there was an error in the per-folio writeback inside the writeback_iter()
 * loop, @error should be set to the error value.
 *
 * Once the writeback described in @wbc has finished, this function will return
 * %NULL and if there was an error in any iteration restore it to @error.
 *
 * Note: callers should not manually break out of the loop using break or goto
 * but must keep calling writeback_iter() until it returns %NULL.
 *
 * Return: the folio to write or %NULL if the loop is done.
 */
struct folio *writeback_iter(struct address_space *mapping,
			     struct writeback_control *wbc,
			     struct folio *folio, int *error)
{
	/* TODO! */
	return NULL;
}
EXPORT_SYMBOL_GPL(writeback_iter);

void __init init_vmalloc(void)
{
	unsigned long totalram_pages;
	ntos_get_system_ram_info(&totalram_pages, &_freeram_pages);
	_totalram_pages.counter = totalram_pages;

	int ret = ntos_reserve_virtual_memory(VMALLOC_SIZE, 0, (void *)&vmalloc_base);
	if (ret) {
		panic("Failed to reserve vmalloc region, error %d\n", ret);
	}
	vmalloc_vmap_space.vaddr_start = vmalloc_base;
	vmalloc_initialized = true;
}
