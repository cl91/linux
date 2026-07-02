#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/netdevice.h>
#include <linux/kmod.h>
#include <host_ops.h>
#include <init.h>
#include "core.h"

/* This is the NT DRIVER_OBJECT for the LNXDRV.SYS driver itself.
 * LNXDRV.SYS is loaded as a dynamic library and there can only be
 * exactly one instance of LNXDRV.SYS loaded into a host process. */
static void *global_driver_object_handle;

void __init init_lnxdrv_core(void *nt_driver_object_handle)
{
	global_driver_object_handle = nt_driver_object_handle;
}

static inline LNX_DEVICE_TYPE dev_to_lnx_devcls(struct device *dev)
{
	if (to_net_dev_safe(dev)) {
		return LnxNetDev;
	}
	return LnxCharDev;
}

static inline bool dev_is_exclusive(struct device *dev)
{
	/* For net_device, we make sure the device object can only be opened
	 * once at a given time. (Think about the RX path of a raw ethernet
	 * frame. If two clients opened the ethernet device, which client
	 * should the RX packet go to?) */
	return !!to_net_dev_safe(dev);
}

struct lnxdrv_dev_ext *lnxdrv_core_create_device(void *driver_handle,
						 struct device *dev,
						 const char *devname)
{
	BUG_ON(!driver_handle);
	void *nt_handle = NULL;
	const char *devname_dup = kstrdup(devname, GFP_KERNEL);
	if (!devname_dup) {
		return ERR_PTR(-ENOMEM);
	}
	int ret = ntos_create_device(&nt_handle, driver_handle,
				     sizeof(struct lnxdrv_dev_ext), devname,
				     dev_to_lnx_devcls(dev), dev_is_exclusive(dev));
	if (ret) {
		kfree(devname_dup);
		return ERR_PTR(ret);
	}
	BUG_ON(!nt_handle);
	struct lnxdrv_dev_ext *dev_ext = ntos_get_device_extension(nt_handle);
	dev_ext->nt_handle = nt_handle;
	dev_ext->device = dev;
	dev_ext->driver_handle = driver_handle;
	dev_ext->name = devname_dup;
	dev->lnxdrv_data = dev_ext;
	return dev_ext;
}

int lnxdrv_core_attach_device(struct lnxdrv_dev_ext *dev_ext, void *pdo)
{
	int ret = ntos_attach_device(dev_ext->nt_handle, pdo, &dev_ext->lower_do_handle);
	if (ret) {
		dev_ext->lower_do_handle = NULL;
		return ret;
	}
	dev_ext->pdo_handle = pdo;
	return 0;
}

void lnxdrv_core_delete_device(struct lnxdrv_dev_ext *dev_ext)
{
	/* struct lnxdrv_dev_ext is part of the NT DEVICE_OBJECT so we don't
	 * free it on the Linux side. */
	BUG_ON(!dev_ext);
	BUG_ON(!dev_ext->nt_handle);
	/* LNXDRV-specific bus data must be freed before calling this routine. */
	BUG_ON(dev_ext->bus_data);
	kfree(dev_ext->name);
	dev_ext->device->lnxdrv_data = NULL;
	ntos_delete_device(dev_ext->nt_handle);
}

/* the strings here must match the enum in include/linux/kobject.h */
static const char *kobject_actions[] = {
	[KOBJ_ADD] =		"add",
	[KOBJ_REMOVE] =		"remove",
	[KOBJ_CHANGE] =		"change",
	[KOBJ_MOVE] =		"move",
	[KOBJ_ONLINE] =		"online",
	[KOBJ_OFFLINE] =	"offline",
	[KOBJ_BIND] =		"bind",
	[KOBJ_UNBIND] =		"unbind",
};

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
		       char *envp_ext[])
{
	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);

	/*
	 * Mark "remove" event done regardless of result, for some subsystems
	 * do not want to re-trigger "remove" event via automatic cleanup.
	 */
	if (action == KOBJ_REMOVE)
		kobj->state_remove_uevent_sent = 1;

	/* search the kset we belong to */
	struct kobject *top_kobj = kobj;
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;

	if (!top_kobj->kset) {
		pr_debug("kobject: '%s' (%p): %s: attempted to send uevent "
			 "without kset!\n", kobject_name(kobj), kobj,
			 __func__);
		return -EINVAL;
	}

	struct kset *kset = top_kobj->kset;
	const struct kset_uevent_ops *uevent_ops = kset->uevent_ops;

	/* skip the event, if uevent_suppress is set*/
	if (kobj->uevent_suppress) {
		pr_debug("kobject: '%s' (%p): %s: uevent_suppress "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
		return 0;
	}
	/* skip the event, if the filter returns zero. */
	if (uevent_ops && uevent_ops->filter)
		if (!uevent_ops->filter(kobj)) {
			pr_debug("kobject: '%s' (%p): %s: filter function "
				 "caused the event to drop!\n",
				 kobject_name(kobj), kobj, __func__);
			return 0;
		}

	/* originating subsystem */
	const char *subsystem;
	if (uevent_ops && uevent_ops->name)
		subsystem = uevent_ops->name(kobj);
	else
		subsystem = kobject_name(&kset->kobj);
	if (!subsystem) {
		pr_debug("kobject: '%s' (%p): %s: unset subsystem caused the "
			 "event to drop!\n", kobject_name(kobj), kobj,
			 __func__);
		return 0;
	}

	/* environment buffer */
	struct kobj_uevent_env *env = kzalloc_obj(struct kobj_uevent_env);
	if (!env)
		return -ENOMEM;

	int ret = 0;
	/* complete object path */
	const char *devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		ret = -ENOENT;
		goto exit;
	}

	/* default keys */
	const char *action_string = kobject_actions[action];
	ret = add_uevent_var(env, "ACTION=%s", action_string);
	if (ret)
		goto exit;
	ret = add_uevent_var(env, "DEVPATH=%s", devpath);
	if (ret)
		goto exit;
	ret = add_uevent_var(env, "SUBSYSTEM=%s", subsystem);
	if (ret)
		goto exit;

	/* keys passed in from the caller */
	if (envp_ext) {
		for (int i = 0; envp_ext[i]; i++) {
			ret = add_uevent_var(env, "%s", envp_ext[i]);
			if (ret)
				goto exit;
		}
	}

	/* let the kset specific function add its stuff */
	if (uevent_ops && uevent_ops->uevent) {
		ret = uevent_ops->uevent(kobj, env);
		if (ret) {
			pr_debug("kobject: '%s' (%p): %s: uevent() returned "
				 "%d\n", kobject_name(kobj), kobj,
				 __func__, ret);
			goto exit;
		}
	}

	struct device *dev = safe_kobj_to_dev(kobj);

	switch (action) {
	case KOBJ_ADD:
	{
		kobj->state_add_uevent_sent = 1;
		/* If we are supplied a MODALIAS, load the requested module. */
		char modalias[64] = {};
		for (int i = 0; i < UEVENT_NUM_ENVP && env->envp[i]; i++) {
			if (strncmp(env->envp[i], "MODALIAS=", 9) == 0) {
				strscpy(modalias, env->envp[i] + 9, sizeof(modalias));
				break;
			}
		}
		if (modalias[0] != '\0') {
			request_module("%s", modalias);
		}

		/* We only create NT device object for char devices and net_device objects */
		if (!dev || !(dev->devt || to_net_dev_safe(dev))) {
			ret = 0;
			goto exit;
		}

		/* Call the LNXDRV driver core to create a new NT device object. If
		 * the device object has a parent LNXDRV device, we retrive the driver
		 * object handle and the function device name from the parent device.
		 * For net_device, the parent device is set by SET_NETDEV_DEV (usually
		 * in the probe() routine) before the struct device is registered. */
		void *driver_handle = global_driver_object_handle;
		const char *name = dev_name(dev);
		struct device *parent = dev->parent;
		char buf[256] = {};
		if (parent && parent->lnxdrv_data) {
			struct lnxdrv_dev_ext *parent_ext = parent->lnxdrv_data;
			snprintf(buf, sizeof(buf), "%s:%s",
				 parent_ext->name ? parent_ext->name : "",
				 name);
			name = buf;
		}

		struct lnxdrv_dev_ext *ext = lnxdrv_core_create_device(driver_handle,
								       dev, name);
		ret = IS_ERR(ext) ? PTR_ERR(ext) : 0;
		break;
	}

	case KOBJ_REMOVE:
		if (!dev || !(dev->devt || to_net_dev_safe(dev))) {
			ret = 0;
			goto exit;
		}

		if (dev->lnxdrv_data) {
			lnxdrv_core_delete_device(dev->lnxdrv_data);
			BUG_ON(dev->lnxdrv_data);
		}
		break;

	case KOBJ_CHANGE:
	case KOBJ_BIND:
	case KOBJ_UNBIND:
	default:
		/* TODO */
		break;
	}

exit:
	kfree(devpath);
	kfree(env);
	return ret;
}
EXPORT_SYMBOL_GPL(kobject_uevent_env);

static struct lnxdrv_bus lnxdrv_bus_table[] = {
#ifdef CONFIG_PCI
	{ .name = "PCI",
	  .match = lnxdrv_pci_match,
	  .start_device = lnxdrv_pci_start_device }
#endif
};

int lnxdrv_add_device(void *driver_object, void *kevent,
		      void *pdo, const char *dev_inst_path)
{
	KERNEL_THREAD_ENTER(kevent);
	int ret = -ENODEV;
	for (int i = 0; i < ARRAY_SIZE(lnxdrv_bus_table); i++) {
		struct lnxdrv_bus *bus = &lnxdrv_bus_table[i];
		const char *name = bus->name;
		int name_len = strlen(name);
		if (!strncmp(name, dev_inst_path, name_len) &&
		    dev_inst_path[name_len] == '\\') {
			BUG_ON(!bus->match);
			ret = bus->match(bus, driver_object, pdo,
					 dev_inst_path + name_len + 1);
			goto out;
		}
	}
out:
	KERNEL_THREAD_EXIT;
	return ret;
}

#define DISPATCH_ROUTINE_PROLOGUE(ntdev, dev, ext, kevent)	\
	BUG_ON(!ntdev);						\
	BUG_ON(!kevent);					\
	struct lnxdrv_dev_ext *ext =				\
		ntos_get_device_extension(ntdev);		\
	BUG_ON(!ext);						\
	BUG_ON(ext->nt_handle != ntdev);			\
	struct device *dev = ext->device;			\
	BUG_ON(!dev->lnxdrv_data);				\
	BUG_ON(dev->lnxdrv_data != ext);			\
	KERNEL_THREAD_ENTER(kevent)

#define DISPATCH_ROUTINE_EPILOGUE		\
	KERNEL_THREAD_EXIT

int lnxdrv_start_device(void *irp,
			void *ntdev, void *kevent,
			unsigned int res_count,
			PLNXDRV_RESOURCE resources)
{
	DISPATCH_ROUTINE_PROLOGUE(ntdev, dev, ext, kevent);
	int ret = -EPERM;
	if (!ext->bus) {
		goto out;
	}
	if (ext->lower_do_handle) {
		ret = ntos_forward_irp(ext->lower_do_handle, irp);
		if (ret) {
			goto out;
		}
	}
	BUG_ON(!ext->bus->start_device);
	ret = ext->bus->start_device(ext->bus, dev, res_count, resources);
out:
	DISPATCH_ROUTINE_EPILOGUE;
	return ret;
}

struct lnxdrv_dev_type lnxdrv_dev_types[LnxMaxDevType];

int lnxdrv_dispatch_create(void *irp, void *device_object, void *kevent,
			   void *file_object)
{
	DISPATCH_ROUTINE_PROLOGUE(device_object, dev, dev_ext, kevent);
	int ret = -ENODEV;
	for (int i = 0; i < ARRAY_SIZE(lnxdrv_dev_types); i++) {
		BUG_ON(!lnxdrv_dev_types[i].match);
		if (lnxdrv_dev_types[i].match(dev)) {
			ret = lnxdrv_dev_types[i].create(dev, irp, file_object);
			goto out;
		}
	}
out:	DISPATCH_ROUTINE_EPILOGUE;
	return ret;
}

int lnxdrv_dispatch_readwrite(void *irp, void *device_object, void *kevent,
			      void *file_object, unsigned long long file_offset,
			      void *buffer, unsigned int buffer_length,
			      unsigned long *pfn_db, unsigned int pfn_count,
			      int write, unsigned int *result_length)
{
	DISPATCH_ROUTINE_PROLOGUE(device_object, dev, dev_ext, kevent);
	int ret = -ENODEV;
	for (int i = 0; i < ARRAY_SIZE(lnxdrv_dev_types); i++) {
		BUG_ON(!lnxdrv_dev_types[i].match);
		if (lnxdrv_dev_types[i].match(dev)) {
			ret = lnxdrv_dev_types[i].read_write(dev, irp, file_object,
							     file_offset, buffer,
							     buffer_length,
							     pfn_db, pfn_count,
							     write, result_length);
			goto out;
		}
	}
out:
	DISPATCH_ROUTINE_EPILOGUE;
	return ret;
}

int lnxdrv_dispatch_cleanup(void *irp, void *device_object, void *kevent,
			    void *file_object)
{
	DISPATCH_ROUTINE_PROLOGUE(device_object, dev, dev_ext, kevent);
	int ret = 0;
	for (int i = 0; i < ARRAY_SIZE(lnxdrv_dev_types); i++) {
		BUG_ON(!lnxdrv_dev_types[i].match);
		if (lnxdrv_dev_types[i].match(dev) && lnxdrv_dev_types[i].cleanup) {
			ret = lnxdrv_dev_types[i].cleanup(dev, irp, file_object);
			goto out;
		}
	}
out:	DISPATCH_ROUTINE_EPILOGUE;
	return ret;
}
