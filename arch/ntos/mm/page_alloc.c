#include <linux/mm.h>
#include "../../../mm/internal.h"
#include <linux/rbtree.h>
#include <host_ops.h>
#include <init.h>
#include "internal.h"

/* We use the arch_1 bit in the page flags to indicate that this page comes from
 * a user buffer mapped into the driver process, rather than as "kernel" memory. */
#define PG_external PG_arch_1
#define PageExternal(page) test_bit(PG_external, &(page)->flags.f)
#define SetPageExternal(page) set_bit(PG_external, &(page)->flags.f)

unsigned long min_low_pfn = 0;
unsigned long max_low_pfn = ~0UL >> PAGE_SHIFT;
unsigned long max_pfn = ~0UL >> PAGE_SHIFT;
unsigned long long max_possible_pfn = ~0UL >> PAGE_SHIFT;

static struct mem_section *__mem_section[NR_SECTION_ROOTS];
struct mem_section **mem_section = __mem_section;
EXPORT_SYMBOL(mem_section);

#if MAX_PAGE_ORDER > PFN_SECTION_SHIFT
#error "Our page allocator only supports allocation of order up to PFN_SECTION_SHIFT"
#endif

#define SECTION_USAGE_TREE_SIZE	(1UL << (MAX_PAGE_ORDER + 1))

#define PHYMEM_REQUEST_ORDER	(4) /* 64KB = 2^4 * 4KB */
#define PHYMEM_TREE_DEPTH	(PFN_SECTION_SHIFT - PHYMEM_REQUEST_ORDER)
#define PHYMEM_BITMAP_TREE_SIZE (1UL << (PHYMEM_TREE_DEPTH + 1))
#if PHYMEM_BITMAP_TREE_SIZE < BITS_PER_LONG
#error "Physical memory request granularity too large"
#endif

/* We redefine the mem_section_usage to track the allocation status
 * of physical pages in a mem_section. There are two things we must
 * track: first, whether the physical pages of a given range of memory
 * are actually present (ie. NTOS has given us this range of physical
 * pages), and second, whether a physical page has been given out by
 * the buddy allocator to a higher-level component. For each purpose,
 * we use a bitmap storing the binary tree of the allocation status
 * of each order. For instance, in the following tree
 *
 * LEVEL 2              [1]
 *                    /     \
 * LEVEL 1          [1]     [0]
 *                 /  \     /  \
 * LEVEL 0       [1]  [0] [0]  [0]
 *
 * we have the zeroth page allocated, and the tree is represented in
 * memory as
 *
 * BIT    | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
 * SET?   | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 1 |
 *
 * Note bit 7 is unused and is always unset.
 *
 * For the physical page presence status, a tree node being set means
 * that all of the physical pages at and below the given level is present,
 * even though its descendant nodes may not be set. When allocating pages
 * of a given order, after the correspoinding node in the tree is marked
 * as set, its parent node will also marked if the sibling node is also
 * set. This is done recursively till we reach the root node. In other
 * words, the leaf nodes of the physical page presence tree represents the
 * memory ranges of the original allocation requests. When freeing pages
 * corresponding to a given node, we will always call the NT memory release
 * routine on the leaf nodes in its subtree, rather than on the node itself
 * (unless, of course, the node is in the lowest order of the presence tree).
 * This is due to the fact that the NT physical memory release routine must
 * be called with the exact physical memory range that the allocation routine
 * has returned with.
 *
 * For the buddy allocation status, a set node means that some pages at or
 * below that level are allocated, and an unset node means that all pages
 * at and below that level are free to be allocated, assuming that the
 * corresponding physical pages are present.
 *
 * The tracking granularity of the physical page presence status is 64KB
 * (meaning that the leaf node represents a physically contiguous, aligned
 * 64KB region). The tracking granularity of the buddy allocation status
 * is page size (4KB).
 *
 * The buddy bitmap is 256 bytes. The physical memory bitmap is 16 bytes.
 *
 * Note: the pages for user buffers are tracked using the user_buffers rb_tree.
 * The corresponding bits in the phymem_bitmap and buddy_bitmap for these pages
 * are zero.
 */
struct mem_section_usage {
	unsigned long buddy_bitmap[SECTION_USAGE_TREE_SIZE / BITS_PER_LONG];
	unsigned long phymem_bitmap[PHYMEM_BITMAP_TREE_SIZE / BITS_PER_LONG];
	struct list_head entry;
	unsigned long paddr;
};
static LIST_HEAD(section_usage_list);

/*
 * Returns the position in the bitmap for the given node at the given level of
 * the binary tree. For instance, for a tree of max order 2, the root node is
 * assigned the position 6 in the tree, which is 2^(2-0) + 2^(2-1) + 0.
 */
static inline unsigned long bitmap_tree_get_bit_pos(unsigned int tree_depth,
						    unsigned long level,
						    unsigned long node)
{
	BUG_ON(level > tree_depth);
	BUG_ON(node >= (1UL << (tree_depth - level)));
	for (unsigned long i = 0; i < level; i++) {
		node += 1UL << (tree_depth - i);
	}
	return node;
}

static inline void bitmap_tree_mark_node(unsigned long *bitmap,
					 unsigned int tree_depth,
					 unsigned long level,
					 unsigned long node)
{
	BUG_ON(!bitmap);
	BUG_ON(level > tree_depth);
	BUG_ON(node >= (1UL << (tree_depth - level)));
	bitmap_set(bitmap, bitmap_tree_get_bit_pos(tree_depth, level, node), 1);
}

/*
 * Mark the nodes from the given tree node (inclusive) leading to the root node as set.
 */
static inline void bitmap_tree_mark_node_and_ancestors(unsigned long *bitmap,
						       unsigned int tree_depth,
						       unsigned long level,
						       unsigned long node)
{
	BUG_ON(!bitmap);
	BUG_ON(level > tree_depth);
	BUG_ON(node >= (1UL << (tree_depth - level)));
	while (level <= tree_depth) {
		bitmap_tree_mark_node(bitmap, tree_depth, level, node);
		level++;
		node >>= 1;
	}
}

/*
 * Mark the descendants of a given node (but not the node itself) as set.
 */
static inline void bitmap_tree_mark_descendants(unsigned long *bitmap,
						unsigned int tree_depth,
						unsigned long level,
						unsigned long node)
{
	BUG_ON(!bitmap);
	BUG_ON(level > tree_depth);
	BUG_ON(node >= (1UL << (tree_depth - level)));
	for (unsigned long i = 0; i < level; i++) {
		unsigned long start_node = node << (level - i);
		bitmap_set(bitmap, bitmap_tree_get_bit_pos(tree_depth, i, start_node),
			   1UL << (level - i));
	}
}

static inline bool bitmap_tree_is_node_marked(unsigned long *bitmap,
					      unsigned int tree_depth,
					      unsigned long level,
					      unsigned long order)
{
	return test_bit(bitmap_tree_get_bit_pos(tree_depth, level, order), bitmap);
}

/*
 * On return, level and node are set to the actual level and node id of the
 * marked node.
 */
static inline bool bitmap_tree_is_node_or_ancestor_marked(unsigned long *bitmap,
							  unsigned int tree_depth,
							  unsigned long level,
							  unsigned long node,
							  unsigned long *found_level,
							  unsigned long *found_node)
{
	if (level >= PHYMEM_TREE_DEPTH) {
		return false;
	}
	if (bitmap_tree_is_node_marked(bitmap, tree_depth, level, node)) {
		*found_level = level;
		*found_node = node;
		return true;
	}
	return bitmap_tree_is_node_or_ancestor_marked(bitmap, tree_depth,
						      level + 1, node >> 1,
						      found_level, found_node);
}

/*
 * In order to convert from vaddr to paddr in approximately O(1) time, we
 * use the following multi-level table to organize the mapping from vaddr
 * to paddr. Since we allocate physical memory at a granularity of at least
 * 64KB, the low 16 bits can be mapped directly. The high bits are converted
 * using a two-level table on 32-bit arch and a three-level table on 64-bit.
 * The L1 (on 32-bit) and L1/L2 (on 64-bit) tables are page-sized.
 *
 * |---|------|--------------|
 * | 6 |  10  | 16 == 4 + 12 |  (32-bit physical address)
 * |---|------|--------------|
 *   L2   L1   (Same pa range)
 *
 * |---|-----|-----|--------------|
 * | 6 |  9  |  9  | 16 == 4 + 12 |  (40-bit physical address)
 * |---|-----|-----|--------------|
 *   L3   L2   L1   (Same pa range)
 */
#define PHYMEM_ALLOCATION_SHIFT (PHYMEM_REQUEST_ORDER + PAGE_SHIFT)
#define PADDR_MAP_OUTER_SHIFT 6
#define PADDR_MAP_OUTER_DIM (1UL << PADDR_MAP_OUTER_SHIFT)
#ifdef CONFIG_64BIT
typedef unsigned long **paddr_map_t[PADDR_MAP_OUTER_DIM];
#define PADDR_MAP_INNER_SHIFT 9
#else
#define PADDR_MAP_INNER_SHIFT 10
typedef unsigned long *paddr_map_t[PADDR_MAP_OUTER_DIM];
#endif
#define PADDR_MAP_INNER_DIM (1UL << PADDR_MAP_INNER_SHIFT)
#define PADDR_MAP_L1_SHIFT  (PHYMEM_ALLOCATION_SHIFT + PADDR_MAP_INNER_SHIFT)
#define PADDR_MAP_SHIFT     ((sizeof(unsigned long) / 4) * PADDR_MAP_INNER_SHIFT + \
			     PHYMEM_ALLOCATION_SHIFT)

#define PADDR_MAP_L0_INDEX(va) ((unsigned long)(va) & ((1UL << PHYMEM_ALLOCATION_SHIFT) - 1))
#define PADDR_MAP_L1_INDEX(va) (((unsigned long)(va) >> PHYMEM_ALLOCATION_SHIFT) & \
				(PADDR_MAP_INNER_DIM - 1))
#ifdef CONFIG_64BIT
#define PADDR_MAP_L2_INDEX(va) (((unsigned long)(va) >> PADDR_MAP_L1_SHIFT) & \
				(PADDR_MAP_INNER_DIM - 1))
#endif
#define PADDR_MAP_OUTER_INDEX(va) (((unsigned long)(va) >> PADDR_MAP_SHIFT) & \
				   (PADDR_MAP_OUTER_DIM - 1))

static paddr_map_t paddr_map;

static_assert(MAX_PHYSMEM_BITS == PADDR_MAP_OUTER_SHIFT + PADDR_MAP_SHIFT);
static_assert(PADDR_MAP_INNER_DIM == (PAGE_SIZE / sizeof(unsigned long)));

gfp_t gfp_allowed_mask __read_mostly = GFP_BOOT_MASK;

/* We don't have pfmemalloc so this routine always returns false. */
bool gfp_pfmemalloc_allowed(gfp_t gfp_mask)
{
	return false;
}

/*
 * The section_mem_map member of the mem_section structure is computed
 * by subtracting the virtual address of the mem_map array for this
 * section from the starting PFN of the section. This is so that later
 * the address of the page struct of a memory page can be simply computed
 * by adding the section_mem_map (sans the lower bits) with the PFN of
 * that page.
 */
static unsigned long sparse_encode_mem_map(struct page *mem_map,
					   unsigned long sec_nr)
{
	unsigned long coded_mem_map =
		(unsigned long)(mem_map - (section_nr_to_pfn(sec_nr)));
	BUILD_BUG_ON(SECTION_MAP_LAST_BIT > PFN_SECTION_SHIFT);
	BUG_ON(coded_mem_map & ~SECTION_MAP_MASK);
	return coded_mem_map;
}

static void init_single_page(struct page *page, unsigned long pfn,
			     unsigned long zone, int nid,
			     void *vaddr, bool external)
{
	mm_zero_struct_page(page);
	set_page_links(page, zone, nid, pfn);
	init_page_count(page);
	atomic_set(&page->_mapcount, -1);
	page_cpupid_reset_last(page);
	page_kasan_tag_reset(page);
	INIT_LIST_HEAD(&page->lru);
	set_page_address(page, vaddr);
	if (external) {
		SetPageExternal(page);
	}
}

/* Populate the struct page array. */
static void init_pages(unsigned long phy_addr,
		       void *virt_addr,
		       unsigned long num_pages,
		       bool external)
{
	for (unsigned long i = 0; i < num_pages; i++) {
		init_single_page(phys_to_page(phy_addr + i * PAGE_SIZE),
				 (phy_addr >> PAGE_SHIFT) + i,
				 ZONE_NORMAL, 0,
				 (char *)virt_addr + PAGE_SIZE * i,
				 external);
	}
}

/* Allocate the session usage and its mem_map (struct page array) and initialze the section.
 * The page structs of the given address range will be populated, with the rest populated
 * with poison. */
static int init_section(struct mem_section *section,
			unsigned long phy_addr,
			void *virt_addr,
			unsigned long num_pages,
			bool external)
{
	BUG_ON(section->usage);
	unsigned long section_nr = phy_addr >> PA_SECTION_SHIFT;
	unsigned long section_align = 1UL << SECTION_SIZE_BITS;
	unsigned long section_paddr = ALIGN_DOWN(phy_addr, section_align);

	/* Allocate memory for the boot section usage. Note we request pool memory here
	 * as mem_section_usage is a small object (less than one page size). */
	struct mem_section_usage *usage =
		ntos_allocate_pool(sizeof(struct mem_section_usage));
	if (!usage) {
		return -ENOMEM;
	}

	/* Allocate the mem_map (array of struct page) */
	int mem_map_size = PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION);
	struct page *mem_map;
	ntos_allocate_virtual_memory(mem_map_size, 0, (void **)&mem_map);
	if (!mem_map) {
		ntos_free_pool(usage);
		return -ENOMEM;
	}
	page_init_poison(mem_map, mem_map_size);

	BUG_ON((unsigned long)mem_map & ~SECTION_MAP_MASK);
	section->section_mem_map |= sparse_encode_mem_map(mem_map, section_nr)
		| SECTION_HAS_MEM_MAP | SECTION_MARKED_PRESENT | SECTION_IS_ONLINE;
	section->usage = usage;

	BUG_ON(phys_to_page(section_paddr) != mem_map);
	init_pages(phy_addr, virt_addr, num_pages, external);

	list_add_tail(&usage->entry, &section_usage_list);
	usage->paddr = section_paddr;

	return 0;
}

/* Initialize the virtual-to-physical address map for the given virtual
 * address. The address must NOT already have been mapped. */
static int init_paddr_map(void *virt_addr,
			  unsigned long phy_addr,
			  int alloc_order)
{
	BUG_ON(alloc_order > MAX_PAGE_ORDER);
	BUG_ON(alloc_order < PHYMEM_REQUEST_ORDER);
	BUG_ON(!IS_ALIGNED((unsigned long)virt_addr, 1UL << (alloc_order + PAGE_SHIFT)));
	BUG_ON(!IS_ALIGNED(phy_addr, 1UL << (alloc_order + PAGE_SHIFT)));

	unsigned long idx_l0 = PADDR_MAP_L0_INDEX(virt_addr);
	BUG_ON(idx_l0);
	unsigned long idx_l1 = PADDR_MAP_L1_INDEX(virt_addr);
#ifdef CONFIG_64BIT
	unsigned long idx_l2 = PADDR_MAP_L2_INDEX(virt_addr);
#endif
	unsigned long idx_outer = PADDR_MAP_OUTER_INDEX(virt_addr);

	if (!paddr_map[idx_outer]) {
		void *table = NULL;
		ntos_allocate_virtual_memory(PAGE_SIZE, 0, &table);
		if (!table) {
			return -ENOMEM;
		}
		paddr_map[idx_outer] = table;
	}
#ifdef CONFIG_64BIT
	if (!paddr_map[idx_outer][idx_l2]) {
		void *table = NULL;
		ntos_allocate_virtual_memory(PAGE_SIZE, 0, &table);
		if (!table) {
			return -ENOMEM;
		}
		paddr_map[idx_outer][idx_l2] = table;
	}
	unsigned long *table = paddr_map[idx_outer][idx_l2];
#else
	unsigned long *table = paddr_map[idx_outer];
#endif

	BUILD_BUG_ON(PADDR_MAP_INNER_SHIFT < MAX_PAGE_ORDER - PHYMEM_REQUEST_ORDER);
	alloc_order -= PHYMEM_REQUEST_ORDER;
	for (unsigned long i = 0; i < (1UL << alloc_order); i++) {
		BUG_ON(table[idx_l1 + i]);
		table[idx_l1 + i] = phy_addr + (i << PHYMEM_ALLOCATION_SHIFT);
	}
	return 0;
}

static void mark_pages_present(struct mem_section_usage *usage,
			       unsigned int alloc_order,
			       unsigned long phy_addr)
{
	BUG_ON(!usage);
	unsigned long section_align = 1UL << SECTION_SIZE_BITS;
	unsigned long section_paddr = ALIGN_DOWN(phy_addr, section_align);
	unsigned int page_start = (phy_addr - section_paddr) >> PAGE_SHIFT;

	/* Mark the physical page range as present */
	bitmap_tree_mark_node(usage->phymem_bitmap,
			      PHYMEM_TREE_DEPTH,
			      alloc_order - PHYMEM_REQUEST_ORDER,
			      page_start >> alloc_order);
}

extern void __init kmem_cache_init(void);

void __init init_mem(void)
{
	/* Allocate an initial memory section with 64KB of physical memory. */
	void *virt_addr = NULL;
	unsigned long phy_addr = 0;
	unsigned int boot_alloc_order = PHYMEM_REQUEST_ORDER;
	ntos_allocate_physical_memory(boot_alloc_order, 0, &virt_addr, &phy_addr);
	if (!virt_addr || !phy_addr) {
		panic("failed to allocate boot memory (got va %p pa %p)",
		      virt_addr, (void *)phy_addr);
	}

	/* Allocate one page for the second-level mem_section pointers. */
	struct mem_section *section_root = NULL;
	ntos_allocate_virtual_memory(PAGE_SIZE, 0, (void **)&section_root);
	if (!section_root) {
		panic("failed to allocate boot memory\n");
	}

	/* Set up the mem_section pointers. The first-level of pointers (two on 32-bit,
	 * 1024 on 64-bit) is simply allocated as a global variable (__mem_section).
	 * The second level is allocated dynamically above. */
	unsigned long section_nr = phy_addr >> PA_SECTION_SHIFT;
	unsigned long section_idx = section_nr & SECTION_ROOT_MASK;
	unsigned long root_nr = SECTION_NR_TO_ROOT(section_nr);
	mem_section[root_nr] = section_root;
	struct mem_section *boot_section = &section_root[section_idx];
	BUG_ON(boot_section != __nr_to_section(section_nr));

	/* Allocate the section usage if needed and initialize the section and its mem_map. */
	int ret = init_section(boot_section, phy_addr, virt_addr,
			       1UL << boot_alloc_order, false);
	if (ret) {
		panic("failed to allocate boot pool memory\n");
	}

	/* Allocate the tables for paddr_map and populate the entry for the vaddr. */
	ret = init_paddr_map(virt_addr, phy_addr, boot_alloc_order);
	if (ret) {
		panic("failed to allocate boot virt_phy_map\n");
	}

	mark_pages_present(boot_section->usage, boot_alloc_order, phy_addr);

	/* Initialize the percpu allocator which is need for the slub allocator init. */
	setup_per_cpu_areas();

	/* Initialize the slub allocator. */
	kmem_cache_init();

	/* Initialize vmalloc */
	init_vmalloc();
}

/*
 * Higher-order pages are called "compound pages".  They are structured thusly:
 *
 * The first PAGE_SIZE page is called the "head page" and have PG_head set.
 *
 * The remaining PAGE_SIZE pages are called "tail pages". PageTail() is encoded
 * in bit 0 of page->compound_head. The rest of bits is pointer to head page.
 *
 * The first tail page's ->compound_order holds the order of allocation.
 * This usage means that zero-order pages may not be compound.
 */
void prep_compound_page(struct page *page, unsigned int order)
{
        int i;
        int nr_pages = 1 << order;

        __SetPageHead(page);
        for (i = 1; i < nr_pages; i++)
                prep_compound_tail(page, i);

        prep_compound_head(page, order);
}

static struct page *allocate_pages(unsigned int order)
{
	/* We need to disable interrupts as this routine may be called in
	 * interrupt or DPC context. */
	unsigned long irq_flags;
	local_irq_save(irq_flags);
	/* Loop all the available mem_section's and check for free space. */
	struct mem_section_usage *u;
	list_for_each_entry(u, &section_usage_list, entry) {
		const unsigned long pa_order = order > PHYMEM_REQUEST_ORDER ?
			order - PHYMEM_REQUEST_ORDER : 0;
		const unsigned long pa_node_count = 1UL << (PHYMEM_TREE_DEPTH - pa_order);
		/* Look at the the phymem tree and find a node that is mapped. */
		for (unsigned long pa_node = 0; pa_node < pa_node_count; pa_node++) {
			unsigned long found_level, found_node;
			if (!bitmap_tree_is_node_or_ancestor_marked(u->phymem_bitmap,
								    PHYMEM_TREE_DEPTH,
								    pa_order, pa_node,
								    &found_level,
								    &found_node)) {
				continue;
			}
			/* We found a physically contiguous range that is mapped, so check
			 * if it has free pages. */
			const int node_shift = PHYMEM_REQUEST_ORDER + found_level - order;
			BUG_ON(node_shift < 0);
			const unsigned long start_node = found_node << node_shift;
			const unsigned long node_count = 1UL << node_shift;
			for (unsigned long i = start_node; i < start_node + node_count; i++) {
				if (!bitmap_tree_is_node_marked(u->buddy_bitmap,
								MAX_PAGE_ORDER,
								order, i)) {
					bitmap_tree_mark_node_and_ancestors(u->buddy_bitmap,
									    MAX_PAGE_ORDER,
									    order, i);
					bitmap_tree_mark_descendants(u->buddy_bitmap,
								     MAX_PAGE_ORDER,
								     order, i);
					unsigned long offset = i << (order + PAGE_SHIFT);
					BUG_ON(!IS_ALIGNED(u->paddr, 1UL << SECTION_SIZE_BITS));
					struct page *page = phys_to_page(u->paddr + offset);
					if (order > 0) {
						prep_compound_page(page, order);
					}
					local_irq_restore(irq_flags);
					return page;
				}
			}
		}
	}
	local_irq_restore(irq_flags);
	return NULL;
}

/*
 * Allocate and initialize the mem_section for the specified physical address range,
 * and populate the struct page array. The page structs must not be initialized before.
 */
static struct mem_section *init_section_and_pages(unsigned long phy_addr,
						  void *virt_addr,
						  unsigned long num_pages,
						  bool external)
{
	/* Allocate the second-level mem_section pointer, if it does not exist. */
	unsigned long section_nr = phy_addr >> PA_SECTION_SHIFT;
	unsigned long root_nr = SECTION_NR_TO_ROOT(section_nr);
	if (!mem_section[root_nr]) {
		void *section_root = NULL;
		ntos_allocate_virtual_memory(PAGE_SIZE, 0, &section_root);
		if (!section_root) {
			return ERR_PTR(-ENOMEM);
		}
		mem_section[root_nr] = section_root;
	}
	struct mem_section *section = __nr_to_section(section_nr);
	BUG_ON(!section);

	/* If section is uninitialized, initialize the section. */
	if (!section->usage) {
		int ret = init_section(section, phy_addr, virt_addr, num_pages, external);
		if (ret) {
			return ERR_PTR(ret);
		}
	} else {
		init_pages(phy_addr, virt_addr, num_pages, external);
	}
	return section;
}

static int request_phy_mem(unsigned int order)
{
	if (order < PHYMEM_REQUEST_ORDER) {
		order = PHYMEM_REQUEST_ORDER;
	}

	void *virt_addr = NULL;
	unsigned long phy_addr = 0;
	ntos_allocate_physical_memory(order, 0, &virt_addr, &phy_addr);
	if (!virt_addr || !phy_addr) {
		return -ENOMEM;
	}

	struct mem_section *section = init_section_and_pages(phy_addr, virt_addr,
							     1UL << order, false);
	if (IS_ERR(section)) {
		ntos_free_physical_memory(order, 0, virt_addr);
		return PTR_ERR(section);
	}

	if (init_paddr_map(virt_addr, phy_addr, order)) {
		/* Uninitialize the page structs and free the requested memory. */
		page_init_poison(phys_to_page(phy_addr), 1UL << order);
		ntos_free_physical_memory(order, 0, virt_addr);
		return -ENOMEM;
	}

	mark_pages_present(section->usage, order, phy_addr);
	return 0;
}

struct page *__alloc_frozen_pages_noprof(gfp_t gfp,
					 unsigned int order,
					 int preferred_nid,
					 nodemask_t *nodemask)
{
	struct page *pages = allocate_pages(order);
	if (pages) {
		return pages;
	}
	if (request_phy_mem(order)) {
		return NULL;
	}
	pages = allocate_pages(order);
	BUG_ON(!pages);
	return pages;
}
EXPORT_SYMBOL(__alloc_frozen_pages_noprof);

struct page *alloc_frozen_pages_nolock_noprof(gfp_t gfp_flags,
					      int nid,
					      unsigned int order)
{
	/* Since we run in userspace, it is always safe to ask for more memory in
	 * an interrupt handler so we will just call the locked version of the
	 * page allocation routine. We also don't need to worry about acquiring
	 * or releasing spinlocks as we are effectively an uniprocessor machine
	 * and can simply disable interrupts to synchronize (and you can call
	 * local_irq_save as many times as you want). */
	return __alloc_frozen_pages_noprof(gfp_flags, order, nid, NULL);
}

struct page *__alloc_pages_noprof(gfp_t gfp, unsigned int order,
				  int preferred_nid, nodemask_t *nodemask)
{
	struct page *page;

	page = __alloc_frozen_pages_noprof(gfp, order, preferred_nid, nodemask);
	if (page)
		set_page_refcounted(page);
	return page;
}
EXPORT_SYMBOL(__alloc_pages_noprof);

struct folio *__folio_alloc_noprof(gfp_t gfp, unsigned int order, int preferred_nid,
                nodemask_t *nodemask)
{
        struct page *page = __alloc_pages_noprof(gfp | __GFP_COMP, order,
                                        preferred_nid, nodemask);
        return page_rmappable_folio(page);
}
EXPORT_SYMBOL(__folio_alloc_noprof);

unsigned long get_free_pages_noprof(gfp_t gfp_mask, unsigned int order)
{
	struct page *page;

	page = alloc_pages_noprof(gfp_mask & ~__GFP_HIGHMEM, order);
	if (!page)
		return 0;
	return (unsigned long) page_address(page);
}
EXPORT_SYMBOL(get_free_pages_noprof);

void free_frozen_pages(struct page *page, unsigned int order)
{
	/* TODO */
}

/**
 * __free_pages - Free pages allocated with alloc_pages().
 * @page: The page pointer returned from alloc_pages().
 * @order: The order of the allocation.
 *
 * This function can free multi-page allocations that are not compound
 * pages.  It does not check that the @order passed in matches that of
 * the allocation, so it is easy to leak memory.  Freeing more memory
 * than was allocated will probably emit a warning.
 *
 * If the last reference to this page is speculative, it will be released
 * by put_page() which only frees the first page of a non-compound
 * allocation.  To prevent the remaining pages from being leaked, we free
 * the subsequent pages here.  If you want to use the page's reference
 * count to decide when to free the allocation, you should allocate a
 * compound page, and use put_page() instead of __free_pages().
 *
 * Context: May be called in any context.
 */
void __free_pages(struct page *page, unsigned int order)
{
	BUG_ON(!page);
	/* get PageHead before we drop reference */
	int head = PageHead(page);

	if (put_page_testzero(page)) {
		free_frozen_pages(page, order);
	} else if (!head) {
		while (order-- > 0) {
			/*
			 * The "tail" pages of this non-compound high-order
			 * page will have no code tags, so to avoid warnings
			 * mark them as empty.
			 */
			clear_page_tag_ref(page + (1 << order));
			free_frozen_pages(page + (1 << order), order);
		}
	}
}
EXPORT_SYMBOL(__free_pages);

/*
 * Can be called while holding raw_spin_lock or from IRQ and NMI for any
 * page type (not only those that came from alloc_pages_nolock)
 */
void free_pages_nolock(struct page *page, unsigned int order)
{
	__free_pages(page, order);
}

/**
 * free_pages - Free pages allocated with __get_free_pages().
 * @addr: The virtual address tied to a page returned from __get_free_pages().
 * @order: The order of the allocation.
 *
 * This function behaves the same as __free_pages(). Use this function
 * to free pages when you only have a valid virtual address. If you have
 * the page, call __free_pages() instead.
 */
void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_pages(virt_to_page((void *)addr), order);
	}
}
EXPORT_SYMBOL(free_pages);

static void *make_alloc_exact(unsigned long addr, unsigned int order,
                size_t size)
{
        if (addr) {
                unsigned long nr = DIV_ROUND_UP(size, PAGE_SIZE);
                struct page *page = virt_to_page((void *)addr);
                struct page *last = page + nr;

                split_page(page, order);
                while (page < --last)
                        set_page_refcounted(last);

                last = page + (1UL << order);
                for (page += nr; page < last; page++)
                        __free_pages(page, 0);
        }
        return (void *)addr;
}

/**
 * alloc_pages_exact - allocate an exact number physically-contiguous pages.
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation, must not contain __GFP_COMP
 *
 * This function is similar to alloc_pages(), except that it allocates the
 * minimum number of pages to satisfy the request.  alloc_pages() can only
 * allocate memory in power-of-two pages.
 *
 * This function is also limited by MAX_PAGE_ORDER.
 *
 * Memory allocated by this function must be released by free_pages_exact().
 *
 * Return: pointer to the allocated area or %NULL in case of error.
 */
void *alloc_pages_exact_noprof(size_t size, gfp_t gfp_mask)
{
	unsigned int order = get_order(size);

        if (WARN_ON_ONCE(gfp_mask & (__GFP_COMP | __GFP_HIGHMEM)))
                gfp_mask &= ~(__GFP_COMP | __GFP_HIGHMEM);

        unsigned long addr = get_free_pages_noprof(gfp_mask, order);
	return make_alloc_exact(addr, order, size);
}
EXPORT_SYMBOL(alloc_pages_exact_noprof);

/**
 * free_pages_exact - release memory allocated via alloc_pages_exact()
 * @virt: the value returned by alloc_pages_exact.
 * @size: size of allocation, same value as passed to alloc_pages_exact().
 *
 * Release the memory allocated by a previous call to alloc_pages_exact.
 */
void free_pages_exact(void *virt, size_t size)
{
        unsigned long addr = (unsigned long)virt;
        unsigned long end = addr + PAGE_ALIGN(size);

        while (addr < end) {
                free_page(addr);
                addr += PAGE_SIZE;
        }
}
EXPORT_SYMBOL(free_pages_exact);

void __folio_put(struct folio *folio)
{
	if (!PageExternal(&folio->page)) {
		free_frozen_pages(&folio->page, folio_order(folio));
	}
}
EXPORT_SYMBOL(__folio_put);

/*
 * Free a batch of folios
 */
void free_unref_folios(struct folio_batch *folios)
{
	for (int i = 0; i < folios->nr; i++) {
		folio_put(folios->folios[i]);
		folios->folios[i] = NULL;
	}
	folio_batch_reinit(folios);
}

/**
 * folios_put_refs - Reduce the reference count on a batch of folios.
 * @folios: The folios.
 * @refs: The number of refs to subtract from each folio.
 *
 * Like folio_put(), but for a batch of folios.  This is more efficient
 * than writing the loop yourself as it will optimise the locks which need
 * to be taken if the folios are freed.  The folios batch is returned
 * empty and ready to be reused for another batch; there is no need
 * to reinitialise it.  If @refs is NULL, we subtract one from each
 * folio refcount.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
void folios_put_refs(struct folio_batch *folios, unsigned int *refs)
{
	int i, j;

	for (i = 0, j = 0; i < folios->nr; i++) {
		struct folio *folio = folios->folios[i];
		unsigned int nr_refs = refs ? refs[i] : 1;

		if (is_huge_zero_folio(folio))
			continue;

		if (!folio_ref_sub_and_test(folio, nr_refs))
			continue;

		if (j != i)
			folios->folios[j] = folio;
		j++;
	}
	if (!j) {
		folio_batch_reinit(folios);
		return;
	}

	folios->nr = j;
	free_unref_folios(folios);
}
EXPORT_SYMBOL(folios_put_refs);

/**
 * release_pages - batched put_page()
 * @arg: array of pages to release
 * @nr: number of pages
 *
 * Decrement the reference count on all the pages in @arg.  If it
 * fell to zero, remove the page from the LRU and free it.
 *
 * Note that the argument can be an array of pages, encoded pages,
 * or folio pointers. We ignore any encoded bits, and turn any of
 * them into just a folio that gets free'd.
 */
void release_pages(release_pages_arg arg, int nr)
{
        struct folio_batch fbatch;
        int refs[PAGEVEC_SIZE];
        struct encoded_page **encoded = arg.encoded_pages;
        int i;

        folio_batch_init(&fbatch);
        for (i = 0; i < nr; i++) {
                /* Turn any of the argument types into a folio */
                struct folio *folio = page_folio(encoded_page_ptr(encoded[i]));

                /* Is our next entry actually "nr_pages" -> "nr_refs" ? */
                refs[fbatch.nr] = 1;
                if (unlikely(encoded_page_flags(encoded[i]) &
                             ENCODED_PAGE_BIT_NR_PAGES_NEXT))
                        refs[fbatch.nr] = encoded_nr_pages(encoded[++i]);

                if (folio_batch_add(&fbatch, folio) > 0)
                        continue;
                folios_put_refs(&fbatch, refs);
        }

        if (fbatch.nr)
                folios_put_refs(&fbatch, refs);
}
EXPORT_SYMBOL(release_pages);

bool is_free_buddy_page(const struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int order;

	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		const struct page *head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(head) &&
		    buddy_order_unsafe(head) >= order)
			break;
	}

	return order <= MAX_PAGE_ORDER;
}
EXPORT_SYMBOL(is_free_buddy_page);

/*
 * split_page takes a non-compound higher-order page, and splits it into
 * n (1<<order) sub-pages: page[0..n]
 * Each sub-page must be freed individually. This is a no-op for our page
 * allocator since non-compound pages can always be freed individually.
 */
void split_page(struct page *page, unsigned int order)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(split_page);

/**
 * check_move_unevictable_folios - Move evictable folios to appropriate zone
 * lru list
 * @fbatch: Batch of lru folios to check.
 *
 * Checks folios for evictability, if an evictable folio is in the unevictable
 * lru list, moves it to the appropriate evictable lru list. This function
 * should be only used for lru folios.
 */
void check_move_unevictable_folios(struct folio_batch *fbatch)
{
	/* TODO */
}
EXPORT_SYMBOL_GPL(check_move_unevictable_folios);

/* Structure representing a tracked user buffer */
struct user_buffer_node {
	struct rb_node node;
	void *va;
	unsigned long size;
	unsigned long pfndb_size;
	unsigned long pfndb[];
};

/* Global tree root and spinlock for synchronization */
static struct rb_root user_buffers = RB_ROOT;

/**
 * lnxdrv_register_user_buffer - Record a user buffer range into the rb_tree.
 * @va: Starting virtual address.
 * @size: Size of the buffer in bytes.
 * @pfndb_size: Size of the PFN database.
 * @pfndb: Pointer to the PFN database.
 *
 * Returns 0 on success, or -ENOMEM on allocation failure.
 * Calls BUG_ON() if an overlapping range is detected.
 */
int lnxdrv_register_user_buffer(void *va,
                                unsigned long size,
                                unsigned long pfndb_size,
                                unsigned long *pfndb)
{
	unsigned long start = (unsigned long)va;
	unsigned long end = start + size;
	unsigned long align = LNXDRV_PFN_PAGE_SIZE(pfndb[0]);
	start = ALIGN_DOWN(start, align);
	end = ALIGN(end, align);
	va = (void *)start;
	size = end - start;

	/* Pre-allocate node before taking the lock to avoid blocking */
	struct user_buffer_node *new_buf = kmalloc(sizeof(*new_buf) +
						   sizeof(unsigned long) * pfndb_size,
						   GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;

	new_buf->va = va;
	new_buf->size = size;
	new_buf->pfndb_size = pfndb_size;
	memcpy(new_buf->pfndb, pfndb, pfndb_size * sizeof(unsigned long));

	unsigned long irq_flags;
	local_irq_save(irq_flags);

	/* Navigate the tree to find the insertion point */
	struct rb_node **link = &user_buffers.rb_node;
	struct rb_node *parent = NULL;
	while (*link) {
		struct user_buffer_node *this_buf = rb_entry(*link,
							     struct user_buffer_node,
							     node);
		parent = *link;

		unsigned long this_va = (unsigned long)this_buf->va;
		unsigned long this_end = this_va + this_buf->size;
		/* Check if there is any overlap with an already registered buffer.
		 * We allow re-registering the original buffer or a sub-buffer, but
		 * disallow registering a new buffer with an overlap. */
		if ((start >= this_va && start < this_end) ||
		    (end > this_va && end <= this_end)) {
			if (start >= this_va && end <= this_end) {
				local_irq_restore(irq_flags);
				kfree(new_buf);
				return 0;
			} else {
				BUG_ON(1);
			}
		}

		if (start < this_va)
			link = &((*link)->rb_left);
		else
			link = &((*link)->rb_right);
	}

	rb_link_node(&new_buf->node, parent, link);
	rb_insert_color(&new_buf->node, &user_buffers);

	/* For each PFN entry, we populate the corresponding struct page array. */
	unsigned long offset = 0;
	for (int i = 0; i < pfndb_size; i++) {
		unsigned long pfn = pfndb[i];
		unsigned long pa_start = LNXDRV_PFN_PAGE_ADDRESS(pfn);
		unsigned long page_size = LNXDRV_PFN_PAGE_SIZE(pfn);
		unsigned long page_count = LNXDRV_PFN_PAGE_COUNT(pfn);
		unsigned long pa_end = pa_start + page_size * page_count;
		for (unsigned long pa = pa_start; pa < pa_end; ) {
			unsigned long sec_end = min(pa_end,
						    ALIGN(pa, 1UL << PA_SECTION_SHIFT));
			unsigned long num_pages = (sec_end - pa) / PAGE_SIZE;
			struct mem_section *section = init_section_and_pages(pa,
									     (char *)va + offset,
									     num_pages, true);
			if (IS_ERR(section)) {
				for (unsigned long addr = (unsigned long)va;
				     addr < (unsigned long)va + offset;
				     addr += PAGE_SIZE) {
					struct page *page = virt_to_page((void *)addr);
					put_page(page);
					page_init_poison(page, PAGE_SIZE);
				}
				rb_erase(&new_buf->node, &user_buffers);
				local_irq_restore(irq_flags);
				return PTR_ERR(section);
			}
			offset += sec_end - pa;
			pa = sec_end;
		}
	}

	local_irq_restore(irq_flags);
	return 0;
}

/**
 * lnxdrv_unregister_user_buffer - Unregisters a user buffer range from the rb_tree.
 * @va: Starting virtual address.
 * @size: Size of the buffer in bytes.
 *
 * Returns 0 on success.
 * Calls BUG_ON() if not all pages of the user buffer has been dropped.
 * Calls BUG_ON() if the exact [va, va+size) range is not found in the tree.
 */
int lnxdrv_unregister_user_buffer(void *va, unsigned long size)
{
	struct rb_node *node = user_buffers.rb_node;
	struct user_buffer_node *found_buf = NULL;
	unsigned long start = (unsigned long)va;
	unsigned long end = start + size;
	start = PAGE_ALIGN_DOWN(start);
	end = PAGE_ALIGN(end);
	va = (void *)start;
	size = end - start;

	unsigned long irq_flags;
	local_irq_save(irq_flags);

	/* Search for the exact matching node */
	while (node) {
		struct user_buffer_node *this_buf = rb_entry(node,
							     struct user_buffer_node,
							     node);

		unsigned long this_va = (unsigned long)this_buf->va;
		unsigned long this_end = this_va + this_buf->size;
		if (start >= this_va && start < this_end) {
			unsigned long align = LNXDRV_PFN_PAGE_SIZE(this_buf->pfndb[0]);
			start = ALIGN_DOWN(start, align);
			end = ALIGN(end, align);
			BUG_ON((void *)start != this_buf->va && end - start != this_buf->size);

			/* Drop the references to the pages of the user buffer which we
			 * increased when initializing the page structs. Note at this
			 * point a subsystem may still refer to a page, but they are
			 * expected to drop the page references immediately, before the
			 * server reuses the virtual address range for a new buffer. */
			for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
				struct page *page = virt_to_page((void *)addr);
				if (PageExternal(page)) {
					put_page(page);
				}
			}
			found_buf = this_buf;
			break;
		}

		if (start < this_va)
			node = node->rb_left;
		else
			node = node->rb_right;
	}

	/* BUG_ON if the exact range was not found */
	BUG_ON(!found_buf);

	/* Erase from tree and free memory */
	rb_erase(&found_buf->node, &user_buffers);

	local_irq_restore(irq_flags);

	kfree(found_buf);
	return 0;
}

static unsigned long user_buffer_to_paddr(const volatile void *vaddr)
{
	unsigned long va = (unsigned long)vaddr;
	struct rb_node *node = user_buffers.rb_node;
	while (node) {
		struct user_buffer_node *buf = rb_entry(node,
							struct user_buffer_node,
							node);
		unsigned long buf_va = (unsigned long)buf->va;
		unsigned long buf_end = buf_va + buf->size;
		if (va >= buf_va && va < buf_end) {
			unsigned long target_offset = va - buf_va;
			unsigned long offset = 0;
			for (int i = 0; i < buf->pfndb_size; i++) {
				unsigned long pfn = buf->pfndb[i];
				unsigned long page_size = LNXDRV_PFN_PAGE_SIZE(pfn);
				unsigned long page_count = LNXDRV_PFN_PAGE_COUNT(pfn);
				unsigned long size = page_size * page_count;
				if (target_offset >= offset && target_offset < offset + size) {
					unsigned long pa_start = LNXDRV_PFN_PAGE_ADDRESS(pfn);
					return pa_start + target_offset - offset;
				}
				offset += size;
			}
			BUG_ON(1);
		}

		if (va < buf_va)
			node = node->rb_left;
		else
			node = node->rb_right;
	}
	BUG_ON(1);
	return 0;
}

static unsigned long map_virt_to_phy(const volatile void *vaddr)
{
	unsigned long idx_l0 = PADDR_MAP_L0_INDEX(vaddr);
	unsigned long idx_l1 = PADDR_MAP_L1_INDEX(vaddr);
	unsigned long idx_outer = PADDR_MAP_OUTER_INDEX(vaddr);
#ifdef CONFIG_64BIT
	if (!paddr_map[idx_outer]) {
		return 0;
	}
	unsigned long idx_l2 = PADDR_MAP_L2_INDEX(vaddr);
	unsigned long *table = paddr_map[idx_outer][idx_l2];
#else
	unsigned long *table = paddr_map[idx_outer];
#endif
	if (!table || !table[idx_l1]) {
		return 0;
	}
	return table[idx_l1] | idx_l0;
}

struct page *__virt_to_page(const volatile void *vaddr)
{
	vaddr = (void *)((unsigned long)vaddr & ~(PAGE_SIZE - 1));
	if (vmalloc_initialized && is_vmalloc_addr((void *)vaddr)) {
		return vmalloc_to_page((void *)vaddr);
	}

	unsigned long paddr = map_virt_to_phy(vaddr);
	if (!paddr) {
		paddr = user_buffer_to_paddr(vaddr);
	}
	if (!__pfn_to_section(PHYS_PFN(paddr))) {
		return NULL;
	}
	return phys_to_page(paddr);
}
EXPORT_SYMBOL(__virt_to_page);
