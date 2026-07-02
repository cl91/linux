#include <linux/device.h>
#include <ntlnxdrv.h>

/* Much like the struct bus_type in the Linux device driver model, a lnxdrv_bus
 * represents a matching domain between the NT PNP manager and the Linux drivers.
 * The AddDevice routine on the NT side will supply lnxdrv_add_device() with the
 * device instance path (eg. PCI\VEN_1B36&DEV_0011&SUBSYS_11001AF4&REV_01\18) of
 * the PDO. lnxdrv_add_device() will then find a lnxdrv_bus matching the enumerator
 * (eg. "PCI" in the device instance path above) to load the appropriate Linux-side
 * drivers and perform initializations and device creations. */
struct lnxdrv_bus {
	const char *name; /* Matches the enumerator ("PCI") in the device instance path */
	int (*match)(struct lnxdrv_bus *self, void *driver_handle,
		     void *pdo, const char *instance);
	int (*start_device)(struct lnxdrv_bus *self, struct device *dev,
			    unsigned int res_count,
			    PLNXDRV_RESOURCE resources);
};

/* Device extension attached to the NT DEVICE_OBJECT. */
struct lnxdrv_dev_ext {
	void *nt_handle;
	struct device *device;
	void *driver_handle;
	const char *name;
	struct lnxdrv_bus *bus;
	void *pdo_handle;
	void *lower_do_handle;
	void *bus_data;		/* Populated by the lnxdrv_bus */
};

struct lnxdrv_dev_ext *lnxdrv_core_create_device(void *driver_handle,
						 struct device *dev,
						 const char *devname);
int lnxdrv_core_attach_device(struct lnxdrv_dev_ext *dev_ext, void *pdo);
void lnxdrv_core_delete_device(struct lnxdrv_dev_ext *dev_ext);

#ifdef CONFIG_PCI
extern int lnxdrv_pci_match(struct lnxdrv_bus *self, void *driver_handle,
			    void *pdo, const char *instance);
extern int lnxdrv_pci_start_device(struct lnxdrv_bus *self, struct device *dev,
				   unsigned int res_count,
				   PLNXDRV_RESOURCE resources);
#endif

/* A lnxdrv_dev_type represents a device type (eg. char device or net device) handled by
 * the LNXDRV framework. When given a generic struct device, the match() routine returns
 * true if the device is of the given type. If matched, the dispatch routines are then
 * called to handle the corresponding IRPs (eg. create() for IRP_MJ_CREATE). */
struct lnxdrv_dev_type {
	bool (*match)(struct device *dev);
	int (*create)(struct device *dev, void *irp, void *file_object);
	int (*read_write)(struct device *dev, void *irp, void *file_object,
			  unsigned long long file_offset,
			  void *buffer, unsigned int buffer_length,
			  unsigned long *pfn_db, unsigned int pfn_count,
			  int write, unsigned int *result_length);
	int (*cleanup)(struct device *dev, void *irp, void *file_object);
};

extern struct lnxdrv_dev_type lnxdrv_dev_types[LnxMaxDevType];
