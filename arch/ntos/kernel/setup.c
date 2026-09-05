#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>
#include <linux/tick.h>
#include <linux/async.h>
#include <host_ops.h>
#include <init.h>

DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_ALLOC_DEFAULT_ON, init_on_alloc);
EXPORT_SYMBOL(init_on_alloc);

DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
EXPORT_SYMBOL(init_on_free);

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

struct pglist_data __refdata contig_page_data;
EXPORT_SYMBOL(contig_page_data);

atomic_long_t vm_zone_stat[NR_VM_ZONE_STAT_ITEMS] __cacheline_aligned_in_smp;
atomic_long_t vm_node_stat[NR_VM_NODE_STAT_ITEMS] __cacheline_aligned_in_smp;
EXPORT_SYMBOL(vm_zone_stat);
EXPORT_SYMBOL(vm_node_stat);

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

bool initcall_debug;

struct kobject *kernel_kobj;
EXPORT_SYMBOL_GPL(kernel_kobj);

int overflowuid = DEFAULT_OVERFLOWUID;
int overflowgid = DEFAULT_OVERFLOWGID;
EXPORT_SYMBOL(overflowuid);
EXPORT_SYMBOL(overflowgid);
int fs_overflowuid = DEFAULT_FS_OVERFLOWUID;
int fs_overflowgid = DEFAULT_FS_OVERFLOWGID;
EXPORT_SYMBOL(fs_overflowuid);
EXPORT_SYMBOL(fs_overflowgid);

static int __init ksysfs_init(void)
{
	int error = 0;

	kernel_kobj = kobject_create_and_add("kernel", NULL);
	if (!kernel_kobj) {
		error = -ENOMEM;
		goto exit;
	}
exit:
	return error;
}

static inline char dash2underscore(char c)
{
	if (c == '-')
		return '_';
	return c;
}

/* Check for early params. */
static void __init set_param(char *param, char *val, bool early)
{
	for (const struct obs_kernel_param *p = __setup_start; p < __setup_end; p++) {
		if ((!early || p->early) && parameq(param, p->str)) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
			return;
		}
	}
	pr_warn("unknown %sparameter %s=%s", early ? "early " : "", param, val);
}

int __init lnxdrv_init(void)
{
	struct task_struct dummy_task = {
		.usage = REFCOUNT_INIT(1)
	};
	current = &dummy_task;

	no_hash_pointers = true;
	set_param("no_hash_pointers", NULL, true);
	init_mem();
	maple_tree_init();
	radix_tree_init();
	workqueue_init_early();
	early_irq_init();
	srcu_init();
	rcu_init();
	ksysfs_init();
	init_timer();
	workqueue_init();
	kmem_cache_init_late();
	wait_bit_init();
	init_panic();
	workqueue_init_topology();
	async_init();

	current = NULL;
	return 0;
}

extern void __init chrdev_init(void);

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
}

static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall_start,
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64] = {};
	int ret = fn();

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	return ret;
}

static void __init do_initcalls(void)
{
	for (int level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		for (initcall_entry_t *fn = initcall_levels[level];
		     fn < initcall_levels[level+1]; fn++) {
			do_one_initcall(initcall_from_entry(fn));
		}
	}
}

int __init lnxdrv_init_driver(void *nt_driver_object_handle,
			      void *kevent)
{
	KERNEL_THREAD_ENTER(kevent);

	init_lnxdrv_core(nt_driver_object_handle);
	init_file_table();
	inode_init();
	chrdev_init();
	driver_init();
	do_ctors();
	do_initcalls();

	KERNEL_THREAD_EXIT;
	return 0;
}

int register_reboot_notifier(struct notifier_block *nb)
{
	return 0;
}
EXPORT_SYMBOL(register_reboot_notifier);

struct sys_off_handler *
register_sys_off_handler(enum sys_off_mode mode,
			 int priority,
			 int (*callback)(struct sys_off_data *data),
			 void *cb_data)
{
	/* TODO */
	return NULL;
}
EXPORT_SYMBOL_GPL(register_sys_off_handler);

void unregister_sys_off_handler(struct sys_off_handler *handler)
{
	/* TODO */
}
EXPORT_SYMBOL_GPL(unregister_sys_off_handler);

/**
 * orderly_poweroff - Trigger an orderly system poweroff
 * @force: force poweroff if command execution fails
 *
 * We don't allow drivers to trigger a system poweroff or reboot, so at
 * this point this routine is a no-op. This routine is used, for instance,
 * by the nvidia GPU driver to trigger a system shutdown when thermal is
 * above critical, so eventually we should implement a notification so
 * NTOS can shutdown the system.
 */
void orderly_poweroff(bool force)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(orderly_poweroff);

void emergency_restart(void)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(emergency_restart);
