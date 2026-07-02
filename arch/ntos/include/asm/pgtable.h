/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NTOS_PGTABLE_H
#define _NTOS_PGTABLE_H

/*
 * (C) Copyright 2000-2002, Greg Ungerer <gerg@snapgear.com>
 */

#include <asm/page.h>
#include <asm-generic/pgtable-nopud.h>
#include <asm/processor.h>

#define pgd_present(pgd)	(1)
#define pgd_none(pgd)		(0)
#define pgd_bad(pgd)		(0)
#define pgd_clear(pgdp)
#define kern_addr_valid(addr)	(1)
#define	pmd_offset(a, b)	((void *)0)

#define swapper_pg_dir		((pgd_t *)0)

#define __swp_type(x)		(0)
#define __swp_offset(x)		(0)
#define __swp_entry(typ, off)	((swp_entry_t) { ((typ) | ((off) << 7)) })
#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
extern void *empty_zero_page;
#define ZERO_PAGE(vaddr)	(virt_to_page(empty_zero_page))

#define PTRS_PER_PTE 0
#define PTRS_PER_PMD 0

#ifdef __i386__
#define PMD_SHIFT		22
#elif defined(__x86_64__)
#define PMD_SHIFT		21
#elif defined(__aarch64__)
#define PMD_SHIFT		21
#else
#error "Unsupported architecture"
#endif

#ifndef __ASSEMBLY__
extern unsigned long vmalloc_base;
#endif

#define VMALLOC_START		vmalloc_base
#ifdef CONFIG_64BIT
#define VMALLOC_SIZE		(1024 * 1024 * 1024 * 1024ULL)
#else
#define VMALLOC_SIZE		(1024 * 1024 * 1024UL)
#endif
#define VMALLOC_END		(VMALLOC_START + VMALLOC_SIZE)

#define PAGE_KERNEL		__pgprot(0)
#define PAGE_SHARED		__pgprot(0)
#define PAGE_KERNEL_IO		__pgprot(0)

#ifdef CONFIG_TARGET_ARCH_X86
typedef unsigned long pteval_t;

#define _PAGE_BIT_PWT           3       /* page write through */
#define _PAGE_BIT_PCD           4       /* page cache disabled */
#define _PAGE_BIT_PAT           7       /* on 4KB pages */

#define _PAGE_PWT       (_AT(pteval_t, 1) << _PAGE_BIT_PWT)
#define _PAGE_PCD       (_AT(pteval_t, 1) << _PAGE_BIT_PCD)
#define _PAGE_PAT       (_AT(pteval_t, 1) << _PAGE_BIT_PAT)

#define pat_enabled() true

#define _PAGE_CACHE_MASK        (_PAGE_PWT | _PAGE_PCD | _PAGE_PAT)

#define _PAGE_CACHE_MODE_WB     (0)
#define _PAGE_CACHE_MODE_WT     (_PAGE_PWT)
#define _PAGE_CACHE_MODE_UC     (_PAGE_PCD | _PAGE_PWT)
#define _PAGE_CACHE_MODE_WC     (_PAGE_PAT)

#define pgprot_writecombine(prot) \
    ((pgprot_t) { ((prot).pgprot & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_MODE_WC })

#define pgprot_noncached(prot) \
    ((pgprot_t) { ((prot).pgprot & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_MODE_UC })

#define pgprot_writethrough(prot) \
    ((pgprot_t) { ((prot).pgprot & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_MODE_WT })
#endif

#endif
