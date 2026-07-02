#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/dev_printk.h>
#include <host_ops.h>

static int _request_firmware(const struct firmware **firmware_p, const char *name,
			     struct device *device, bool nowarn)
{
	if (!firmware_p)
		return -EINVAL;

	if (!name || name[0] == '\0') {
		return -EINVAL;
	}

	struct firmware *firmware = kzalloc_obj(*firmware);
	if (!firmware) {
		dev_err(device, "%s: kmalloc(struct firmware) failed\n",
			__func__);
		return -ENOMEM;
	}

	int ret = ntos_request_firmware(name, (void *)&firmware->data, &firmware->size);
	if (ret) {
		kfree(firmware);
		return ret;
	}
	*firmware_p = firmware;
	return 0;
}

/**
 * request_firmware() - send firmware request and wait for it
 * @firmware_p: pointer to firmware image
 * @name: name of firmware file
 * @device: device for which firmware is being loaded
 **/
int request_firmware(const struct firmware **firmware_p, const char *name,
		     struct device *device)
{
	return _request_firmware(firmware_p, name, device, false);
}
EXPORT_SYMBOL(request_firmware);

/**
 * firmware_request_nowarn() - request for an optional fw module
 * @firmware: pointer to firmware image
 * @name: name of firmware file
 * @device: device for which firmware is being loaded
 *
 * This function is similar in behaviour to request_firmware(), except it
 * doesn't produce warning messages when the file is not found. The sysfs
 * fallback mechanism is enabled if direct filesystem lookup fails. However,
 * failures to find the firmware file with it are still suppressed. It is
 * therefore up to the driver to check for the return value of this call and to
 * decide when to inform the users of errors.
 **/
int firmware_request_nowarn(const struct firmware **firmware, const char *name,
			    struct device *device)
{
	return _request_firmware(firmware, name, device, true);
}
EXPORT_SYMBOL_GPL(firmware_request_nowarn);

/**
 * request_firmware_direct() - load firmware directly without usermode helper
 * @firmware_p: pointer to firmware image
 * @name: name of firmware file
 * @device: device for which firmware is being loaded
 *
 * This function works pretty much like request_firmware(), but this doesn't
 * fall back to usermode helper even if the firmware couldn't be loaded
 * directly from fs.  Hence it's useful for loading optional firmwares, which
 * aren't always present, without extra long timeouts of udev.
 **/
int request_firmware_direct(const struct firmware **firmware_p,
                            const char *name, struct device *device)
{
	return firmware_request_nowarn(firmware_p, name, device);
}
EXPORT_SYMBOL_GPL(request_firmware_direct);

/**
 * release_firmware() - release the resource associated with a firmware image
 * @fw: firmware resource to release
 **/
void release_firmware(const struct firmware *fw)
{
	if (fw) {
		if (fw->data) {
			ntos_release_firmware(fw->data);
		}
		kfree(fw);
	}
}
EXPORT_SYMBOL(release_firmware);
