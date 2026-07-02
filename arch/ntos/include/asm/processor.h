#ifndef _ASM_NTOS_PROCESSOR_H
#define _ASM_NTOS_PROCESSOR_H

#include <linux/types.h>

struct task_struct;

static inline void cpu_relax(void)
{
	/* Do nothing */
}

struct thread_struct { };

#define INIT_THREAD { }

#define task_pt_regs(tsk) (struct pt_regs *)(NULL)

/* We don't have strict user/kernel spaces */
#define TASK_SIZE ((unsigned long)-1)
#define TASK_UNMAPPED_BASE	0

#ifdef CONFIG_TARGET_ARCH_X86
struct cpuinfo_x86 {
	u16 x86_clflush_size;
};
extern struct cpuinfo_x86 boot_cpu_data;
#endif

#endif
