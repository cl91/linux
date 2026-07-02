#include <linux/memblock.h>
#include <host_ops.h>

/**
 * memblock_alloc_try_nid - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. This function zeroes the allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void __init *memblock_alloc_try_nid(phys_addr_t size, phys_addr_t align,
				    phys_addr_t min_addr, phys_addr_t max_addr,
				    int nid)
{
	pr_debug("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		 __func__, (u64)size, (u64)align, nid, &min_addr,
		 &max_addr, (void *)_RET_IP_);

	if (size < PAGE_SIZE) {
		return ntos_allocate_pool(size);
	}

	unsigned long order = 0;
	while ((1UL << (order + PAGE_SHIFT)) < size) {
		order++;
	}
	struct page *page = alloc_pages(GFP_KERNEL, order);
	if (!page) {
		return NULL;
	}
	return page->virtual;
}

/**
 * __memblock_alloc_or_panic - Try to allocate memory and panic on failure
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @func: caller func name
 *
 * This function attempts to allocate memory using memblock_alloc,
 * and in case of failure, it calls panic with the formatted message.
 * This function should not be used directly, please use the macro memblock_alloc_or_panic.
 */
void __init *__memblock_alloc_or_panic(phys_addr_t size, phys_addr_t align,
				       const char *func)
{
	void *addr = memblock_alloc(size, align);

	if (unlikely(!addr))
		panic("%s: Failed to allocate %pap bytes\n", func, &size);
	return addr;
}

/**
 * memblock_free - free boot memory allocation
 * @ptr: starting address of the  boot memory allocation
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_alloc_xx() API.
 * The freeing memory will not be released to the buddy allocator.
 */
void __init_memblock memblock_free(void *ptr, size_t size)
{
	if (!ptr) {
		return;
	}

	if (size < PAGE_SIZE) {
		ntos_free_pool(ptr);
	} else {
		unsigned long order = 0;
		while ((1UL << (order + PAGE_SHIFT)) < size) {
			order++;
		}
		free_pages((unsigned long)ptr, order);
	}
}
