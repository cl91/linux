#include <linux/printk.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/ethtool.h>
#include <linux/nstree.h>
#include <net/flow_offload.h>
#include <net/pkt_sched.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/sock.h>
#include <init.h>
#include "../drivers/core.h"

/* net_namespace.c */
struct net init_net;
EXPORT_SYMBOL(init_net);
DECLARE_RWSEM(pernet_ops_rwsem);
LIST_HEAD(net_namespace_list);
EXPORT_SYMBOL_GPL(net_namespace_list);

DEFINE_STATIC_KEY_FALSE(bpf_stats_enabled_key);
EXPORT_SYMBOL(bpf_stats_enabled_key);

int register_pernet_subsys(struct pernet_operations *ops)
{
	BUG_ON(!ops);
	BUG_ON(!ops->init);
	ops->init(&init_net);
	return 0;
}
EXPORT_SYMBOL_GPL(register_pernet_subsys);

static void init_net_ns(struct net *net)
{
	list_add_tail(&net->list, &net_namespace_list);
	idr_init(&net->netns_ids);
	spin_lock_init(&net->nsid_lock);
	INIT_LIST_HEAD(&net->ptype_all);
	INIT_LIST_HEAD(&net->ptype_specific);
}

static int __init init_net_core(void)
{
	init_net_ns(&init_net);
	skb_init();
	return 0;
}
fs_initcall(init_net_core);

int net_ratelimit(void)
{
        return 0;
}
EXPORT_SYMBOL(net_ratelimit);

void do_trace_netlink_extack(const char *msg)
{
	/* Do nothing */
}
EXPORT_SYMBOL(do_trace_netlink_extack);

void rtmsg_ifinfo(int type, struct net_device *dev, unsigned int change,
                  gfp_t flags, u32 portid, const struct nlmsghdr *nlh)
{
	/* Do nothing */
}

struct sk_buff *rtmsg_ifinfo_build_skb(int type, struct net_device *dev,
				       unsigned int change,
				       u32 event, gfp_t flags, int *new_nsid,
				       int new_ifindex, u32 portid,
				       const struct nlmsghdr *nlh)
{
	return NULL;
}

void rtmsg_ifinfo_send(struct sk_buff *skb, struct net_device *dev, gfp_t flags,
		       u32 portid, const struct nlmsghdr *nlh)
{
}

void sock_efree(struct sk_buff *skb)
{
	/* TODO. Perhaps do nothing? */
}
EXPORT_SYMBOL(sock_efree);

void sk_free(struct sock *sk)
{
	/* TODO */
}
EXPORT_SYMBOL(sk_free);

void sk_error_report(struct sock *sk)
{
}
EXPORT_SYMBOL(sk_error_report);
