/*
 * Implementation of timekeeping and timers. We cannot reuse the Linux kernel
 * implementations as it assumes a continuous running timer tick, even for the
 * socalled tickless kernels. Instead, we use the TSC value as the system time
 * and call NT to request the timer service to signal us when timer expires.
 */

#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/jiffies.h>
#include <linux/sched/clock.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hashtable.h>
#include <linux/irq.h>
#include <linux/gcd.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/timerqueue.h>
#include <host_ops.h>
#include "init.h"

#if defined(__i386__) || defined(__x86_64__)
/* Use the ordered version of rdtsc to get an accurate time stamp counter. */
static inline u64 rdtsc_ordered(void) {
	u32 aux;
	return __builtin_ia32_rdtscp(&aux);
}
#elif defined(__aarch64__)
/* Read the CNTVCT cpu system register which provides a consistent value of
 * the virtual system counter across the system. */
static inline u64 rdtsc_ordered(void) {
	u64 cntvct;
	asm volatile ("mrs %0, cntvct_el0; " : "=r"(cntvct));
	return cntvct;
}
#else
#error "Unsupported architecture"
#endif

#define KTIME_MULTIPLIER_SHIFT		32
#define JIFFIES_MULTIPLIER_SHIFT	48

#define EPOCH_DIFF_SECS 11644473600ULL
#define EPOCH_DIFF_100NS (EPOCH_DIFF_SECS * 10000000ULL)

static u64 tsc_base;
static u64 system_boot_ktime;
static struct timespec64 system_boot_ts;
static u64 ktime_multiplier;
static u64 jiffies_multiplier;

unsigned int __read_mostly tsc_khz;
EXPORT_SYMBOL(tsc_khz);

unsigned long loops_per_jiffy;
EXPORT_SYMBOL(loops_per_jiffy);

/* We don't expect to have a lot of timers since our drivers are isolated,
 * so use exactly one hash bucket. */
static HLIST_HEAD(timer_queue);
static spinlock_t timer_lock;
static atomic64_t global_due_time = { ~0ULL }; /* ktime (in ns) */
static struct hrtimer_clock_base hrtimer_base;

#define KHZ     1000
static unsigned long __init get_loops_per_jiffy(void)
{
        u64 lpj = (u64)tsc_khz * KHZ;

        do_div(lpj, HZ);
        return lpj;
}

void __init init_timer(void)
{
	tsc_base = rdtsc_ordered();
	u64 system_time = ntos_get_system_time();
	/* We are living in the past? */
	BUG_ON(system_time < EPOCH_DIFF_100NS);
	system_boot_ktime = (system_time - EPOCH_DIFF_100NS) * 100;
	system_boot_ts = ktime_to_timespec64(system_boot_ktime);
	/* ntos_get_tsc_scale_factor() returns the value by which TSC increments in
	 * one microsecond. Therefore, to convert a TSC value into time in ns (10^-9s),
	 * we divide the TSC value by (tsc_freq_mhz / 1000). However, in order to
	 * reduce the cost of doing an integer division in the hot path, we precompute
	 * the multiplier with which we can turn the division into a multiplication
	 * followed by a shift. */
	u64 tsc_freq_mhz = ntos_get_tsc_frequency_in_mhz();
	tsc_khz = tsc_freq_mhz * 1000;
	ktime_multiplier = div64_ul(1000ULL << KTIME_MULTIPLIER_SHIFT, tsc_freq_mhz);
	/* HZ is the frequency of the timer tick. Since we are a tickless system,
	 * there is actually no periodic timer interrupt. The number of timer ticks,
	 * or jiffies, is used purely as a reference for time-keeping. */
	jiffies_multiplier = div64_ul(1ULL << JIFFIES_MULTIPLIER_SHIFT,
				      tsc_freq_mhz * (1000000ULL / HZ));
	loops_per_jiffy = get_loops_per_jiffy();
}

u64 get_jiffies_64(void)
{
	u64 tsc = rdtsc_ordered() - tsc_base;
	return mul_u64_u64_shr(tsc, jiffies_multiplier, JIFFIES_MULTIPLIER_SHIFT);
}
EXPORT_SYMBOL(get_jiffies_64);

ktime_t ktime_get(void)
{
	/* ktime is in nanoseconds */
	u64 tsc = rdtsc_ordered() - tsc_base;
	return mul_u64_u64_shr(tsc, ktime_multiplier, KTIME_MULTIPLIER_SHIFT);
}
EXPORT_SYMBOL_GPL(ktime_get);

ktime_t ktime_get_with_offset(enum tk_offsets offs)
{
	ktime_t now = ktime_get();
	switch (offs) {
	case TK_OFFS_BOOT:
		return now;
	case TK_OFFS_REAL:
	case TK_OFFS_TAI: /* We really don't care about leap seconds */
		return now + system_boot_ktime;
	default:
		BUG_ON(1);
	}
}
EXPORT_SYMBOL_GPL(ktime_get_with_offset);

/**
 * ktime_get_real_ts64 - Returns the time of day in a timespec64.
 * @ts:		pointer to the timespec to be set
 *
 * Returns the time of day in a timespec64 (WARN if suspended).
 */
void ktime_get_real_ts64(struct timespec64 *ts)
{
	*ts = system_boot_ts;
	timespec64_add_ns(ts, ktime_get());
}
EXPORT_SYMBOL(ktime_get_real_ts64);

/**
 * ktime_get_ts64 - get the monotonic clock in timespec64 format
 * @ts:		pointer to timespec variable
 *
 * The function calculates the monotonic clock from the realtime
 * clock and the wall_to_monotonic offset and stores the result
 * in normalized timespec64 format in the variable pointed to by @ts.
 */
void ktime_get_ts64(struct timespec64 *ts)
{
	*ts = ktime_to_timespec64(ktime_get());
}
EXPORT_SYMBOL_GPL(ktime_get_ts64);

/**
 * ktime_get_clock_ts64 - Returns time of a clock in a timespec
 * @id:		POSIX clock ID of the clock to read
 * @ts:		Pointer to the timespec64 to be set
 *
 * The timestamp is invalidated (@ts->sec is set to -1) if the
 * clock @id is not available.
 */
void ktime_get_clock_ts64(clockid_t id, struct timespec64 *ts)
{
	/* Invalidate time stamp */
	ts->tv_sec = -1;
	ts->tv_nsec = 0;

	switch (id) {
	case CLOCK_REALTIME:
		ktime_get_real_ts64(ts);
		return;
	case CLOCK_MONOTONIC:
		ktime_get_ts64(ts);
		return;
	default:
		WARN_ON_ONCE(1);
	}
}
EXPORT_SYMBOL_GPL(ktime_get_clock_ts64);

time64_t ktime_get_real_seconds(void)
{
	struct timespec64 ts;
	ktime_get_real_ts64(&ts);
	return ts.tv_sec;
}
EXPORT_SYMBOL_GPL(ktime_get_real_seconds);

/**
 * ktime_get_raw - Returns the raw monotonic time in ktime_t format
 */
ktime_t ktime_get_raw(void)
{
        return ktime_get();
}
EXPORT_SYMBOL_GPL(ktime_get_raw);

u64 notrace ktime_get_mono_fast_ns(void)
{
        return ktime_get();
}
EXPORT_SYMBOL_GPL(ktime_get_mono_fast_ns);

u64 notrace ktime_get_raw_fast_ns(void)
{
        return ktime_get();
}
EXPORT_SYMBOL_GPL(ktime_get_raw_fast_ns);

/*
 * Add two ktime values and do a safety check for overflow:
 */
ktime_t ktime_add_safe(const ktime_t lhs, const ktime_t rhs)
{
	ktime_t res = ktime_add_unsafe(lhs, rhs);

	/*
	 * We use KTIME_SEC_MAX here, the maximum timeout which we can
	 * return to user space in a timespec:
	 */
	if (res < 0 || res < lhs || res < rhs)
		res = ktime_set(KTIME_SEC_MAX, 0);

	return res;
}
EXPORT_SYMBOL_GPL(ktime_add_safe);

/*
 * Scheduler clock - returns current time in nanosec units.
 */
notrace unsigned long long sched_clock(void)
{
        return ktime_get();
}
EXPORT_SYMBOL_GPL(sched_clock);

/**
 * timer_init_key - initialize a timer
 * @timer: the timer to be initialized
 * @func: timer callback function
 * @flags: timer flags
 * @name: name of the timer
 * @key: lockdep class key of the fake lock used for tracking timer
 *       sync lock dependencies
 *
 * timer_init_key() must be done to a timer prior to calling *any* of the
 * other timer functions.
 */
void timer_init_key(struct timer_list *timer,
		    void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key)
{
	timer->entry.pprev = NULL;
	timer->function = func;
	if (WARN_ON_ONCE(flags & ~TIMER_INIT_FLAGS))
		flags &= TIMER_INIT_FLAGS;
	timer->flags = flags;
}
EXPORT_SYMBOL(timer_init_key);

static inline void remove_hrtimer(struct hrtimer *timer)
{
	timerqueue_del(&timer->base->active, &timer->node);
	timerqueue_init(&timer->node);
}

static inline u64 ktime_diff_to_100ns(ktime_t t, ktime_t b)
{
	if (t <= b) {
		return 0;
	}
	u64 d = t - b;
	do_div(d, 100);
	return d;
}

static void MS_ABI REALIGN_STACK timer_callback(void *ctx)
{
	ktime_t now = ktime_get();
	atomic64_set(&global_due_time, ~0ULL);
	ktime_t new_due_time = ~0ULL;
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	struct timer_list *t;
	struct hlist_node *n;
	hlist_for_each_entry_safe(t, n, &timer_queue, entry) {
		u64 expire_time = jiffies_to_nsecs(t->expires);
		bool expired = now >= expire_time;
		if (expired) {
			hlist_del(&t->entry);
			INIT_HLIST_NODE(&t->entry);
			if (t->function) {
				t->function(t);
			}
		} else if (expire_time < new_due_time) {
			new_due_time = expire_time;
		}
	}
	struct timerqueue_node *qn;
	while ((qn = timerqueue_getnext(&hrtimer_base.active)) != NULL) {
		if (qn->expires > now)
			break;

		struct hrtimer *ht = container_of(qn, struct hrtimer, node);
		remove_hrtimer(ht);
		ht->state = true;
		enum hrtimer_restart ret = ht->function(ht);
		ht->state = false;
		if (ret == HRTIMER_RESTART) {
			timerqueue_add(&ht->base->active, &ht->node);
		}
	}
	if ((qn = timerqueue_getnext(&hrtimer_base.active)) != NULL) {
		struct hrtimer *ht = container_of(qn, struct hrtimer, node);
		if (ht->node.expires < new_due_time) {
			new_due_time = ht->node.expires;
		}
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	if (new_due_time != ~0ULL) {
		atomic64_set(&global_due_time, new_due_time);
		ntos_set_global_timer(ktime_diff_to_100ns(new_due_time, now),
				      timer_callback, NULL);
	}
}

/**
 * add_timer - Start a timer
 * @timer:	The timer to be started
 *
 * Start @timer to expire at @timer->expires in the future. @timer->expires
 * is the absolute expiry time measured in 'jiffies'. When the timer expires
 * timer->function(timer) will be invoked from soft interrupt context.
 *
 * The @timer->expires and @timer->function fields must be set prior
 * to calling this function.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded.
 *
 * If @timer->expires is already in the past @timer will be queued to
 * expire at the next timer tick.
 *
 * This can only operate on an inactive timer. Attempts to invoke this on
 * an active timer are rejected with a warning.
 */
void add_timer(struct timer_list *timer)
{
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	if (!timer_pending(timer)) {
		hlist_add_head(&timer->entry, &timer_queue);
	}
	u64 due_time = ~0ULL;
	struct timer_list *t;
	hlist_for_each_entry(t, &timer_queue, entry) {
		u64 expire = jiffies_to_nsecs(t->expires);
		if (expire < due_time) {
			due_time = expire;
		}
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	if (due_time < atomic64_read(&global_due_time)) {
		atomic64_set(&global_due_time, due_time);
		ntos_set_global_timer(ktime_diff_to_100ns(due_time, ktime_get()),
				      timer_callback, NULL);
	}
}
EXPORT_SYMBOL(add_timer);

/**
 * add_timer_on - Start a timer on a particular CPU
 * @timer:	The timer to be started
 * @cpu:	The CPU to start it on (ignored)
 *
 * Exactly the same as add_timer(). See add_timer() for further details.
 */
void add_timer_on(struct timer_list *timer, int cpu)
{
	add_timer(timer);
}
EXPORT_SYMBOL_GPL(add_timer_on);

/**
 * add_timer_global() - Start a timer without TIMER_PINNED flag set
 * @timer:      The timer to be started
 *
 * Same as add_timer(). We ignore the TIMER_PINNED flag.
 *
 * See add_timer() for further details.
 */
void add_timer_global(struct timer_list *timer)
{
        if (WARN_ON_ONCE(timer_pending(timer)))
                return;
        add_timer(timer);
}
EXPORT_SYMBOL(add_timer_global);

/**
 * mod_timer - Modify a timer's timeout
 * @timer:	The timer to be modified
 * @expires:	New absolute timeout in jiffies
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     timer_delete(timer); timer->expires = expires; add_timer(timer);
 *
 * mod_timer() is more efficient than the above open coded sequence. In
 * case that the timer is inactive, the timer_delete() part is a NOP. The
 * timer is in any case activated with the new expiry time @expires.
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded. In this case the return value is 0 and meaningless.
 *
 * Return:
 * * %0 - The timer was inactive and started or was in shutdown
 *	  state and the operation was discarded
 * * %1 - The timer was active and requeued to expire at @expires or
 *	  the timer was active and not modified because @expires did
 *	  not change the effective expiry time
 */
int mod_timer(struct timer_list *timer, unsigned long expires)
{
	bool pending;
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	pending = timer_pending(timer);
	timer->expires = expires;
	u64 expire_ktime = jiffies_to_nsecs(expires);
	spin_unlock_irqrestore(&timer_lock, flags);
	if (pending) {
		if (expire_ktime < atomic64_read(&global_due_time)) {
			atomic64_set(&global_due_time, expire_ktime);
			ntos_set_global_timer(ktime_diff_to_100ns(expire_ktime, ktime_get()),
					      timer_callback, NULL);
		}
	} else {
		add_timer(timer);
	}
	return pending;
}
EXPORT_SYMBOL(mod_timer);

/**
 * __timer_delete - Internal function: Deactivate a timer
 * @timer:	The timer to be deactivated
 * @shutdown:	If true, this indicates that the timer is about to be
 *		shutdown permanently.
 *
 * If @shutdown is true then @timer->function is set to NULL under the
 * timer base lock which prevents further rearming of the time. In that
 * case any attempt to rearm @timer after this function returns will be
 * silently ignored.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending and deactivated
 */
static int __timer_delete(struct timer_list *timer, bool shutdown)
{
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	bool pending;
	pending = timer_pending(timer);
	if (pending) {
		hlist_del(&timer->entry);
		INIT_HLIST_NODE(&timer->entry);
	}
	if (shutdown) {
		timer->function = NULL;
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	return pending;
}

/**
 * timer_delete - Deactivate a timer
 * @timer:	The timer to be deactivated
 *
 * The function only deactivates a pending timer, but contrary to
 * timer_delete_sync() it does not take into account whether the timer's
 * callback function is concurrently executed on a different CPU or not.
 * It neither prevents rearming of the timer.  If @timer can be rearmed
 * concurrently then the return value of this function is meaningless.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending and deactivated
 */
int timer_delete(struct timer_list *timer)
{
	return __timer_delete(timer, false);
}
EXPORT_SYMBOL(timer_delete);

/**
 * timer_shutdown - Deactivate a timer and prevent rearming
 * @timer:	The timer to be deactivated
 *
 * The function does not wait for an eventually running timer callback on a
 * different CPU but it prevents rearming of the timer. Any attempt to arm
 * @timer after this function returns will be silently ignored.
 *
 * This function is useful for teardown code and should only be used when
 * timer_shutdown_sync() cannot be invoked due to locking or context constraints.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending
 */
int timer_shutdown(struct timer_list *timer)
{
	return __timer_delete(timer, true);
}
EXPORT_SYMBOL_GPL(timer_shutdown);

/**
 * __timer_delete_sync - Internal function: Deactivate a timer and wait
 *			 for the handler to finish.
 * @timer:	The timer to be deactivated
 * @shutdown:	If true, @timer->function will be set to NULL under the
 *		timer base lock which prevents rearming of @timer
 *
 * If @shutdown is not set the timer can be rearmed later. If the timer can
 * be rearmed concurrently, i.e. after dropping the base lock then the
 * return value is meaningless.
 *
 * If @shutdown is set then @timer->function is set to NULL under timer
 * base lock which prevents rearming of the timer. Any attempt to rearm
 * a shutdown timer is silently ignored.
 *
 * If the timer should be reused after shutdown it has to be initialized
 * again.
 *
 * Return:
 * * %0	- The timer was not pending
 * * %1	- The timer was pending and deactivated
 */
static int __timer_delete_sync(struct timer_list *timer, bool shutdown)
{
	return __timer_delete(timer, shutdown);
}

/**
 * timer_delete_sync - Deactivate a timer and wait for the handler to finish.
 * @timer:	The timer to be deactivated
 *
 * Synchronization rules: Callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts unless the timer is an irqsafe one. The caller must
 * not hold locks which would prevent completion of the timer's callback
 * function. The timer's handler must not call add_timer_on(). Upon exit
 * the timer is not queued and the handler is not running on any CPU.
 *
 * For !irqsafe timers, the caller must not hold locks that are held in
 * interrupt context. Even if the lock has nothing to do with the timer in
 * question.  Here's why::
 *
 *    CPU0                             CPU1
 *    ----                             ----
 *                                     <SOFTIRQ>
 *                                       call_timer_fn();
 *                                       base->running_timer = mytimer;
 *    spin_lock_irq(somelock);
 *                                     <IRQ>
 *                                        spin_lock(somelock);
 *    timer_delete_sync(mytimer);
 *    while (base->running_timer == mytimer);
 *
 * Now timer_delete_sync() will never return and never release somelock.
 * The interrupt on the other CPU is waiting to grab somelock but it has
 * interrupted the softirq that CPU0 is waiting to finish.
 *
 * This function cannot guarantee that the timer is not rearmed again by
 * some concurrent or preempting code, right after it dropped the base
 * lock. If there is the possibility of a concurrent rearm then the return
 * value of the function is meaningless.
 *
 * If such a guarantee is needed, e.g. for teardown situations then use
 * timer_shutdown_sync() instead.
 *
 * Return:
 * * %0	- The timer was not pending
 * * %1	- The timer was pending and deactivated
 */
int timer_delete_sync(struct timer_list *timer)
{
	return __timer_delete_sync(timer, false);
}
EXPORT_SYMBOL(timer_delete_sync);

/**
 * timer_shutdown_sync - Shutdown a timer and prevent rearming
 * @timer: The timer to be shutdown
 *
 * When the function returns it is guaranteed that:
 *   - @timer is not queued
 *   - The callback function of @timer is not running
 *   - @timer cannot be enqueued again. Any attempt to rearm
 *     @timer is silently ignored.
 *
 * See timer_delete_sync() for synchronization rules.
 *
 * This function is useful for final teardown of an infrastructure where
 * the timer is subject to a circular dependency problem.
 *
 * A common pattern for this is a timer and a workqueue where the timer can
 * schedule work and work can arm the timer. On shutdown the workqueue must
 * be destroyed and the timer must be prevented from rearming. Unless the
 * code has conditionals like 'if (mything->in_shutdown)' to prevent that
 * there is no way to get this correct with timer_delete_sync().
 *
 * timer_shutdown_sync() is solving the problem. The correct ordering of
 * calls in this case is:
 *
 *	timer_shutdown_sync(&mything->timer);
 *	workqueue_destroy(&mything->workqueue);
 *
 * After this 'mything' can be safely freed.
 *
 * This obviously implies that the timer is not required to be functional
 * for the rest of the shutdown operation.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending
 */
int timer_shutdown_sync(struct timer_list *timer)
{
	return __timer_delete_sync(timer, true);
}
EXPORT_SYMBOL_GPL(timer_shutdown_sync);

/**
 * hrtimer_setup - initialize a timer to the given clock
 * @timer:	the timer to be initialized
 * @function:	the callback function
 * @clock_id:	the clock to be used
 * @mode:       The modes which are relevant for initialization:
 *              HRTIMER_MODE_ABS, HRTIMER_MODE_REL, HRTIMER_MODE_ABS_SOFT,
 *              HRTIMER_MODE_REL_SOFT
 *
 *              The PINNED variants of the above can be handed in,
 *              but the PINNED bit is ignored as pinning happens
 *              when the hrtimer is started
 */
void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*function)(struct hrtimer *),
		   clockid_t clock_id, enum hrtimer_mode mode)
{
	memset(timer, 0, sizeof(struct hrtimer));
	timer->is_rel = !!(mode & HRTIMER_MODE_REL);
	timer->is_soft = true;
	timer->function = function;
	timer->base = &hrtimer_base;
	timerqueue_init(&timer->node);
}
EXPORT_SYMBOL_GPL(hrtimer_setup);

/**
 * hrtimer_start_range_ns - (re)start an hrtimer
 * @timer:	the timer to be added
 * @time:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	timer mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL), and pinned (HRTIMER_MODE_PINNED);
 *		softirq based mode is considered for debug purpose only!
 */
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t time,
			    u64 delta_ns, const enum hrtimer_mode mode)
{
	if (mode & HRTIMER_MODE_REL)
		time = ktime_add_safe(time, ktime_get());
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	if (timerqueue_node_queued(&timer->node)) {
		remove_hrtimer(timer);
	}
	hrtimer_set_expires(timer, time);
	timerqueue_add(&timer->base->active, &timer->node);
	spin_unlock_irqrestore(&timer_lock, flags);
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns);

/**
 * hrtimer_cancel - cancel a timer and wait for the handler to finish.
 * @timer:	the timer to be cancelled
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 */
int hrtimer_cancel(struct hrtimer *timer)
{
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	bool active = timerqueue_node_queued(&timer->node);
	if (active) {
		remove_hrtimer(timer);
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	return active;
}
EXPORT_SYMBOL_GPL(hrtimer_cancel);

/*
 * A timer is active, when it is enqueued into the rbtree or the
 * callback function is running.
 *
 * It is important for this function to not return a false negative.
 */
bool hrtimer_active(const struct hrtimer *timer)
{
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	bool active = timerqueue_node_queued((void *)&timer->node);
	spin_unlock_irqrestore(&timer_lock, flags);
	return active || timer->state;
}
EXPORT_SYMBOL_GPL(hrtimer_active);

ktime_t hrtimer_cb_get_time(const struct hrtimer *timer)
{
	return ktime_get();
}
EXPORT_SYMBOL_GPL(hrtimer_cb_get_time);

/**
 * hrtimer_forward() - forward the timer expiry
 * @timer:	hrtimer to forward
 * @now:	forward past this time
 * @interval:	the interval to forward
 *
 * Forward the timer expiry so it will expire in the future.
 *
 * .. note::
 *  This only updates the timer expiry value and does not requeue the timer.
 *
 * There is also a variant of the function hrtimer_forward_now().
 *
 * Context: Can be safely called from the callback function of @timer. If called
 *          from other contexts @timer must neither be enqueued nor running the
 *          callback and the caller needs to take care of serialization.
 *
 * Return: The number of overruns are returned.
 */
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	ktime_t delta = ktime_sub(now, hrtimer_get_expires(timer));
	if (delta < 0)
		return 0;

	hrtimer_add_expires(timer, interval);
	return 1;
}
EXPORT_SYMBOL_GPL(hrtimer_forward);

/**
 * hrtimer_try_to_cancel - try to deactivate a timer
 * @timer:	hrtimer to stop
 *
 * Returns:
 *
 *  *  0 when the timer was not active
 *  *  1 when the timer was active
 *  * -1 when the timer is currently executing the callback function and
 *    cannot be stopped
 */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	unsigned long flags;
	spin_lock_irqsave(&timer_lock, flags);
	bool active = timerqueue_node_queued(&timer->node);
	bool running = timer->state;
	if (active) {
		remove_hrtimer(timer);
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	return running ? -1 : active;
}
EXPORT_SYMBOL_GPL(hrtimer_try_to_cancel);

/*
 * Sleep related functions:
 */
static enum hrtimer_restart hrtimer_wakeup(struct hrtimer *timer)
{
        struct hrtimer_sleeper *t =
                container_of(timer, struct hrtimer_sleeper, timer);
        struct task_struct *task = t->task;

        t->task = NULL;
        if (task)
                wake_up_process(task);

        return HRTIMER_NORESTART;
}

static void __hrtimer_setup_sleeper(struct hrtimer_sleeper *sl,
                                    clockid_t clock_id, enum hrtimer_mode mode)
{
        hrtimer_setup(&sl->timer, hrtimer_wakeup, clock_id, mode);
        sl->task = current;
}

/**
 * hrtimer_setup_sleeper_on_stack - initialize a sleeper in stack memory
 * @sl:         sleeper to be initialized
 * @clock_id:   the clock to be used
 * @mode:       timer mode abs/rel
 */
void hrtimer_setup_sleeper_on_stack(struct hrtimer_sleeper *sl,
                                    clockid_t clock_id, enum hrtimer_mode mode)
{
        __hrtimer_setup_sleeper(sl, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_setup_sleeper_on_stack);

/**
 * hrtimer_sleeper_start_expires - Start a hrtimer sleeper timer
 * @sl:         sleeper to be started
 * @mode:       timer mode abs/rel
 *
 * Wrapper around hrtimer_start_expires() for hrtimer_sleeper based timers
 * to allow PREEMPT_RT to tweak the delivery mode (soft/hardirq context)
 */
void hrtimer_sleeper_start_expires(struct hrtimer_sleeper *sl,
                                   enum hrtimer_mode mode)
{
        hrtimer_start_expires(&sl->timer, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_sleeper_start_expires);

/**
 * usleep_range_state - Sleep for an approximate time in a given state
 * @min:	Minimum time in usecs to sleep
 * @max:	Maximum time in usecs to sleep
 * @state:	State of the current task that will be while sleeping
 *
 * The kernel implemention uses high-resolution timer which we won't have yet,
 * so we replace it with a lower resolution one.
 */
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
	unsigned long timeout = usecs_to_jiffies(min);
	while (timeout)
		timeout = schedule_timeout_uninterruptible(timeout);
}
EXPORT_SYMBOL(usleep_range_state);

void __udelay(unsigned long usecs)
{
	ktime_t start = ktime_get();
	while (ktime_get() - start < usecs * 1000) ;
}
EXPORT_SYMBOL(__udelay);

void __const_udelay(unsigned long xloops)
{
	__udelay(((u64)xloops * USEC_PER_SEC) >> 32);
}
EXPORT_SYMBOL(__const_udelay);

void __ndelay(unsigned long nsecs)
{
	ktime_t start = ktime_get();
	while (ktime_get() - start < nsecs) ;
}
EXPORT_SYMBOL(__ndelay);
