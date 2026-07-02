/*
 * A shrinker responds to the message from the NTOS server to release cache
 * memory back to the system when system is low on memory. For now we simply
 * have a dummy implementation.
 */
#include <linux/slab.h>
#include <linux/shrinker.h>
#include <linux/oom.h>

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	return kzalloc(sizeof(struct shrinker), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(shrinker_alloc);

void shrinker_register(struct shrinker *shrinker)
{
	/* TODO */
}
EXPORT_SYMBOL_GPL(shrinker_register);

void shrinker_free(struct shrinker *shrinker)
{
	kfree(shrinker);
}
EXPORT_SYMBOL_GPL(shrinker_free);

static BLOCKING_NOTIFIER_HEAD(oom_notify_list);

int register_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_oom_notifier);

int unregister_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_oom_notifier);
