#include <linux/ratelimit.h>

int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	return 1;
}

EXPORT_SYMBOL(___ratelimit);
