#pragma once

#include <linux/init.h>
#include <linux/cred.h>
#include <linux/signal.h>

extern void __init init_panic(void);
extern void __init init_workqueue(void);
extern void __init init_timer(void);
extern void __init init_mem(void);
extern void __init init_file_table(void);
extern void __init init_inode(void);
extern void __init init_lnxdrv_core(void *nt_driver_object_handle);

extern struct cred dummy_cred;
extern struct signal_struct dummy_signal;

/* For now we assign a dummy, shared struct signal_struct. At this point we effectively
 * have only one process as far as credential goes. The DRM subsystem records the pid
 * associated with the drm_file to check if requesting process is the drm master. In
 * the future when we have implemented the full NT security model we will pass the NT
 * security context (LUID, PACCESS_TOKEN) to the Linux side and link it to the open
 * file object, and use that as the unique identification of the requestor. */
#define KERNEL_THREAD_ENTER(_kevent)		\
	struct task_struct current_task = {	\
		.thread_info.kevent = _kevent,	\
		.cred = &dummy_cred,		\
		.signal = &dummy_signal,	\
		.usage = REFCOUNT_INIT(1),	\
	};					\
	current = &current_task			\

#define KERNEL_THREAD_EXIT			\
	current = NULL

extern int get_file_mode(void *nt_file_object);
extern struct super_block pseudo_sb;
extern struct vfsmount pseudo_vfsmount;
