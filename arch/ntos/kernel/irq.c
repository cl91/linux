#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irq_work.h>
#include <asm/irq_regs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/msi.h>
#include <linux/irqdomain.h>
#include <asm/irqflags.h>
#include <host_ops.h>
#include <init.h>

static bool interrupt_disabled;
static unsigned long interrupt_pending;
static long local_bh_depth;

static bool softirq_pending_state[NR_SOFTIRQS];

static inline bool softirq_pending(void)
{
	for (int i = 0; i < NR_SOFTIRQS; i++) {
		if (softirq_pending_state[i]) {
			return true;
		}
	}
	return false;
}

void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	BUG_ON(local_bh_depth < 0);
	local_bh_depth++;
}
EXPORT_SYMBOL(__local_bh_disable_ip);

void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
	BUG_ON(local_bh_depth <= 0);
	local_bh_depth--;
	if (!local_bh_depth && softirq_pending()) {
		ntos_queue_softirq_dpc(NULL, NULL);
	}
}
EXPORT_SYMBOL(__local_bh_enable_ip);

static void deliver_irqs(void);

unsigned long arch_local_save_flags(void)
{
	return interrupt_disabled ? ARCH_IRQ_DISABLED : ARCH_IRQ_ENABLED;
}
EXPORT_SYMBOL(arch_local_save_flags);

void arch_local_irq_restore(unsigned long flags)
{
	if (flags == ARCH_IRQ_DISABLED) {
		interrupt_disabled = true;
	} else if (flags == ARCH_IRQ_ENABLED) {
		interrupt_disabled = false;
		deliver_irqs();
		if (softirq_pending()) {
			ntos_queue_softirq_dpc(NULL, NULL);
		}
	}
}
EXPORT_SYMBOL(arch_local_irq_restore);

struct ntos_irq_chip_data {
	void *introbj;		/* NT interrupt object */
	unsigned int hwirq;	/* NT IRQ line */
	struct list_head entry;
	unsigned long pending;
};

static LIST_HEAD(ntos_irq_list);
static struct irq_domain *ntos_irq_domain;

static void handle_irq(struct ntos_irq_chip_data *chip_data)
{
	BUG_ON(!interrupt_disabled);
	struct task_struct *old_current = current;
	/* We cannot sleep in irq context, so the kevent of the task_struct is NULL. */
	struct task_struct dummy = {
		.thread_info.preempt_count = 1 << HARDIRQ_SHIFT,
		.cred = &dummy_cred,
		.signal = &dummy_signal,
		.usage = REFCOUNT_INIT(1),
	};
	current = &dummy;
	generic_handle_domain_irq(ntos_irq_domain, chip_data->hwirq);
	current = old_current;
}

static void deliver_irqs(void)
{
	if (test_and_clear_bit(0, &interrupt_pending)) {
		BUG_ON(interrupt_disabled);
		local_irq_disable();
		struct ntos_irq_chip_data *chip_data;
		list_for_each_entry(chip_data, &ntos_irq_list, entry) {
			if (test_and_clear_bit(0, &chip_data->pending)) {
				handle_irq(chip_data);
			}
		}
		local_irq_enable();
	}
}

static BOOLEAN MS_ABI NTAPI REALIGN_STACK lnxdrv_isr_callback(void *introbj,
							      void *ctx)
{
	struct ntos_irq_chip_data *chip_data = ctx;
	if (interrupt_disabled) {
		interrupt_pending = true;
		chip_data->pending = true;
		return true;
	}
	local_irq_disable();
	handle_irq(chip_data);
	local_irq_enable();
	if (softirq_pending()) {
		ntos_queue_softirq_dpc(NULL, NULL);
	}
	return true;
}

static struct softirq_action softirq_vec[NR_SOFTIRQS];

static void MS_ABI NTAPI REALIGN_STACK softirq_dpc_routine(void *dpc,
							   void *ctx,
							   void *arg1,
							   void *arg2)
{
	/* If interrupt is disabled, do nothing. Softirqs will be delivered when
	 * interrupt is reenabled. */
	if (interrupt_disabled) {
		return;
	}
	/* If interrupt is pending, deliver interrupts first. */
	deliver_irqs();
	local_bh_disable();
	struct task_struct *old_current = current;
	/* We cannot sleep in softirq context, so the kevent of the task_struct is NULL. */
	struct task_struct dummy = {
		.thread_info.preempt_count = 1 << SOFTIRQ_SHIFT,
		.cred = &dummy_cred,
		.signal = &dummy_signal,
		.usage = REFCOUNT_INIT(1),
	};
	current = &dummy;
	for (int i = 0; i < NR_SOFTIRQS; i++) {
		if (xchg(&softirq_pending_state[i], false)) {
			softirq_vec[i].action();
		}
	}
	current = old_current;
	local_bh_enable();
}

void open_softirq(int nr, void (*action)(void))
{
	softirq_vec[nr].action = action;
}

void __raise_softirq_irqoff(unsigned int nr)
{
	softirq_pending_state[nr] = true;
	ntos_queue_softirq_dpc(NULL, NULL);
}

void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);
}

void raise_softirq(unsigned int nr)
{
	unsigned long flags;
	local_irq_save(flags);
	__raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

static inline struct ntos_irq_chip_data *get_ntos_irq_chip_data(struct irq_data *d)
{
	return (struct ntos_irq_chip_data *)d->chip_data;
}

static int ntos_irqdomain_map(struct irq_domain *d,
                              unsigned int virq,
                              irq_hw_number_t hwirq)
{
	struct ntos_irq_chip_data *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->hwirq = hwirq;

	ntos_connect_interrupt(&ctx->introbj, lnxdrv_isr_callback, ctx, hwirq);
	if (!ctx->introbj) {
		kfree(ctx);
		return -EINVAL;
	}

	irq_set_chip_and_handler(virq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(virq, ctx);
	unsigned long flags;
	local_irq_save(flags);
	list_add(&ctx->entry, &ntos_irq_list);
	local_irq_restore(flags);
	return 0;
}

static void ntos_irqdomain_unmap(struct irq_domain *d,
				 unsigned int virq)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);
	struct ntos_irq_chip_data *ctx = get_ntos_irq_chip_data(data);
	BUG_ON(!ctx);
	unsigned long flags;
	local_irq_save(flags);
	list_del(&ctx->entry);
	local_irq_restore(flags);
	ntos_disconnect_interrupt(ctx->introbj);
	kfree(ctx);
}

static const struct irq_domain_ops ntos_irqdomain_ops = {
	.map   = ntos_irqdomain_map,
	.unmap = ntos_irqdomain_unmap,
};

int __init arch_early_irq_init(void)
{
	ntos_irq_domain = irq_domain_create_tree(NULL, &ntos_irqdomain_ops, NULL);
	if (!ntos_irq_domain)
		panic("failed to create ntos irq domain");
	irq_set_default_domain(ntos_irq_domain);

	ntos_initialize_softirq_dpc(softirq_dpc_routine, NULL);
	return 0;
}

unsigned int arch_dynirq_lower_bound(unsigned int from)
{
	return from;
}

int __init arch_probe_nr_irqs(void)
{
	/* On driver process startup we initially do not have any connected IRQ,
	 * so return zero here. */
	return 0;
}

bool irq_work_queue(struct irq_work *work)
{
	unsigned long flags;

	local_irq_save(flags);
	work->func(work);
	local_irq_restore(flags);
	return true;
}
EXPORT_SYMBOL_GPL(irq_work_queue);

void irq_work_sync(struct irq_work *work)
{
	/* Do nothing as our irq_work is always executed immediately. */
}
EXPORT_SYMBOL_GPL(irq_work_sync);

void add_interrupt_randomness(int irq)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(add_interrupt_randomness);
