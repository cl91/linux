#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/wake_q.h>
#include <host_ops.h>

DEFINE_PER_CPU(struct kernel_stat, kstat);
DEFINE_PER_CPU(struct kernel_cpustat, kernel_cpustat);

EXPORT_PER_CPU_SYMBOL(kstat);
EXPORT_PER_CPU_SYMBOL(kernel_cpustat);

struct cred dummy_cred;
struct pid dummy_pids[PIDTYPE_MAX] = {
	[0] = { .count = {1} },
	[1] = { .count = {1} },
	[2] = { .count = {1} },
	[3] = { .count = {1} },
};
struct signal_struct dummy_signal = {
	.pids = { dummy_pids, dummy_pids + 1, dummy_pids + 2, dummy_pids + 3 }
};

struct task_struct *current;
EXPORT_SYMBOL(current);

void free_task(struct task_struct *tsk)
{
	/* A task_struct is either allocated by the host process or as part of a
	 * workqueue, so this routine is a no-op. */
}
EXPORT_SYMBOL(free_task);

void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(refcount_read(&tsk->usage));
	WARN_ON(tsk == current);
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(__put_task_struct);

void __put_task_struct_rcu_cb(struct rcu_head *rhp)
{
        struct task_struct *task = container_of(rhp, struct task_struct, rcu);

        __put_task_struct(task);
}
EXPORT_SYMBOL_GPL(__put_task_struct_rcu_cb);

/*
 * Checks if the current task is in a sleep state and sleep on its KEVENT.
 * Otherwise, this routine does nothing.
 */
asmlinkage __visible void schedule(void)
{
	BUG_ON(!current);
	unsigned int state = get_current_state();
	if (state | (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)) {
		void wq_worker_sleeping(struct task_struct *task);
		void wq_worker_running(struct task_struct *task);
		if (current->flags & PF_WQ_WORKER)
			wq_worker_sleeping(current);
		ntos_wait_for_single_object(current->thread_info.kevent,
					    state | TASK_INTERRUPTIBLE);
		if (current->flags & PF_WQ_WORKER)
			wq_worker_running(current);
	}
}
EXPORT_SYMBOL(schedule);

void schedule_preempt_disabled(void)
{
	schedule();
}

int io_schedule_prepare(void)
{
	return 0;
}

void io_schedule_finish(int token)
{
	/* Do nothing */
}

long io_schedule_timeout(long timeout)
{
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(io_schedule_timeout);

void io_schedule(void)
{
	schedule();
}
EXPORT_SYMBOL(io_schedule);

/*
 * Used when the primary interrupt handler is forced into a thread, in addition
 * to the (always threaded) secondary handler.  The secondary handler gets a
 * slightly lower priority so that the primary handler can preempt it, thereby
 * emulating the behavior of a non-PREEMPT_RT system where the primary handler
 * runs in hard interrupt context.
 */
void sched_set_fifo_secondary(struct task_struct *p)
{
	/* Do nothing */
}

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesn't have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(struct task_struct *p)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(kick_process);

/*
 * Set the KEVENT of the given task which will wake the task up.
 */
int wake_up_process(struct task_struct *p)
{
	p->__state &= ~(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE);
	if (p->thread_info.kevent) {
		ntos_set_event(p->thread_info.kevent);
	} else {
		ntos_queue_softirq_dpc(NULL, NULL);
	}
	return 0;
}
EXPORT_SYMBOL(wake_up_process);

/* We never reschedule a task. */
int __cond_resched(void)
{
	return 0;
}
EXPORT_SYMBOL(__cond_resched);

int __cond_resched_lock(spinlock_t *lock)
{
	return 0;
}
EXPORT_SYMBOL(__cond_resched_lock);

int __cond_resched_rwlock_read(rwlock_t *lock)
{
	return 0;
}
EXPORT_SYMBOL(__cond_resched_rwlock_read);

extern void resched_cpu(int cpu);
void resched_cpu(int cpu)
{
	/* This routine is a no-op in our thread model. */
}

void set_user_nice(struct task_struct *p, long nice)
{
	/* Do nothing */
}
EXPORT_SYMBOL(set_user_nice);

int rcuwait_wake_up(struct rcuwait *w)
{
        int ret = 0;
        struct task_struct *task;

        rcu_read_lock();

        /*
         * Order condition vs @task, such that everything prior to the load
         * of @task is visible. This is the condition as to why the user called
         * rcuwait_wake() in the first place. Pairs with set_current_state()
         * barrier (A) in rcuwait_wait_event().
         *
         *    WAIT                WAKE
         *    [S] tsk = current   [S] cond = true
         *        MB (A)              MB (B)
         *    [L] cond            [L] tsk
         */
        smp_mb(); /* (B) */

        task = rcu_dereference(w->task);
        if (task)
                ret = wake_up_process(task);
        rcu_read_unlock();

        return ret;
}
EXPORT_SYMBOL_GPL(rcuwait_wake_up);

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * This is only used by smpboot_create_thread so we always return success (1).
 * Our thread model does not support waiting for another task to become inactive.
 */
unsigned long wait_task_inactive(struct task_struct *p, unsigned int match_state)
{
	return 1;
}

extern int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags);
int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	if (p->__state & state) {
		wake_up_process(p);
	}
	return 0;
}

int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags,
			  void *key)
{
	return try_to_wake_up(curr->private, mode, wake_flags);
}
EXPORT_SYMBOL(default_wake_function);

static bool __wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	struct wake_q_node *node = &task->wake_q;

	/*
	 * Atomically grab the task, if ->wake_q is !nil already it means
	 * it's already queued (either by us or someone else) and will get the
	 * wakeup due to that.
	 *
	 * In order to ensure that a pending wakeup will observe our pending
	 * state, even in the failed case, an explicit smp_mb() must be used.
	 */
	smp_mb__before_atomic();
	if (unlikely(cmpxchg_relaxed(&node->next, NULL, WAKE_Q_TAIL)))
		return false;

	/*
	 * The head is context local, there can be no concurrency.
	 */
	*head->lastp = node;
	head->lastp = &node->next;
	return true;
}

/**
 * wake_q_add() - queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 */
void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (__wake_q_add(head, task))
		get_task_struct(task);
}

/**
 * wake_q_add_safe() - safely queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 *
 * This function is essentially a task-safe equivalent to wake_q_add(). Callers
 * that already hold reference to @task can call the 'safe' version and trust
 * wake_q to do the right thing depending whether or not the @task is already
 * queued for wakeup.
 */
void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	if (!__wake_q_add(head, task))
		put_task_struct(task);
}

void wake_up_q(struct wake_q_head *head)
{
	struct wake_q_node *node = head->first;

	while (node != WAKE_Q_TAIL) {
		struct task_struct *task;

		task = container_of(node, struct task_struct, wake_q);
		node = node->next;
		/* pairs with cmpxchg_relaxed() in __wake_q_add() */
		WRITE_ONCE(task->wake_q.next, NULL);
		/* Task can safely be re-inserted now. */

		/*
		 * wake_up_process() executes a full barrier, which pairs with
		 * the queueing in wake_q_add() so as not to miss wakeups.
		 */
		wake_up_process(task);
		put_task_struct(task);
	}
}

void yield(void)
{
	/* This routine is a no-op in our scheduling model. */
}
EXPORT_SYMBOL(yield);

void sched_set_fifo(struct task_struct *p)
{
	/* This routine is a no-op in our scheduling model. */
}
EXPORT_SYMBOL_GPL(sched_set_fifo);

#ifdef CONFIG_RT_MUTEXES

void rt_mutex_pre_schedule(void)
{
	/* Do nothing */
}

void rt_mutex_schedule(void)
{
	/* Do nothing */
}

void rt_mutex_post_schedule(void)
{
	/* Do nothing */
}

void rt_mutex_setprio(struct task_struct *p, struct task_struct *pi_task)
{
	/* Do nothing */
}

#endif	/* CONFIG_RT_MUTEXES */
