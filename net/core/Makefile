# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux networking core.
#

obj-$(CONFIG_NET_BASE) := skbuff.o flow_dissector.o \
	 gen_estimator.o gen_stats.o
obj-$(CONFIG_NET) += sock.o datagram.o stream.o scm.o \
	 net_namespace.o secure_seq.o

obj-$(CONFIG_SYSCTL) += sysctl_net_core.o

obj-$(CONFIG_NET_BASE) += dst.o dev.o dev_api.o dev_addr_lists.o dev_ioctl.o flow_offload.o \
			  gso.o link_watch.o rtnetlink.o gro.o
obj-$(CONFIG_NET) += netevent.o \
			neighbour.o utils.o \
			sock_diag.o tso.o sock_reuseport.o \
			fib_notifier.o xdp.o \
			netdev-genl.o netdev-genl-gen.o

obj-$(CONFIG_NETDEV_ADDR_LIST_TEST) += dev_addr_lists_test.o

obj-$(CONFIG_NET_BASE) += net-sysfs.o
obj-$(CONFIG_NET_BASE) += hotdata.o
obj-$(CONFIG_NET) += netdev_config.o
obj-$(CONFIG_NET) += netdev_rx_queue.o
obj-$(CONFIG_NET) += netdev_queues.o
obj-$(CONFIG_PAGE_POOL) += page_pool.o page_pool_user.o
obj-$(CONFIG_PROC_FS) += net-procfs.o
obj-$(CONFIG_NET_PKTGEN) += pktgen.o
obj-$(CONFIG_NETPOLL) += netpoll.o
obj-$(CONFIG_FIB_RULES) += fib_rules.o
obj-$(CONFIG_TRACEPOINTS) += net-traces.o
obj-$(CONFIG_NET_DROP_MONITOR) += drop_monitor.o
obj-$(CONFIG_NET_IEEE8021Q_HELPERS) += ieee8021q_helpers.o
obj-$(CONFIG_NET_SELFTESTS) += selftests.o
obj-$(CONFIG_NETWORK_PHY_TIMESTAMPING) += timestamping.o
obj-$(CONFIG_NET_PTP_CLASSIFY) += ptp_classifier.o
obj-$(CONFIG_CGROUP_NET_PRIO) += netprio_cgroup.o
obj-$(CONFIG_CGROUP_NET_CLASSID) += netclassid_cgroup.o
obj-$(CONFIG_LWTUNNEL) += lwtunnel.o
obj-$(CONFIG_LWTUNNEL_BPF) += lwt_bpf.o
obj-$(CONFIG_DST_CACHE) += dst_cache.o
obj-$(CONFIG_HWBM) += hwbm.o
obj-$(CONFIG_GRO_CELLS) += gro_cells.o
obj-$(CONFIG_FAILOVER) += failover.o
obj-$(CONFIG_NET_SOCK_MSG) += skmsg.o
obj-$(CONFIG_BPF_SYSCALL) += sock_map.o
obj-$(CONFIG_BPF_SYSCALL) += bpf_sk_storage.o
obj-$(CONFIG_OF)	+= of_net.o
obj-$(CONFIG_NET_TEST) += net_test.o
obj-$(CONFIG_NET_DEVMEM) += devmem.o
obj-$(CONFIG_DEBUG_NET) += lock_debug.o
obj-$(CONFIG_FAIL_SKB_REALLOC) += skb_fault_injection.o
