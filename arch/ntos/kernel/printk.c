#include <linux/stdarg.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <host_ops.h>

int oops_in_progress;
EXPORT_SYMBOL(oops_in_progress);

asmlinkage int vprintk(const char *fmt, va_list args)
{
	if (!fmt) {
		return 0;
	}
	char buf[256];
	va_list va;
	va_copy(va, args);
	int n = vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	/* For now we print everything regardless of loglevel. */
	fmt = buf;
	if (*fmt == KERN_SOH_ASCII) {
		fmt++;
		if (*fmt) {
			fmt++;
		}
	}
	ntos_dbgprint(fmt);
	return n;
}
EXPORT_SYMBOL(vprintk);

asmlinkage int vprintk_emit(int facility, int level,
			    const struct dev_printk_info *dev_info,
			    const char *fmt, va_list args)
{
	return vprintk(fmt, args);
}
EXPORT_SYMBOL(vprintk_emit);

asmlinkage __visible int _printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);

	return r;
}
EXPORT_SYMBOL(_printk);

void warn_slowpath_fmt(const char *file, int line, unsigned taint,
                       const char *fmt, ...)
{
	pr_warn("(%s:%d) WARNING\n", file, line);
	va_list args;
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
}
EXPORT_SYMBOL(warn_slowpath_fmt);

int __printk_ratelimit(const char *func)
{
        return 1;
}
EXPORT_SYMBOL(__printk_ratelimit);

void __printk_deferred_enter(void)
{
	/* Do nothing */
}

void __printk_deferred_exit(void)
{
	/* Do nothing */
}
