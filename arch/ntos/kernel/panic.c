#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/panic_notifier.h>
#include <linux/kdebug.h>
#include <host_ops.h>
#include <init.h>

static unsigned long tainted_mask;
unsigned long panic_on_taint;

void panic(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	ntos_panic();
}
EXPORT_SYMBOL(panic);

ATOMIC_NOTIFIER_HEAD(panic_notifier_list);
EXPORT_SYMBOL(panic_notifier_list);

static void REALIGN_STACK lnx_bugcheck_callback(const char *msg)
{
	struct die_args args = {
		.str	= msg
	};
	atomic_notifier_call_chain(&panic_notifier_list, DIE_UNUSED, &args);
}

void __init init_panic(void)
{
	ntos_register_bugcheck_callback(lnx_bugcheck_callback);
}

#define TAINT_FLAG(taint, _c_true, _c_false)                            \
        [ TAINT_##taint ] = {                                           \
                .c_true = _c_true, .c_false = _c_false,                 \
                .desc = #taint,                                         \
        }

const struct taint_flag taint_flags[TAINT_FLAGS_COUNT] = {
        TAINT_FLAG(PROPRIETARY_MODULE,          'P', 'G'),
        TAINT_FLAG(FORCED_MODULE,               'F', ' '),
        TAINT_FLAG(CPU_OUT_OF_SPEC,             'S', ' '),
        TAINT_FLAG(FORCED_RMMOD,                'R', ' '),
        TAINT_FLAG(MACHINE_CHECK,               'M', ' '),
        TAINT_FLAG(BAD_PAGE,                    'B', ' '),
        TAINT_FLAG(USER,                        'U', ' '),
        TAINT_FLAG(DIE,                         'D', ' '),
        TAINT_FLAG(OVERRIDDEN_ACPI_TABLE,       'A', ' '),
        TAINT_FLAG(WARN,                        'W', ' '),
        TAINT_FLAG(CRAP,                        'C', ' '),
        TAINT_FLAG(FIRMWARE_WORKAROUND,         'I', ' '),
        TAINT_FLAG(OOT_MODULE,                  'O', ' '),
        TAINT_FLAG(UNSIGNED_MODULE,             'E', ' '),
        TAINT_FLAG(SOFTLOCKUP,                  'L', ' '),
        TAINT_FLAG(LIVEPATCH,                   'K', ' '),
        TAINT_FLAG(AUX,                         'X', ' '),
        TAINT_FLAG(RANDSTRUCT,                  'T', ' '),
        TAINT_FLAG(TEST,                        'N', ' '),
        TAINT_FLAG(FWCTL,                       'J', ' '),
};

int test_taint(unsigned flag)
{
        return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

/**
 * add_taint: add a taint flag if not already set.
 * @flag: one of the TAINT_* constants.
 * @lockdep_ok: whether lock debugging is still OK.
 *
 * If something bad has gone wrong, you'll want @lockdebug_ok = false, but for
 * some notewortht-but-not-corrupting cases, it can be set to true.
 */
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
        set_bit(flag, &tainted_mask);

        if (tainted_mask & panic_on_taint) {
                panic_on_taint = 0;
                panic("panic_on_taint set ...");
        }
}
EXPORT_SYMBOL(add_taint);
