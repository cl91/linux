/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_NTOS_PAGE_H
#define _ASM_NTOS_PAGE_H

/* PAGE_SHIFT determines the page size */

#define PAGE_SHIFT	12
#ifdef __ASSEMBLY__
#define PAGE_SIZE	(1 << PAGE_SHIFT)
#else
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#endif
#define PAGE_MASK	(~(PAGE_SIZE-1))

#ifndef __ASSEMBLY__

#define clear_page(page)	memset((page), 0, PAGE_SIZE)
#define copy_page(to,from)	memcpy((to), (from), PAGE_SIZE)

#define clear_user_page(page, vaddr, pg)	\
	({(void)(vaddr); (void)(pg); clear_page(page);})
#define copy_user_page(to, from, vaddr, pg)	\
	({(void)(vaddr); (void)(pg); copy_page(to, from);})

/*
 * These are used to make use of C type-checking.
 */
typedef struct {
	unsigned long pte;
} pte_t;
typedef struct {
	unsigned long pmd;
} pmd_t;
typedef struct {
	unsigned long pgd;
} pgd_t;
typedef struct {
	unsigned long pgprot;
} pgprot_t;
typedef struct page *pgtable_t;

#define pte_val(x)	((x).pte)
#define pmd_val(x)	((x).pmd)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x)	((pte_t) { (x) } )
#define __pmd(x)	((pmd_t) { (x) } )
#define __pgd(x)	((pgd_t) { (x) } )
#define __pgprot(x)	((pgprot_t) { (x) } )

extern struct page *__virt_to_page(const volatile void *vaddr);

#define WANT_PAGE_VIRTUAL

#define virt_to_page(addr)	(__virt_to_page(addr))
#define page_to_virt(page)	((page)->virtual)

#define __va(x)								\
	({								\
		unsigned long _pa = (x);				\
		void *_va;						\
		if (!_pa) {						\
			_va = NULL;					\
		} else if (_pa == ~0UL) {				\
			_va = (void *)(~0UL);				\
		} else {						\
			_va = page_to_virt(pfn_to_page(PHYS_PFN(_pa)));	\
			_va = (char *)_va + (_pa & (PAGE_SIZE - 1));	\
		}							\
		(_va);							\
	})

#define __pa(x)								\
	({								\
		void *_va = (void *)(x);				\
		unsigned long _pa;					\
		if (_va == NULL) {					\
			_pa = 0UL;					\
		} else if (_va == (void *)~0UL) {			\
			_pa = ~0UL;					\
		} else {						\
			_pa = page_to_phys(__virt_to_page(_va)) +	\
				((unsigned long)_va & (PAGE_SIZE - 1));	\
		}							\
		(_pa);							\
	})

#define virt_addr_valid(kaddr)  (__virt_to_page(kaddr) != NULL)

#endif /* __ASSEMBLY__ */

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#undef PAGE_OFFSET

#endif /* _ASM_NTOS_PAGE_H */
