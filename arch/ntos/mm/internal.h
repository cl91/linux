#pragma once
#include <linux/mm.h>

extern bool vmalloc_initialized;
extern unsigned long _freeram_pages;

void init_vmalloc(void);

void *vmalloc_modules(unsigned long size);
void vfree_modules(const void *addr);

void remap_pfn_range_prepare(struct vm_area_desc *desc, unsigned long pfn);
int remap_pfn_range_complete(struct vm_area_struct *vma, unsigned long addr,
			     unsigned long pfn, unsigned long size, pgprot_t prot);
