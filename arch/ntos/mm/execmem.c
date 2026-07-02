#define pr_fmt(fmt) "execmem: " fmt

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/execmem.h>
#include "internal.h"

void *execmem_alloc(enum execmem_type type, size_t size)
{
	return vmalloc_modules(size);
}

void *execmem_alloc_rw(enum execmem_type type, size_t size)
{
	return vmalloc_modules(size);
}

void execmem_free(void *ptr)
{
	vfree_modules(ptr);
}

bool execmem_is_rox(enum execmem_type type)
{
	return type == EXECMEM_DEFAULT;
}
