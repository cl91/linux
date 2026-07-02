#include <linux/kernel.h>
#include <linux/cpuhotplug.h>
#include <linux/sched/stat.h>
#include <host_ops.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>

#ifdef CONFIG_TARGET_ARCH_X86
struct cpuinfo_x86 boot_cpu_data = {
	.x86_clflush_size = 64
};
EXPORT_SYMBOL(boot_cpu_data);
#endif

struct cpumask __cpu_possible_mask __read_mostly = { .bits = { 1 } };
EXPORT_SYMBOL(__cpu_possible_mask);

struct cpumask __cpu_enabled_mask __read_mostly = { .bits = { 1 } };
EXPORT_SYMBOL(__cpu_enabled_mask);

struct cpumask __cpu_online_mask __read_mostly = { .bits = { 1 } };
EXPORT_SYMBOL(__cpu_online_mask);

struct cpumask __cpu_present_mask __read_mostly = { .bits = { 1 } };
EXPORT_SYMBOL(__cpu_present_mask);

struct cpumask __cpu_active_mask __read_mostly = { .bits = { 1 } };
EXPORT_SYMBOL(__cpu_active_mask);

/*
 * cpu_bit_bitmap[] is a special, "compressed" data structure that
 * represents all NR_CPUS bits binary values of 1<<nr.
 *
 * It is used by cpumask_of() to get a constant address to a CPU
 * mask value that has a single bit set only.
 */

/* cpu_bit_bitmap[0] is empty - so we can back into it */
#define MASK_DECLARE_1(x)	[x+1][0] = (1UL << (x))
#define MASK_DECLARE_2(x)	MASK_DECLARE_1(x), MASK_DECLARE_1(x+1)
#define MASK_DECLARE_4(x)	MASK_DECLARE_2(x), MASK_DECLARE_2(x+2)
#define MASK_DECLARE_8(x)	MASK_DECLARE_4(x), MASK_DECLARE_4(x+4)

const unsigned long cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)] = {

	MASK_DECLARE_8(0),	MASK_DECLARE_8(8),
	MASK_DECLARE_8(16),	MASK_DECLARE_8(24),
#if BITS_PER_LONG > 32
	MASK_DECLARE_8(32),	MASK_DECLARE_8(40),
	MASK_DECLARE_8(48),	MASK_DECLARE_8(56),
#endif
};
EXPORT_SYMBOL_GPL(cpu_bit_bitmap);

int __cpuhp_setup_state(enum cpuhp_state state,
			const char *name, bool invoke,
			int (*startup)(unsigned int cpu),
			int (*teardown)(unsigned int cpu),
			bool multi_instance)
{
	/* Do nothing */
	return 0;
}

/*
 * Preemption is disabled here to make sure the cond_func is called under the
 * same conditions in UP and SMP.
 */
void on_each_cpu_cond_mask(smp_cond_func_t cond_func, smp_call_func_t func,
                           void *info, bool wait, const struct cpumask *mask)
{
        unsigned long flags;

        if ((!cond_func || cond_func(0, info)) && cpumask_test_cpu(0, mask)) {
                local_irq_save(flags);
                func(info);
                local_irq_restore(flags);
        }
}
EXPORT_SYMBOL(on_each_cpu_cond_mask);

int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	/* Do nothing */
        return 0;
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);
