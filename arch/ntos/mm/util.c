#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/backing-dev.h>
#include <linux/pagemap.h>
#include <host_ops.h>
#include "internal.h"

struct backing_dev_info noop_backing_dev_info;
EXPORT_SYMBOL_GPL(noop_backing_dev_info);

int bdi_init(struct backing_dev_info *bdi)
{
	memset(bdi, 0, sizeof(struct backing_dev_info));
	kref_init(&bdi->refcnt);
	bdi->max_ratio = 100 * BDI_RATIO_SCALE;
	bdi->max_prop_frac = FPROP_FRAC_BASE;
	INIT_LIST_HEAD(&bdi->bdi_list);
	INIT_LIST_HEAD(&bdi->wb_list);
	init_waitqueue_head(&bdi->wb_waitq);
	bdi->last_bdp_sleep = jiffies;
	return 0;
}

struct backing_dev_info *inode_to_bdi(struct inode *inode)
{
        struct super_block *sb;

        if (!inode)
                return &noop_backing_dev_info;

        sb = inode->i_sb;
        return sb->s_bdi;
}
EXPORT_SYMBOL(inode_to_bdi);

int get_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	if (buflen) {
		buffer[0] = '\0';
	}
	return 0;
}

long copy_from_kernel_nofault(void *dst, const void *src, size_t size)
{
	memcpy(dst, src, size);
	return 0;
}
EXPORT_SYMBOL_GPL(copy_from_kernel_nofault);

long strncpy_from_kernel_nofault(char *dst, const void *unsafe_addr, long count)
{
	strncpy(dst, unsafe_addr, count);
	return count;
}

long copy_to_user_nofault(void __user *dst, const void *src, size_t size)
{
	memcpy(dst, src, size);
	return 0;
}

size_t fault_in_readable(const char __user *uaddr, size_t size)
{
	/* Do nothing */
	return 0;
}

size_t fault_in_safe_writeable(const char __user *uaddr, size_t size)
{
	/* Do nothing */
	return 0;
}

void __iomem *ioremap(phys_addr_t offset, size_t size)
{
	return ntos_map_io_space(offset, size, LnxDrvMemNonCached);
}
EXPORT_SYMBOL(ioremap);

void __iomem *ioremap_wc(phys_addr_t offset, size_t size)
{
	return ntos_map_io_space(offset, size, LnxDrvMemWriteCombined);
}
EXPORT_SYMBOL(ioremap_wc);

void __iomem *ioremap_wt(phys_addr_t offset, size_t size)
{
	return ntos_map_io_space(offset, size, LnxDrvMemWriteThrough);
}
EXPORT_SYMBOL(ioremap_wt);

void iounmap(volatile void __iomem *addr)
{
	ntos_unmap_io_space((void *)addr, 1);
}
EXPORT_SYMBOL(iounmap);

void __copy_overflow(int size, unsigned long count)
{
        WARN(1, "Buffer overflow detected (%d < %lu)!\n", size, count);
}
EXPORT_SYMBOL(__copy_overflow);

void si_meminfo(struct sysinfo *val)
{
        val->totalram = _totalram_pages.counter;
        val->sharedram = 0;
        val->freeram = _freeram_pages;
        val->bufferram = 0;
        val->totalhigh = val->totalram;
        val->freehigh = val->freeram;
        val->mem_unit = PAGE_SIZE;
}
EXPORT_SYMBOL(si_meminfo);

unsigned long _freeram_pages;

atomic_long_t _totalram_pages __read_mostly;
EXPORT_SYMBOL(_totalram_pages);
