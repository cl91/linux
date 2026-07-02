#include <linux/relay.h>

struct rchan *relay_open(const char *base_filename,
			 struct dentry *parent,
			 size_t subbuf_size,
			 size_t n_subbufs,
			 const struct rchan_callbacks *cb,
			 void *private_data)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(relay_open);

int relay_buf_full(struct rchan_buf *buf)
{
	size_t ready = buf->subbufs_produced - buf->subbufs_consumed;
	return (ready >= buf->chan->n_subbufs) ? 1 : 0;
}
EXPORT_SYMBOL_GPL(relay_buf_full);

void relay_close(struct rchan *chan)
{
}
EXPORT_SYMBOL_GPL(relay_close);

void relay_flush(struct rchan *chan)
{
}
EXPORT_SYMBOL_GPL(relay_flush);

size_t relay_switch_subbuf(struct rchan_buf *buf, size_t length)
{
	return -1;
}
EXPORT_SYMBOL_GPL(relay_switch_subbuf);
