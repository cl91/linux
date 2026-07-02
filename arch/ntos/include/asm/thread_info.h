#ifndef _ASM_NTOS_THREAD_INFO_H
#define _ASM_NTOS_THREAD_INFO_H

#include <linux/types.h>

#define THREAD_SIZE	       (4096)

#ifndef __ASSEMBLY__

struct thread_info {
	struct list_head                entry;
	unsigned long flags;
	void *kevent;
	int preempt_count;
};

#define INIT_THREAD_INFO(tsk)				\
{							\
	.flags		= 0,				\
	.kevent		= NULL,				\
	.preempt_count  = INIT_PREEMPT_COUNT,           \
}

#endif /* __ASSEMBLY__ */

#include <asm-generic/thread_info_tif.h>

#endif
