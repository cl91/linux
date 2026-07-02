// SPDX-License-Identifier: GPL-2.0
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-map-ops.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/msi.h>
#include <linux/irqdomain.h>
#include <host_ops.h>
#include <init.h>
#include "core.h"
#include "../../../drivers/pci/pci.h"

unsigned int pci_flags;
EXPORT_SYMBOL_GPL(pci_flags);

bool pci_early_dump;
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_DEFAULT;

/*
 * If we set up a device for bus mastering, we need to check the latency
 * timer as certain BIOSes forget to set it properly.
 */
unsigned int pcibios_max_latency = 255;

#define DEFINE_PARSE_HEX_FUNC(bytes, type)				\
	static inline bool parse_hex_field_##bytes(const char *str,	\
						   const char *key,	\
						   type *out)		\
	{								\
		const char *p;						\
		char buf[bytes + 1];   /* hex digits + null */		\
		int i;							\
									\
		p = strstr(str, key);					\
		if (!p)							\
			return false;					\
									\
		p += strlen(key);					\
									\
		for (i = 0; i < bytes; i++) {				\
			if (!isxdigit(p[i]))				\
				return false;				\
			buf[i] = p[i];					\
		}							\
		buf[bytes] = '\0';					\
									\
		*out = (type)simple_strtoul(buf, NULL, 16);		\
		return true;						\
	}

DEFINE_PARSE_HEX_FUNC(4, u16);
DEFINE_PARSE_HEX_FUNC(8, u32);

/*
 * Bus data attached to a lnxdrv_dev_ext for a PCI device.
 *
 * The io_map table is similar to those defined in pcim_iomap_devres
 * (a deprecated struct). The latter is used for managed pcim_ IO
 * mapping routines and serves a different purpose, so we cannot reuse
 * it (it is also a deprecated struct).
 */
struct lnxdrv_pci_bus_data {
	void __iomem *io_map[PCI_NUM_RESOURCES];
	unsigned long mapped_length[PCI_NUM_RESOURCES];
	unsigned long region_mask;
	struct list_head irq_list;
};

struct lnxdrv_pci_irq {
	struct list_head entry;
	int irq;
	int count;
	bool msix;
};

static inline struct lnxdrv_dev_ext *pdev_to_dev_ext(const struct pci_dev *dev)
{
	return dev->dev.lnxdrv_data;
}

static inline struct lnxdrv_pci_bus_data *pdev_to_bus_data(const struct pci_dev *dev)
{
	return pdev_to_dev_ext(dev)->bus_data;
}

static inline void *pdev_to_pdo(const struct pci_dev *dev)
{
	BUG_ON(!dev);
	struct lnxdrv_dev_ext *dev_ext = pdev_to_dev_ext(dev);
	BUG_ON(!dev_ext);
	BUG_ON(!dev_ext->pdo_handle);
	return dev_ext->pdo_handle;
}

static u8 pci_hdr_type(struct pci_dev *dev)
{
	u8 hdr_type;
	pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type);
	return hdr_type;
}

static u32 pci_class(struct pci_dev *dev)
{
	u32 class;
	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class);
	return class;
}

/**
 * pci_setup_device - Fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's
 * vendor,class,memory and IO-space addresses, IRQ lines etc. We only
 * care about type 0 header (PCI devices) and ignore PCI/Cardbus bridges.
 */
int pci_setup_device(struct pci_dev *dev)
{
	u32 class;
	u8 hdr_type;
	int err;

	hdr_type = pci_hdr_type(dev);

	dev->sysdata = dev->bus->sysdata;
	dev->dev.parent = dev->bus->bridge;
	dev->dev.bus = &pci_bus_type;
	dev->hdr_type = FIELD_GET(PCI_HEADER_TYPE_MASK, hdr_type);
	dev->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr_type);
	dev->error_state = pci_channel_io_normal;
	set_pcie_port_type(dev);

	err = pci_set_of_node(dev);
	if (err)
		return err;
	pci_set_acpi_fwnode(dev);

	pci_dev_assign_slot(dev);

	/*
	 * Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
	 * set this higher, assuming the system even supports it.
	 */
	dev->dma_mask = 0xffffffff;

	/*
	 * Assume 64-bit addresses for MSI initially. Will be changed to 32-bit
	 * if MSI (rather than MSI-X) capability does not have
	 * PCI_MSI_FLAGS_64BIT. Can also be overridden by driver.
	 */
	dev->msi_addr_mask = DMA_BIT_MASK(64);

	dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
		     dev->bus->number, PCI_SLOT(dev->devfn),
		     PCI_FUNC(dev->devfn));

	class = pci_class(dev);

	dev->revision = class & 0xff;
	dev->class = class >> 8;		    /* upper 3 bytes */

	/* Need to have dev->class ready */
	dev->cfg_size = pci_cfg_space_size(dev);

	if (pci_is_pcie(dev))
		dev->supported_speeds = pcie_get_supported_speeds(dev);

	/* "Unknown power state" */
	dev->current_state = PCI_UNKNOWN;

	pci_info(dev, "[%04x:%04x] type %02x class %#08x\n",
		 dev->vendor, dev->device, dev->hdr_type, dev->class);

	/* Device class may be changed after fixup */
	class = dev->class >> 8;

	return 0;
}

int lnxdrv_pci_match(struct lnxdrv_bus *self,
		     void *driver_handle,
		     void *pdo,
		     const char *inst_path)
{
	u16 vendor, device;
	u32 subsystem;

	if (!parse_hex_field_4(inst_path, "VEN_", &vendor))
		return -EINVAL;

	if (!parse_hex_field_4(inst_path, "DEV_", &device))
		return -EINVAL;

	parse_hex_field_8(inst_path, "SUBSYS_", &subsystem);

	/* Get the bus number and device:function index of this PCI device. */
	unsigned int busnr = 0;
	unsigned int address = 0;
	int ret = ntos_get_device_slot_address(pdo, &busnr, &address);
	if (ret) {
		return ret;
	}
	/* The segment number is the high 24 bits of busnr. */
	unsigned int domain = busnr >> 8;
	busnr &= 0xff;
	/* Device is the high 16 bits of address. Function is the low 16 bits. */
	unsigned int devfn = PCI_DEVFN(address >> 16, address & 0xffff);

	pr_info("Matching PCI device: vendor=%04x device=%04x segment 0x%x "
		"bus 0x%x slot 0x%x func 0x%x\n",
		vendor, device, domain, busnr, PCI_SLOT(devfn), PCI_FUNC(devfn));

	struct pci_bus *bus = pci_find_bus(domain, busnr);
	if (!bus) {
		LIST_HEAD(resources);
		struct pci_ops ops = {};
		/* We temporarily stick the domain number into the sysdata since an
		 * arch port is free to use this member to store whatever it needs.
		 * pci_bus_find_domain_nr will read it and return the domain number
		 * to pci_create_root_bus. */
		bus = pci_create_root_bus(NULL, busnr, &ops,
					  (void *)(unsigned long)domain, &resources);
		bus->sysdata = NULL;
		if (!bus) {
			return -ENOMEM;
		}
	}

	/* Create the pci_dev on the Linux side and fill the pci_dev struct. */
	struct pci_dev *dev = pci_alloc_dev(bus);
	if (!dev)
		return -ENOMEM;

	dev->devfn = devfn;
	dev->vendor = vendor;
	dev->device = device;
	if (subsystem) {
		dev->subsystem_vendor = subsystem & 0xffff;
		dev->subsystem_device = subsystem >> 16;
	}

	/* Create the NT FDO and attach the FDO on top of the PDO */
	char buf[128] = {};
	snprintf(buf, sizeof(buf), "PCI(%x)BUS(%x)DEV(%x)FUNC(%x)_VEN%04x_DEV%04x", domain,
		 busnr, PCI_SLOT(devfn), PCI_FUNC(devfn), vendor, device);
	struct lnxdrv_dev_ext *dev_ext = lnxdrv_core_create_device(driver_handle,
								   &dev->dev, buf);
	if (IS_ERR(dev_ext)) {
		ret = PTR_ERR(dev_ext);
		dev_ext = NULL;
		goto err;
	}
	dev_ext->bus = self;

	ret = lnxdrv_core_attach_device(dev_ext, pdo);
	if (ret) {
		goto err;
	}
	dev_ext->bus_data = kmalloc(sizeof(struct lnxdrv_pci_bus_data),
				    GFP_KERNEL);
	if (!dev_ext->bus_data) {
		ret = -ENOMEM;
		goto err;
	}
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
	BUG_ON(!bus_data);
	INIT_LIST_HEAD(&bus_data->irq_list);

	ret = pci_setup_device(dev);
	if (ret) {
		goto err;
	}
	return 0;

err:
	if (dev) {
		pci_bus_put(dev->bus);
		kfree(dev);
	}
	if (dev_ext) {
		if (dev_ext->bus_data) {
			kfree(dev_ext->bus_data);
		}
		lnxdrv_core_delete_device(dev_ext);
	}
	return ret;
}

static int lnxdrv_pci_populate_resources(struct pci_dev *dev,
					 unsigned int res_count,
					 PLNXDRV_RESOURCE resources)
{
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
	BUG_ON(!bus_data);
	for (int i = 0; i < res_count; i++) {
		switch (resources[i].Type) {
		case LnxResMemory:
		case LnxResIoPort:
		{
			int bar = resources[i].IoRange.Index;
			BUG_ON(bar >= PCI_NUM_RESOURCES);

			dev->resource[bar].start = resources[i].IoRange.Start;
			dev->resource[bar].end = resources[i].IoRange.Start +
				resources[i].IoRange.Length - 1;

			dev->resource[bar].flags = resources[i].Type == LnxResMemory ?
				IORESOURCE_MEM : IORESOURCE_IO;
			break;
		}

		case LnxResInterrupt:
			switch (resources[i].Interrupt.Type) {
			case LnxDrvInterruptTypeLegacy:
				dev->irq = resources[i].Interrupt.Irq;
				break;
			case LnxDrvInterruptTypeMsi:
			case LnxDrvInterruptTypeMsiX:
			{
				struct lnxdrv_pci_irq *irq =
					kzalloc(sizeof(struct lnxdrv_pci_irq), GFP_KERNEL);
				if (!irq) {
					return -ENOMEM;
				}
				list_add_tail(&irq->entry, &bus_data->irq_list);
				irq->irq = resources[i].Interrupt.Irq;
				irq->count = resources[i].Interrupt.Count;
				if (resources[i].Interrupt.Type == LnxDrvInterruptTypeMsiX) {
					irq->msix = true;
				}
				if (irq->msix) {
					dev->msix_enabled = true;
				} else {
					dev->msi_enabled = true;
				}
				break;
			}
			default:
				BUG_ON(1);
			}
			break;

		default:
			/* ignore every other resource type */
			break;
		}
	}
	return 0;
}

int lnxdrv_pci_start_device(struct lnxdrv_bus *self, struct device *dev,
			    unsigned int res_count,
			    PLNXDRV_RESOURCE resources)
{
	BUG_ON(!dev_is_pci(dev));
	struct pci_dev *pdev = to_pci_dev(dev);
	pci_device_add(pdev, pdev->bus);
	int ret = lnxdrv_pci_populate_resources(pdev, res_count, resources);
	if (ret) {
		return ret;
	}
	/* Save config space for error recoverability */
	pci_save_state(pdev);
	pci_dev_allow_binding(pdev);
	device_initial_probe(dev);
	wait_for_device_probe();
	bool success = false;
	device_lock(dev);
	success = (dev->driver != NULL);
	device_unlock(dev);
	if (success) {
		pci_dev_assign_added(pdev);
		return 0;
	} else {
		return -EIO;
	}
}

int pcibios_device_add(struct pci_dev *dev)
{
	/* Do nothing. */
	return 0;
}

void pcibios_release_device(struct pci_dev *dev) {}

void pci_reassigndev_resource_alignment(struct pci_dev *dev)
{
	/* Do nothing */
}

void pci_init_reset_methods(struct pci_dev *dev)
{
	/* Do nothing */
}

void pci_configure_ari(struct pci_dev *dev)
{
	/* Do nothing. ARI is configured by the PCI bus driver. */
}

void pci_acs_init(struct pci_dev *dev)
{
	/* Do nothing. ACS is configured by the PCI bus driver. */
}

void pci_enable_acs(struct pci_dev *dev)
{
	/* Do nothing. ACS is enabled by the PCI bus driver. */
}

void pci_msi_init(struct pci_dev *dev)
{
	/* Do nothing. MSI is configured by the PCI bus driver. */
}

void pci_msix_init(struct pci_dev *dev)
{
	/* Do nothing. MSI-X is configured by the PCI bus driver. */
}

void pci_ea_init(struct pci_dev *dev)
{
	/* Do nothing. Enhanced Allocation is configured and arbitrated
	 * by the PCI bus driver. */
}

void pci_configure_aspm_l1ss(struct pci_dev *pdev)
{
	/* Do nothing. PCIE ASPM is configured by the PCI bus driver. */
}

void pci_save_aspm_l1ss_state(struct pci_dev *pdev)
{
	/* Do nothing. PCIE ASPM is configured by the PCI bus driver. */
}

void pci_save_ltr_state(struct pci_dev *dev)
{
	/* Do nothing. PCIE ASPM is configured by the PCI bus driver. */
}

struct irq_domain *pci_msi_get_device_domain(struct pci_dev *pdev)
{
	return irq_get_default_domain();
}

bool pci_bridge_d3_possible(struct pci_dev *bridge)
{
	return false;
}

void pci_bridge_d3_update(struct pci_dev *dev)
{
	/* Do nothing as PCI bridges are managed by the PCI bus driver on NT. */
}

void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev)
{
	/* Device power up is done by the PCI bus driver so we simply
	 * set the power state member of pci_dev. */
	pci_dev->current_state = PCI_D0;
}

/**
 * pci_choose_state - Choose the power state of a PCI device.
 * @dev: Target PCI device.
 * @state: Target state for the whole system.
 *
 * Returns PCI power state suitable for @dev and @state.
 */
pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state)
{
	/* For now we will always return D0 */
	return PCI_D0;
}
EXPORT_SYMBOL(pci_choose_state);

void pci_pme_active(struct pci_dev *dev, bool enable)
{
	/* Do nothing as we don't support PME# wake up. */
}
EXPORT_SYMBOL(pci_pme_active);

int pci_bus_find_domain_nr(struct pci_bus *bus, struct device *parent)
{
	/* See lnxdrv_pci_match for how we use the bus->sysdata member */
	if (bus->domain_nr != PCI_DOMAIN_NR_NOT_SET) {
		return bus->domain_nr;
	}
	return (unsigned long)bus->sysdata;
}

void pci_bus_release_domain_nr(struct device *parent, int domain_nr)
{
	/* Do nothing */
}

void pci_bus_release_emul_domain_nr(int domain_nr)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_bus_release_emul_domain_nr);

/**
 * pci_reset_bus - Try to reset a PCI bus
 * @pdev: top level PCI device to reset via slot/bus
 *
 * We don't support function drivers initialing a reset of the PCI bus, so
 * always return error here.
 */
int pci_reset_bus(struct pci_dev *pdev)
{
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(pci_reset_bus);

int pci_reset_function(struct pci_dev *dev)
{
	/* We don't allow drivers to initiate PCI function reset */
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(pci_reset_function);

/* We don't allow drivers to initiate PCI reset, so there is no need to save
 * or load PCI state. */
struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_store_saved_state);

int pci_load_saved_state(struct pci_dev *dev,
			 struct pci_saved_state *state)
{
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(pci_load_saved_state);

void pci_assign_irq(struct pci_dev *dev)
{
	/* Do nothing as IRQ is assigned by the NT IO manager. We only record
	 * the assigned IRQ information (in lnxdrv_pci_bus_data). */
}

/**
 * pci_irq_vector() - Get Linux IRQ number of a device interrupt vector
 * @dev: the PCI device to operate on
 * @nr:  device-relative interrupt vector index (0-based); has different
 *       meanings, depending on interrupt mode:
 *
 *         * MSI-X     the index in the MSI-X vector table
 *         * MSI       the index of the enabled MSI vectors
 *         * INTx      must be 0
 *
 * Return: the Linux IRQ number, or -EINVAL if @nr is out of range
 */
int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	if (pci_dev_msi_enabled(dev)) {
		struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
		BUG_ON(!bus_data);
		struct lnxdrv_pci_irq *irq;
		int idx = 0;
		list_for_each_entry(irq, &bus_data->irq_list, entry) {
			if (nr == idx) {
				return irq_create_mapping(NULL, irq->irq);
			}
			idx++;
		}
	} else {
		BUG_ON(nr);
		if (!nr) {
			return dev->irq;
		}
	}
	return -EINVAL;
}
EXPORT_SYMBOL(pci_irq_vector);

int pci_enable_device(struct pci_dev *dev)
{
	return 0;
}
EXPORT_SYMBOL(pci_enable_device);

int pcibios_enable_device(struct pci_dev *dev, int bars)
{
	/* We do not need to do anything on the Linux side as PCI device
	 * enabling is done by the PCI bus driver on the NT side. */
	return 0;
}

void pci_disable_device(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL(pci_disable_device);

static bool pci_read_dev_vendor_id(struct pci_dev *dev, u32 *l)
{
        if (pci_read_config_dword(dev, PCI_VENDOR_ID, l))
                return false;

        /* Some broken boards return 0 or ~0 (PCI_ERROR_RESPONSE) if a slot is empty: */
        if (PCI_POSSIBLE_ERROR(*l) || *l == 0x00000000 ||
            *l == 0x0000ffff || *l == 0xffff0000)
                return false;

        return true;
}

bool pci_device_is_present(struct pci_dev *pdev)
{
        u32 v;

        /* Check PF if pdev is a VF, since VF Vendor/Device IDs are 0xffff */
        pdev = pci_physfn(pdev);
        if (pci_dev_is_disconnected(pdev))
                return false;
        return pci_read_dev_vendor_id(pdev, &v);
}
EXPORT_SYMBOL_GPL(pci_device_is_present);

int pci_prepare_to_sleep(struct pci_dev *dev)
{
	return 0;
}
EXPORT_SYMBOL(pci_prepare_to_sleep);

void pci_ignore_hotplug(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_ignore_hotplug);


/**
 * pci_d3cold_enable - Enable D3cold for device
 * @dev: PCI device to handle
 *
 * We don't allow function drivers to alter the D3cold status.
 */
void pci_d3cold_enable(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_d3cold_enable);

/**
 * pci_d3cold_disable - Disable D3cold for device
 * @dev: PCI device to handle
 *
 * We don't allow function drivers to alter the D3cold status.
 */
void pci_d3cold_disable(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_d3cold_disable);

/**
 * pci_wake_from_d3 - enable/disable device to wake up from D3_hot or D3_cold
 * @dev: PCI device to prepare
 * @enable: True to enable wake-up event generation; false to disable
 *
 * Many drivers want the device to wake up the system from D3_hot or D3_cold
 * and this function allows them to set that up cleanly - pci_enable_wake()
 * should not be called twice in a row to enable wake-up due to PCI PM vs ACPI
 * ordering constraints.
 *
 * This function only returns error code if the device is not allowed to wake
 * up the system from sleep or it is not capable of generating PME# from both
 * D3_hot and D3_cold and the platform is unable to enable wake-up power for it.
 */
int pci_wake_from_d3(struct pci_dev *dev, bool enable)
{
	return -EINVAL;
}
EXPORT_SYMBOL(pci_wake_from_d3);

/**
 * pci_dev_run_wake - Check if device can generate run-time wake-up events.
 * @dev: Device to check.
 *
 * Return true if the device itself is capable of generating wake-up events
 * (through the platform or using the native PCIe PME) or if the device supports
 * PME and one of its upstream bridges can generate wake-up events.
 */
bool pci_dev_run_wake(struct pci_dev *dev)
{
	return false;
}
EXPORT_SYMBOL_GPL(pci_dev_run_wake);

/**
 * pci_enable_wake - change wakeup settings for a PCI device
 * @pci_dev: Target device
 * @state: PCI state from which device will issue wakeup events
 * @enable: Whether or not to enable event generation
 *
 * If @enable is set, check device_may_wakeup() for the device before calling
 * __pci_enable_wake() for it.
 */
int pci_enable_wake(struct pci_dev *pci_dev, pci_power_t state, bool enable)
{
	return -EINVAL;
}
EXPORT_SYMBOL(pci_enable_wake);

/**
 * pci_set_power_state - Set the power state of a PCI device
 * @dev: PCI device to handle.
 * @state: PCI power state (D0, D1, D2, D3hot) to put the device into.
 *
 * Transition a device to a new power state, using the platform firmware and/or
 * the device's PCI PM registers.
 *
 * RETURN VALUE:
 * -EINVAL if the requested state is invalid.
 * -EIO if device does not support PCI PM or its PM capabilities register has a
 * wrong version, or device doesn't support the requested state.
 * 0 if the transition is to D1 or D2 but D1 and D2 are not supported.
 * 0 if device already is in the requested state.
 * 0 if the transition is to D3 but D3 is not supported.
 * 0 if device's power state has been successfully changed.
 */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	return -EINVAL;
}
EXPORT_SYMBOL(pci_set_power_state);

/*
 * TODO: Save the part of the PCI config space that the client driver is allow to touch.
 */
int pci_save_state(struct pci_dev *dev)
{
	return 0;
}
EXPORT_SYMBOL(pci_save_state);

void pci_restore_state(struct pci_dev *dev)
{
	/* TODO */
}
EXPORT_SYMBOL(pci_restore_state);

void pci_allocate_cap_save_buffers(struct pci_dev *dev)
{
	/* TOOD: See above */
}

void pci_free_cap_save_buffers(struct pci_dev *dev)
{
	/* TODO: See above */
}

/**
 * pci_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * This routine calls the PCI bus driver to enable the Memory-Write-Invalidate
 * transaction in %PCI_COMMAND.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int pci_set_mwi(struct pci_dev *dev)
{
	/* TODO */
	return 0;
}
EXPORT_SYMBOL(pci_set_mwi);

/**
 * pci_clear_mwi - disables Memory-Write-Invalidate for device dev
 * @dev: the PCI device to disable
 *
 * Disables PCI Memory-Write-Invalidate transaction on the device by calling
 * the PCI bus driver.
 */
void pci_clear_mwi(struct pci_dev *dev)
{
	/* TODO */
}
EXPORT_SYMBOL(pci_clear_mwi);

/**
 * pci_intx - enables/disables PCI INTx for device dev
 * @pdev: the PCI device to operate on
 * @enable: boolean: whether to enable or disable PCI INTx
 *
 * On Neptune OS, PCI INTx is always disabled, so this routine is a no-op.
 */
void pci_intx(struct pci_dev *pdev, int enable)
{
	/* Do nothing */
}

/**
 * pci_enable_msi() - Enable MSI interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to enable MSI interrupts mode on device and
 * allocate a single interrupt vector. On success, the allocated vector
 * Linux IRQ will be saved at @dev->irq. The driver must invoke
 * pci_disable_msi() on cleanup.
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 *
 * Return: 0 on success, errno otherwise
 */
int pci_enable_msi(struct pci_dev *dev)
{
	if (!dev->msi_enabled) {
		return -EINVAL;
	}
	int irq = pci_irq_vector(dev, 0);
	if (irq < 0) {
		return irq;
	}
	dev->irq = irq;
	return 0;
}
EXPORT_SYMBOL(pci_enable_msi);

/**
 * pci_disable_msi() - Disable MSI interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to disable MSI interrupt mode on device.
 * The PCI device Linux IRQ (@dev->irq) is restored to zero. This is
 * the cleanup pair of pci_enable_msi().
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 */
void pci_disable_msi(struct pci_dev *dev)
{
	if (!pci_msi_enabled() || !dev || !dev->msi_enabled)
		return;
	dev->irq = 0;
}
EXPORT_SYMBOL(pci_disable_msi);

/**
 * pci_enable_msix_range() - Enable MSI-X interrupt mode on device
 * @dev:     the PCI device to operate on
 * @entries: input/output parameter, array of MSI-X configuration entries
 * @minvec:  minimum required number of MSI-X vectors
 * @maxvec:  maximum desired number of MSI-X vectors
 *
 * Legacy device driver API to enable MSI-X interrupt mode on device and
 * configure its MSI-X capability structure as appropriate.  The passed
 * @entries array must have each of its members "entry" field set to a
 * desired (valid) MSI-X vector number, where the range of valid MSI-X
 * vector numbers can be queried through pci_msix_vec_count().  If
 * successful, the driver must invoke pci_disable_msix() on cleanup.
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 *
 * Return: number of MSI-X vectors allocated (which might be smaller
 * than @maxvecs), where Linux IRQ numbers for such allocated vectors
 * are saved back in the @entries array elements' "vector" field. Return
 * -ENOSPC if less than @minvecs interrupt vectors are available.
 * Return -EINVAL if one of the passed @entries members "entry" field
 * was invalid or a duplicate, or if plain MSI interrupts mode was
 * earlier enabled on device. Return other errnos otherwise.
 */
int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			  int minvec, int maxvec)
{
	if (minvec < 0) {
		return -EINVAL;
	}
	if (!dev->msix_enabled) {
		return -EINVAL;
	}
	int i;
	for (i = 0; i < maxvec; i++) {
		int irq = pci_irq_vector(dev, entries[i].entry);
		if (irq < 0) {
			return i >= minvec ? i : -ENOSPC;
		}
		entries[i].vector = irq;
	}
	return i;
}
EXPORT_SYMBOL(pci_enable_msix_range);

/**
 * pci_disable_msix() - Disable MSI-X interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to disable MSI-X interrupt mode on device,
 * free earlier-allocated interrupt vectors, and restore INTx.
 * The PCI device Linux IRQ (@dev->irq) is restored to its default pin
 * assertion IRQ. This is the cleanup pair of pci_enable_msix_range().
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 */
void pci_disable_msix(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL(pci_disable_msix);

void pci_restore_msi_state(struct pci_dev *dev)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_restore_msi_state);

/**
 * pci_alloc_irq_vectors() - Allocate multiple device interrupt vectors
 * @dev:      the PCI device to operate on
 * @min_vecs: minimum required number of vectors (must be >= 1)
 * @max_vecs: maximum desired number of vectors
 * @flags:    One or more of:
 *
 *            * %PCI_IRQ_MSIX      Allow trying MSI-X vector allocations
 *            * %PCI_IRQ_MSI       Allow trying MSI vector allocations
 *
 *            * %PCI_IRQ_INTX      Allow trying INTx interrupts, if and
 *              only if @min_vecs == 1
 *
 *            * %PCI_IRQ_AFFINITY  Auto-manage IRQs affinity by spreading
 *              the vectors around available CPUs
 *
 * Allocate up to @max_vecs interrupt vectors on device. MSI-X irq
 * vector allocation has a higher precedence over plain MSI, which has a
 * higher precedence over legacy INTx emulation.
 *
 * Upon a successful allocation, the caller should use pci_irq_vector()
 * to get the Linux IRQ number to be passed to request_threaded_irq().
 * The driver must call pci_free_irq_vectors() on cleanup.
 *
 * Return: number of allocated vectors (which might be smaller than
 * @max_vecs), -ENOSPC if less than @min_vecs interrupt vectors are
 * available, other errnos otherwise.
 */
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs,
					      flags, NULL);
}
EXPORT_SYMBOL(pci_alloc_irq_vectors);

/**
 * pci_alloc_irq_vectors_affinity() - Allocate multiple device interrupt
 *                                    vectors with affinity requirements
 * @dev:      the PCI device to operate on
 * @min_vecs: minimum required number of vectors (must be >= 1)
 * @max_vecs: maximum desired number of vectors
 * @flags:    allocation flags, as in pci_alloc_irq_vectors()
 * @affd:     affinity requirements (can be %NULL).
 *
 * Same as pci_alloc_irq_vectors(), but with the extra @affd parameter.
 * Check that function docs, and &struct irq_affinity, for more details.
 */
int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd)
{
	/* Since IRQ allocation is done by the PCI bus driver on the NT side,
	 * we simply check that PnP manager has given us enough irq vectors. */
	BUG_ON(!min_vecs);
	if (pci_dev_msi_enabled(dev)) {
		struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
		BUG_ON(!bus_data);
		struct lnxdrv_pci_irq *irq;
		unsigned int nvecs = 0;
		list_for_each_entry(irq, &bus_data->irq_list, entry) {
			nvecs++;
		}
		if (nvecs < min_vecs) {
			return -ENOSPC;
		}
		return min(nvecs, max_vecs);
	} else {
		return 1;
	}
}
EXPORT_SYMBOL(pci_alloc_irq_vectors_affinity);


/**
 * pci_free_irq_vectors() - Free previously allocated IRQs for a device
 * @dev: the PCI device to operate on
 *
 * Undo the interrupt vector allocations and possible device MSI/MSI-X
 * enablement earlier done through pci_alloc_irq_vectors_affinity() or
 * pci_alloc_irq_vectors().
 */
void pci_free_irq_vectors(struct pci_dev *dev)
{
	pci_disable_msix(dev);
	pci_disable_msi(dev);
}
EXPORT_SYMBOL(pci_free_irq_vectors);

/**
 * pci_enable_device_mem - Initialize a device for use with Memory space
 * @dev: PCI device to be initialized
 *
 * This routine is a no-op on Neptune OS as decoding is enabled by the
 * bus driver on the NT side.
 */
int pci_enable_device_mem(struct pci_dev *dev)
{
	return 0;
}
EXPORT_SYMBOL(pci_enable_device_mem);

/**
 * pci_enable_rom - enable ROM decoding for a PCI device
 * @pdev: PCI device to enable
 *
 * Enable ROM decoding on @dev. Like pci_enable_device_mem, this routine is a
 * no-op on Neptune OS as decoding is enabled by the bus driver on the NT side.
 */
int pci_enable_rom(struct pci_dev *pdev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(pci_enable_rom);

void pci_disable_rom(struct pci_dev *pdev)
{
	/* Do nothing */
}
EXPORT_SYMBOL_GPL(pci_disable_rom);

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	BUG_ON(!dev);
	BUG_ON(bar < 0 || bar >= PCI_NUM_RESOURCES);
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
	BUG_ON(!bus_data);
	if (bus_data->io_map[bar]) {
		if (maxlen <= bus_data->mapped_length[bar]) {
			return bus_data->io_map[bar];
		} else {
			pci_iounmap(dev, bus_data->io_map[bar]);
		}
	}
	struct resource *res = &dev->resource[bar];
	size_t length = res->end - res->start + 1;
	if (maxlen && maxlen < length) {
		length = maxlen;
	}
	if (res->flags & IORESOURCE_IO) {
		bus_data->io_map[bar] = (void *)res->start;
		bus_data->mapped_length[bar] = length;
		return bus_data->io_map[bar];
	}
	/* Must be a memory resource */
	BUG_ON(!(res->flags & IORESOURCE_MEM));
	BUG_ON(res->start == 0 || res->end < res->start);
	uint64_t phys = res->start;
	void *va = ntos_map_io_space(phys, length, LnxDrvMemNonCached);
	if (!va) {
		return NULL;
	}
	bus_data->io_map[bar] = va;
	bus_data->mapped_length[bar] = length;
	return (void __iomem *)va;
}
EXPORT_SYMBOL(pci_iomap);

void __iomem *pci_iomap_range(struct pci_dev *dev,
			      int bar,
			      unsigned long offset,
			      unsigned long maxlen)
{
	void __iomem *va = pci_iomap(dev, bar, maxlen ? offset + maxlen : 0);
	if (!va) {
		return NULL;
	}
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
	BUG_ON(!bus_data);
	BUG_ON(!bus_data->io_map[bar]);
	if (offset >= bus_data->mapped_length[bar]) {
		return NULL;
	}
	return va + offset;
}
EXPORT_SYMBOL(pci_iomap_range);

void pci_iounmap(struct pci_dev *dev, void __iomem *p)
{
	BUG_ON(!dev);
	BUG_ON(!p);
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(dev);
	BUG_ON(!bus_data);
	for (int i = 0; i < PCI_NUM_RESOURCES; i++) {
		unsigned long start = (unsigned long)bus_data->io_map[i];
		unsigned long end = start + bus_data->mapped_length[i];
		if ((unsigned long)p >= start && (unsigned long)p < end) {
			if (dev->resource[i].flags & IORESOURCE_MEM) {
				ntos_unmap_io_space(bus_data->io_map[i],
						    bus_data->mapped_length[i]);
			}
			bus_data->io_map[i] = NULL;
			bus_data->mapped_length[i] = 0;
			return;
		}
	}
	BUG_ON(1);
}
EXPORT_SYMBOL(pci_iounmap);

int pci_remap_iospace(const struct resource *res, phys_addr_t phys_addr)
{
	/*
	 * This architecture does not have memory mapped I/O space,
	 * so this function should never be called
	 */
	WARN_ONCE(1, "This architecture does not support memory mapped I/O\n");
	return -ENODEV;
}
EXPORT_SYMBOL(pci_remap_iospace);

void pci_unmap_iospace(struct resource *res)
{
	/* Do nothing */
}
EXPORT_SYMBOL(pci_unmap_iospace);

/* We don't support ISA devices so this routine is trivial. */
resource_size_t pcibios_align_resource(void *data,
				       const struct resource *res,
				       resource_size_t size,
				       resource_size_t align)
{
       return res->start;
}
EXPORT_SYMBOL(pcibios_align_resource);

int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	BUG_ON(!pdev);
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(pdev);
	BUG_ON(!bus_data);
	BUG_ON(bar >= PCI_NUM_RESOURCES);
	if (bus_data->region_mask & (1 << bar)) {
		return -EBUSY;
	}
	bus_data->region_mask |= 1 << bar;
	return 0;
}
EXPORT_SYMBOL(pci_request_region);

int pci_request_selected_regions_exclusive(struct pci_dev *pdev, int bars,
					   const char *name)
{
	BUG_ON(!pdev);
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(pdev);
	BUG_ON(!bus_data);
	BUG_ON(bars >= (1ULL << PCI_NUM_RESOURCES));
	if (bus_data->region_mask & bars) {
		return -EBUSY;
	}
	bus_data->region_mask |= bars;
	return 0;
}
EXPORT_SYMBOL(pci_request_selected_regions_exclusive);

/**
 * pci_request_selected_regions - Reserve selected PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @bars: Bitmask of BARs to be requested
 * @name: Name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 */
int pci_request_selected_regions(struct pci_dev *pdev, int bars,
				 const char *name)
{
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++)
		if (bars & (1 << i))
			if (pci_request_region(pdev, i, name))
				goto err_out;
	return 0;

err_out:
	while (--i >= 0)
		if (bars & (1 << i))
			pci_release_region(pdev, i);

	return -EBUSY;
}
EXPORT_SYMBOL(pci_request_selected_regions);

int pci_request_regions(struct pci_dev *pdev, const char *name)
{
	int ret;
	for (int i = 0; i < PCI_NUM_RESOURCES; i++) {
		ret = pci_request_region(pdev, i, name);
		if (ret) {
			for (int j = 0; j < i; j++) {
				pci_release_region(pdev, j);
			}
			return ret;
		}
	}
	return 0;
}
EXPORT_SYMBOL(pci_request_regions);

void pci_release_region(struct pci_dev *pdev, int bar)
{
	BUG_ON(!pdev);
	struct lnxdrv_pci_bus_data *bus_data = pdev_to_bus_data(pdev);
	BUG_ON(!bus_data);
	bus_data->region_mask &= ~(1 << bar);
}
EXPORT_SYMBOL(pci_release_region);

/**
 * pci_release_selected_regions - Release selected PCI I/O and memory resources
 * @pdev: PCI device whose resources were previously reserved
 * @bars: Bitmask of BARs to be released
 *
 * Release selected PCI I/O and memory resources previously reserved.
 * Call this function only after all use of the PCI regions has ceased.
 */
void pci_release_selected_regions(struct pci_dev *pdev, int bars)
{
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++)
		if (bars & (1 << i))
			pci_release_region(pdev, i);
}
EXPORT_SYMBOL(pci_release_selected_regions);

void pci_release_regions(struct pci_dev *pdev)
{
	for (int i = 0; i < PCI_NUM_RESOURCES; i++) {
		pci_release_region(pdev, i);
	}
}
EXPORT_SYMBOL(pci_release_regions);

const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int nr)
{
	return cpu_possible_mask;
}
EXPORT_SYMBOL(pci_irq_get_affinity);

void pci_no_msi(void)
{
	panic("Disabling MSI is unsupported.");
}

bool pci_msi_enabled(void)
{
	return true;
}

/* Note for the PCI config access routines, we do not define ones through pci_bus
 * as we only allow accessing the PCI config space through the given PDO. */
#define DEFINE_PCI_READ_ROUTINE(length, type)				\
	int pci_read_config_##length(const struct pci_dev *dev,		\
				     int where, type *val)		\
	{								\
		if (pci_dev_is_disconnected(dev)) {			\
			PCI_SET_ERROR_RESPONSE(val);			\
			return PCIBIOS_DEVICE_NOT_FOUND;		\
		}							\
		return ntos_read_pci_config_space(pdev_to_pdo(dev),	\
						  where, val,		\
						  sizeof(*val));	\
	}								\
	EXPORT_SYMBOL(pci_read_config_##length);			\
	int pci_user_read_config_##length(struct pci_dev *dev,		\
					  int where, type *val)		\
	{								\
		return pci_read_config_##length(dev, where, val);	\
	}								\
	EXPORT_SYMBOL(pci_user_read_config_##length)

#define DEFINE_PCI_WRITE_ROUTINE(length, type)				\
	int pci_write_config_##length(const struct pci_dev *dev,	\
				      int where, type val)		\
	{								\
		if (pci_dev_is_disconnected(dev)) {			\
			return PCIBIOS_DEVICE_NOT_FOUND;		\
		}							\
		return ntos_write_pci_config_space(pdev_to_pdo(dev),	\
						   where, &val,		\
						   sizeof(val));	\
	}								\
	EXPORT_SYMBOL(pci_write_config_##length);			\
	int pci_user_write_config_##length(struct pci_dev *dev,		\
					   int where, type val)		\
	{								\
		return pci_write_config_##length(dev, where, val);	\
	}								\
	EXPORT_SYMBOL(pci_user_write_config_##length)

DEFINE_PCI_READ_ROUTINE(byte, u8);
DEFINE_PCI_READ_ROUTINE(word, u16);
DEFINE_PCI_READ_ROUTINE(dword, u32);
DEFINE_PCI_WRITE_ROUTINE(byte, u8);
DEFINE_PCI_WRITE_ROUTINE(word, u16);
DEFINE_PCI_WRITE_ROUTINE(dword, u32);

static u8 __pci_find_cap_start(struct pci_dev *dev)
{
	u16 status;

	pci_read_config_word(dev, PCI_STATUS, &status);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;

	switch (dev->hdr_type) {
	case PCI_HEADER_TYPE_NORMAL:
	case PCI_HEADER_TYPE_BRIDGE:
		return PCI_CAPABILITY_LIST;
	case PCI_HEADER_TYPE_CARDBUS:
		return PCI_CB_CAPABILITY_LIST;
	}

	return 0;
}

static u8 __pci_find_next_cap(struct pci_dev *dev, u8 pos, int cap)
{
	return PCI_FIND_NEXT_CAP(pci_read_config, pos, cap, NULL, dev);
}

u8 pci_find_capability(struct pci_dev *dev, int cap)
{
	u8 pos = __pci_find_cap_start(dev);
	if (pos)
		pos = __pci_find_next_cap(dev, pos, cap);

	return pos;
}
EXPORT_SYMBOL(pci_find_capability);

u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
	return __pci_find_next_cap(dev, pos + PCI_CAP_LIST_NEXT, cap);
}
EXPORT_SYMBOL_GPL(pci_find_next_capability);

u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 start, int cap)
{
	if (dev->cfg_size <= PCI_CFG_SPACE_SIZE)
		return 0;

	return PCI_FIND_NEXT_EXT_CAP(pci_read_config, start, cap, NULL, dev);
}
EXPORT_SYMBOL_GPL(pci_find_next_ext_capability);

int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u32 cap_mask)
{
	/* TODO: Call the PCI bus driver to enable atomic ops. This is used heavily
	 * by modern GPUs. The bus driver needs to traverse the path from the device
	 * to the root port to enable atomic ops for every bridge along the path, and
	 * finally turn it on for the device itself. */
	return -EINVAL;
}
EXPORT_SYMBOL(pci_enable_atomic_ops_to_root);

/**
 * pci_resize_resource - reconfigure a Resizable BAR and resources
 * @dev: the PCI device
 * @resno: index of the BAR to be resized
 * @size: new size as defined in the spec (0=1MB, 31=128TB)
 * @exclude_bars: a mask of BARs that should not be released
 *
 * Reconfigure @resno to @size and re-run resource assignment algorithm
 * with the new size.
 *
 * Prior to resize, release @dev resources that share a bridge window with
 * @resno.  This unpins the bridge window resource to allow changing it.
 *
 * The caller may prevent releasing a particular BAR by providing
 * @exclude_bars mask, but this may result in the resize operation failing
 * due to insufficient space.
 *
 * Return: 0 on success, or negative on error. In case of an error, the
 *         resources are restored to their original places.
 */
int pci_resize_resource(struct pci_dev *dev, int resno, int size,
			int exclude_bars)
{
	/* TODO: Call the bus driver to resize the bar. The bus driver will
	 * trigger a resource rebalance via IoInvalidateDeviceState and
	 * configure a new BAR size. */
	return -EINVAL;
}
EXPORT_SYMBOL(pci_resize_resource);

void pci_assign_unassigned_bus_resources(struct pci_bus *bus)
{
	/* TODO! */
}
EXPORT_SYMBOL_GPL(pci_assign_unassigned_bus_resources);
