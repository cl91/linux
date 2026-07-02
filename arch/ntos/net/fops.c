#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <host_ops.h>
#include "../drivers/core.h"

struct netdev_rx_ctx {
	struct sk_buff_head q;
	void *head_irp;
};

static bool match(struct device *dev)
{
	return !!to_net_dev_safe(dev);
}

static void copy_read_result(struct sk_buff *skb,
			     void *user_buf,
			     unsigned int buf_len,
			     unsigned int *result_len)
{
	/* Include Ethernet header if needed */
	unsigned char *mac = skb_mac_header_was_set(skb) ? skb_mac_header(skb) : NULL;
	unsigned int mac_len = skb_mac_header_was_set(skb) ? (skb->data - mac) : 0;

	unsigned int total_len = mac_len + skb->len;
	unsigned int copy_len = min(buf_len, total_len);

	/* Copy L2 header first */
	unsigned int offset = 0;
	if (mac_len) {
		unsigned int l2_copy = min(copy_len, mac_len);
		memcpy(user_buf, mac, l2_copy);
		offset += l2_copy;
	}

	/* Copy payload (possibly non-linear) */
	if (offset < copy_len) {
		skb_copy_bits(skb,
			      0, /* offset into skb->data */
			      (char *)user_buf + offset,
			      copy_len - offset);
	}

	*result_len = copy_len;
	kfree_skb(skb);
}

static rx_handler_result_t netdev_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device *dev = skb->dev;
	struct netdev_rx_ctx *ctx = dev->rx_handler_data;

	if (!ctx)
		return RX_HANDLER_PASS;

	/* Take ownership of the skb */
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return RX_HANDLER_CONSUMED;

	/* If there is a pending IRP, complete it immediately */
	if (ctx->head_irp) {
		void *irp = ctx->head_irp;
		ctx->head_irp = ntos_get_irp_driver_context(irp);
		int result_len = 0;
		copy_read_result(skb, ntos_irp_get_request_buffer(irp),
				 ntos_irp_get_request_length(irp), &result_len);
		ntos_complete_irp(irp, 0, result_len);
		return RX_HANDLER_CONSUMED;
	}

	/* Otherwise queue the packet */
	skb_queue_tail(&ctx->q, skb);
	return RX_HANDLER_CONSUMED;
}

static int netdev_dispatch_create(struct device *dev, void *irp, void *file_object)
{
	struct netdev_rx_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		return -ENOMEM;
	}
	struct net_device *netdev = to_net_dev_safe(dev);
	BUG_ON(!netdev);
	rtnl_lock();
	int ret = dev_open(netdev, NULL);
	rtnl_unlock();
	if (ret) {
		kfree(ctx);
		return ret;
	}
	skb_queue_head_init(&ctx->q);
	ret = netdev_rx_handler_register(netdev, netdev_rx_handler, ctx);
	if (ret) {
		rtnl_lock();
		dev_close(netdev);
		rtnl_unlock();
		kfree(ctx);
	}
	return ret;
}

static int netdev_dispatch_read(struct net_device *netdev, void *irp,
				void *buffer, unsigned int buffer_length,
				unsigned long *pfn_db, unsigned int pfn_count,
				unsigned int *result_length)
{
	struct netdev_rx_ctx *ctx = netdev->rx_handler_data;
	if (!ctx || !buffer || !result_length)
		return -EINVAL;

	struct sk_buff *skb = skb_dequeue(&ctx->q);
	if (!skb) {
		/* No packet available at the moment. We will queue the IRP and
		 * return pending status. */
		local_bh_disable();
		ntos_set_irp_driver_context(irp, ctx->head_irp);
		ctx->head_irp = irp;
		local_bh_enable();
		return -EAGAIN;
	}

	copy_read_result(skb, buffer, buffer_length, result_length);
	return 0;
}

struct netdev_tx_completion_ctx {
	void *irp;
	void *buf;
	unsigned int buf_len;
};
static_assert(sizeof(struct netdev_tx_completion_ctx) <=
	      sizeof_field(struct sk_buff, cb),
	      "struct netdev_tx_completion_ctx too big");

static void netdev_tx_completed(struct sk_buff *skb)
{
	struct netdev_tx_completion_ctx *ctx = (void *)(skb->head + LL_RESERVED_SPACE(skb->dev));
	lnxdrv_unregister_user_buffer(ctx->buf, ctx->buf_len);
	ntos_complete_irp(ctx->irp, 0, ctx->buf_len);
}

static int netdev_dispatch_write(struct net_device *netdev, void *irp,
				 void *buffer, unsigned int buffer_length,
				 unsigned long *pfn_db, unsigned int pfn_count,
				 unsigned int *result_length)
{
	if (!netif_running(netdev))
		return -ENODEV;

	if (buffer_length < ETH_HLEN) {
		return -EINVAL;
	}

	int ret = lnxdrv_register_user_buffer(buffer, buffer_length, pfn_count, pfn_db);
	if (ret) {
		return ret;
	}

	/* Allocate skb header only, with our head room. */
	int headroom = LL_RESERVED_SPACE(netdev) + sizeof(struct netdev_tx_completion_ctx);
	struct sk_buff *skb = alloc_skb(headroom + ETH_HLEN, GFP_ATOMIC);
	if (!skb) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	skb_reserve(skb, headroom);
	skb->dev = netdev;
	skb->protocol = htons(ETH_P_802_3);
	/* No queueing discipline */
	skb->priority = 0;

	struct netdev_tx_completion_ctx *ctx = (void *)(skb->head + LL_RESERVED_SPACE(netdev));
	ctx->irp = irp;
	ctx->buf = buffer;
	ctx->buf_len = buffer_length;
	skb->destructor = netdev_tx_completed;

	/* Copy the ethernet header into the skb */
	memcpy(skb_put(skb, ETH_HLEN), buffer, ETH_HLEN);

	/* Attach the rest of data as frags */
	buffer = (char *)buffer + ETH_HLEN;
	buffer_length -= ETH_HLEN;
	for (unsigned long addr = (unsigned long)buffer;
	     addr < (unsigned long)buffer + buffer_length; ) {
		unsigned long page_addr = PAGE_ALIGN_DOWN(addr);
		struct page *page = virt_to_page((void *)page_addr);
		int off = addr - page_addr;
		int size = min(PAGE_SIZE - off, (unsigned long)buffer + buffer_length - addr);
		ret = skb_append_pagefrags(skb, page, off, size, MAX_SKB_FRAGS);
		if (ret)
			goto err;
		addr = page_addr + PAGE_SIZE;
	}

	skb->len = buffer_length + ETH_HLEN;
	skb->data_len = buffer_length;
	skb->truesize += buffer_length;

	ret = dev_queue_xmit(skb);

	if (ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN) {
		*result_length = buffer_length;
		return -EAGAIN;
	}
	/* dev_queue_xmit automatically frees the skbuff in case of error. */
	return -EIO;
err:
	kfree_skb(skb);
	/* Buffer unregistration is done in the skb destructor. */
	return ret;
err_alloc:
	lnxdrv_unregister_user_buffer(buffer, buffer_length);
	return ret;
}

static int netdev_dispatch_read_write(struct device *dev, void *irp, void *file_object,
				      unsigned long long file_offset,
				      void *buffer, unsigned int buffer_length,
				      unsigned long *pfn_db, unsigned int pfn_count,
				      int write, unsigned int *result_length)
{
	struct net_device *netdev = to_net_dev_safe(dev);
	BUG_ON(!netdev);
	if (write) {
		return netdev_dispatch_write(netdev, irp, buffer, buffer_length,
					     pfn_db, pfn_count, result_length);
	} else {
		return netdev_dispatch_read(netdev, irp, buffer, buffer_length,
					    pfn_db, pfn_count, result_length);
	}
}

static int netdev_dispatch_cleanup(struct device *dev, void *irp, void *file_object)
{
	struct net_device *netdev = to_net_dev_safe(dev);
	BUG_ON(!netdev);
	struct netdev_rx_ctx *ctx = netdev->rx_handler_data;
	BUG_ON(!ctx);
	netdev_rx_handler_unregister(netdev);
	struct sk_buff *skb;
	while ((skb = skb_dequeue(&ctx->q)) != NULL)
		kfree_skb(skb);
	if (netif_running(netdev)) {
		rtnl_lock();
		dev_close(netdev);
		rtnl_unlock();
	}
	kfree(ctx);
	return 0;
}

static int __init register_net_dev_type(void)
{
	lnxdrv_dev_types[LnxNetDev] = (struct lnxdrv_dev_type) {
		.match = match,
		.create = netdev_dispatch_create,
		.read_write = netdev_dispatch_read_write,
		.cleanup = netdev_dispatch_cleanup
	};
	return 0;
}
fs_initcall(register_net_dev_type);
