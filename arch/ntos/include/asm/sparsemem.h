/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NTOS_SPARSEMEM_H
#define _NTOS_SPARSEMEM_H

#ifndef CONFIG_SPARSEMEM
#error "You must enable SPARSEMEM"
#endif

#ifdef CONFIG_SPARSEMEM_VMEMMAP
#error "You must disable SPARSEMEM_VMEMMAP"
#endif

#include <asm/bitsperlong.h>

/*
 * SECTION_SIZE_BITS		2^N: how big each section will be
 * This must be no less than MAX_PAGE_ORDER + PAGE_SHIFT
 */
#define SECTION_SIZE_BITS	22

/*
 * MAX_PHYSMEM_BITS		2^N: size of the physical address space
 */
#if BITS_PER_LONG == 32
#define MAX_PHYSMEM_BITS	32
#else
#define MAX_PHYSMEM_BITS	40 /* two pages for the outer dim of mem_section array */
#endif

#endif /* _NTOS_SPARSEMEM_H */
