#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/slab.h>
#include <host_ops.h>
#include <init.h>

struct kthread {
	struct task_struct task;
	int (*threadfn)(void *);
	void *data;
	void *work_item;
	void *terminated_event;
	int result;
	bool should_park;
	bool should_stop;
};

static inline struct kthread *to_kthread(struct task_struct *k)
{
	BUG_ON(!(k->flags & PF_KTHREAD));
	BUG_ON(container_of(k, struct kthread, task) != k->worker_private);
	return container_of(k, struct kthread, task);
}

static void free_kthread(struct kthread *t)
{
	if (t->work_item) {
		ntos_free_work_item(t->work_item);
	}
	if (t->task.thread_info.kevent) {
		ntos_free_event(t->task.thread_info.kevent);
	}
	if (t->terminated_event) {
		ntos_free_event(t->terminated_event);
	}
	kfree(t);
}

static void kthread_worker_routine(void *work_item,
				   void *ctx)
{
	struct kthread *t = ctx;
	BUG_ON(t->work_item != work_item);
	ntos_wait_for_single_object(t->task.thread_info.kevent, 0);
	BUG_ON(current);
	if (!t->should_stop) {
		current = &t->task;
		current->__state = TASK_RUNNING;
		t->result = t->threadfn(t->data);
	}
	ntos_set_event(t->terminated_event);
}

__printf(4, 0) struct task_struct *__kthread_create_on_node(int (*threadfn)(void *data),
							    void *data, int node,
							    const char namefmt[],
							    va_list args)
{
	struct kthread *t = kzalloc(sizeof(struct kthread), GFP_KERNEL);
	t->work_item = ntos_allocate_work_item();
	if (!t->work_item) {
		pr_info("failed to allocate work_item\n");
		free_kthread(t);
		return NULL;
	}
	t->task.thread_info.kevent = ntos_allocate_event(0);
	if (!t->task.thread_info.kevent) {
		pr_info("failed to allocate event for kthread\n");
		free_kthread(t);
		return NULL;
	}
	t->terminated_event = ntos_allocate_event(0);
	if (!t->terminated_event) {
		pr_info("failed to allocate termination event for kthread\n");
		free_kthread(t);
		return NULL;
	}
	t->task.flags = PF_KTHREAD;
	t->task.worker_private = t;
	t->task.__state = TASK_INTERRUPTIBLE;
	t->task.cred = &dummy_cred;
	t->task.signal = &dummy_signal;
	refcount_set(&t->task.usage, 1);
	vsnprintf(t->task.comm, sizeof(t->task.comm), namefmt, args);
	t->threadfn = threadfn;
	t->data = data;
	ntos_queue_work_item(t->work_item, kthread_worker_routine, t);
	return &t->task;
}

/**
 * kthread_create_on_node - create a kthread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @node: task and thread structures for the thread are allocated on this node
 * @namefmt: printf-style name for the thread.
 *
 * Description: This helper function creates and names a kernel
 * thread.  The thread will be stopped: use wake_up_process() to start
 * it.  See also kthread_run().  The new thread has SCHED_NORMAL policy and
 * is affine to all CPUs.
 *
 * If thread is going to be bound on a particular cpu, give its node
 * in @node, to get NUMA affinity for kthread stack, or else give NUMA_NO_NODE.
 * When woken, the thread will run @threadfn() with @data as its
 * argument. @threadfn() can either return directly if it is a
 * standalone thread for which no one will call kthread_stop(), or
 * return when 'kthread_should_stop()' is true (which means
 * kthread_stop() has been called).  The return value should be zero
 * or a negative error number; it will be passed to kthread_stop().
 *
 * Returns a task_struct or ERR_PTR(-ENOMEM) or ERR_PTR(-EINTR).
 */
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	va_list args;
	va_start(args, namefmt);
	struct task_struct *task = __kthread_create_on_node(threadfn, data, node,
							    namefmt, args);
	va_end(args);
	return task;
}
EXPORT_SYMBOL(kthread_create_on_node);

/**
 * kthread_create_on_cpu - Create a cpu bound kthread
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @cpu: The cpu on which the thread should be bound,
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Description: This helper function creates and names a kernel thread
 */
struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
					  void *data, unsigned int cpu,
					  const char *namefmt)
{
	struct task_struct *p;

	p = kthread_create_on_node(threadfn, data, 0, namefmt, cpu);
	if (IS_ERR(p))
		return p;
	return p;
}
EXPORT_SYMBOL(kthread_create_on_cpu);

void kthread_set_per_cpu(struct task_struct *k, int cpu)
{
	/* Do nothing */
}

/**
 * kthread_should_park - should this kthread park now?
 *
 * When someone calls kthread_park() on your kthread, it will be woken
 * and this will return true.  You should then do the necessary
 * cleanup and call kthread_parkme()
 *
 * Similar to kthread_should_stop(), but this keeps the thread alive
 * and in a park position. kthread_unpark() "restarts" the thread and
 * calls the thread function again.
 */
bool kthread_should_park(void)
{
	return to_kthread(current)->should_park;
}
EXPORT_SYMBOL_GPL(kthread_should_park);

/**
 * kthread_park - park a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return true, wakes it, and
 * waits for it to return. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will park without
 * calling threadfn().
 *
 * Returns 0 if the thread is parked, -ENOSYS if the thread exited.
 * If called by the kthread itself just the park bit is set.
 */
int kthread_park(struct task_struct *t)
{
	struct kthread *k = to_kthread(t);
	k->should_park = true;
	t->__state = TASK_INTERRUPTIBLE;
	ntos_clear_event(t->thread_info.kevent);
	return 0;
}
EXPORT_SYMBOL_GPL(kthread_park);

/**
 * kthread_unpark - unpark a thread created by kthread_create().
 * @k:		thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return false, wakes it, and
 * waits for it to return. If the thread is marked percpu then its
 * bound to the cpu again.
 */
void kthread_unpark(struct task_struct *t)
{
	struct kthread *k = to_kthread(t);
	k->should_park = false;
	wake_up_process(t);
	ntos_wait_for_single_object(k->terminated_event, 0);
}
EXPORT_SYMBOL_GPL(kthread_unpark);

/**
 * kthread_parkme - park the current kthread. You must call this only after
 *                  kthread_should_park() returns true.
 */
void kthread_parkme(void)
{
	BUG_ON(!kthread_should_park());
	BUG_ON(current->__state != TASK_INTERRUPTIBLE);
	schedule();
}
EXPORT_SYMBOL_GPL(kthread_parkme);

/**
 * kthread_should_stop - should this kthread return now?
 *
 * When someone calls kthread_stop() on your kthread, it will be woken
 * and this will return true.  You should then return, and your return
 * value will be passed through to kthread_stop().
 */
bool kthread_should_stop(void)
{
	return to_kthread(current)->should_stop;
}
EXPORT_SYMBOL(kthread_should_stop);

/**
 * kthread_stop - stop a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_stop() for @k to return true, wakes it, and
 * waits for it to exit. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will exit without
 * calling threadfn().
 *
 * If threadfn() may call kthread_exit() itself, the caller must ensure
 * task_struct can't go away.
 *
 * Returns the result of threadfn(), or %-EINTR if wake_up_process()
 * was never called.
 */
int kthread_stop(struct task_struct *t)
{
	struct kthread *k = to_kthread(t);
	k->should_stop = true;
	k->should_park = false;
	wake_up_process(t);
	ntos_wait_for_single_object(k->terminated_event, 0);
	int ret = k->result;
	free_kthread(k);
	return ret;
}
EXPORT_SYMBOL(kthread_stop);

/**
 * kthread_stop_put - stop a thread and put its task struct
 * @k: thread created by kthread_create().
 *
 * Stops a thread created by kthread_create() and put its task_struct.
 * Only use when holding an extra task struct reference obtained by
 * calling get_task_struct().
 */
int kthread_stop_put(struct task_struct *k)
{
	int ret;

	ret = kthread_stop(k);
	put_task_struct(k);
	return ret;
}
EXPORT_SYMBOL(kthread_stop_put);

/**
 * kthread_bind - bind a just-created kthread to a cpu.
 * @p: thread created by kthread_create().
 * @cpu: cpu (might not be online, must be possible) for @k to run on.
 *
 * Description: This function is equivalent to set_cpus_allowed(),
 * except that @cpu doesn't need to be online, and the thread must be
 * stopped (i.e., just returned from kthread_create()).
 */
void kthread_bind(struct task_struct *p, unsigned int cpu)
{
	/* Do nothing */
}
EXPORT_SYMBOL(kthread_bind);

void kthread_bind_mask(struct task_struct *p, const struct cpumask *mask)
{
	/* Do nothing */
}

/**
 * kthread_data - return data value specified on kthread creation
 * @task: kthread task in question
 *
 * Return the data value specified when kthread @task was created.
 * The caller is responsible for ensuring the validity of @task when
 * calling this function.
 */
void *kthread_data(struct task_struct *task)
{
	return to_kthread(task)->data;
}
EXPORT_SYMBOL_GPL(kthread_data);
