/*
 * This file defines the entry point of the final vmlinux ELF image as well as
 * the export functions that the vmlinux image provides to the PE driver object.
 */

#include <linux/init.h>
#include <host_ops.h>
#include <init.h>

LNX_DRV_IMPORT_TABLE LnxDrvImportTable;
EXPORT_SYMBOL(LnxDrvImportTable);

static REALIGN_STACK NTSTATUS __init LnxDrvInitialize(IN PVOID DriverObjectHandle,
						      IN PVOID Event)
{
	int ret = lnxdrv_init_driver(DriverObjectHandle, Event);
	return ret ? STATUS_DRIVER_UNABLE_TO_LOAD : STATUS_SUCCESS;
}

static REALIGN_STACK NTSTATUS LnxDrvAddModule(IN PVOID ViewBase,
					      IN SIZE_T ViewSize,
					      IN PCSTR Args,
					      IN PVOID Event)
{
	return errno_to_ntstatus(lnxdrv_add_module(ViewBase, ViewSize, Args, Event));
}

static REALIGN_STACK NTSTATUS LnxDrvAddDevice(IN PVOID DriverObject,
					      IN PVOID Event,
					      IN PVOID Pdo,
					      IN PCSTR DeviceInstancePath)
{
	return errno_to_ntstatus(lnxdrv_add_device(DriverObject, Event,
						   Pdo, DeviceInstancePath));
}

static REALIGN_STACK NTSTATUS LnxDrvStartDevice(IN PVOID Irp,
						IN PVOID DriverObject,
						IN PVOID Event,
						IN ULONG ResourceCount,
						IN PLNXDRV_RESOURCE Resources)
{
	return errno_to_ntstatus(lnxdrv_start_device(Irp, DriverObject, Event,
						     ResourceCount, Resources));
}

static REALIGN_STACK NTSTATUS LnxDrvDispatchCreate(IN PVOID Irp,
						   IN PVOID DeviceObject,
						   IN PVOID Event,
						   IN PVOID FileObject)
{
	return errno_to_ntstatus(lnxdrv_dispatch_create(Irp, DeviceObject, Event, FileObject));
}

static REALIGN_STACK NTSTATUS LnxDrvDispatchReadWrite(IN PVOID Irp,
						      IN PVOID DeviceObject,
						      IN PVOID Event,
						      IN PVOID FileObject,
						      IN PLARGE_INTEGER FileOffset,
						      IN OUT PVOID Buffer,
						      IN OUT ULONG BufferLength,
						      IN PULONG_PTR PfnDb,
						      IN ULONG PfnCount,
						      IN BOOLEAN Write,
						      OUT ULONG *ResultLength)
{
	return errno_to_ntstatus(lnxdrv_dispatch_readwrite(Irp, DeviceObject, Event, FileObject,
							   FileOffset->QuadPart,
							   Buffer, BufferLength,
							   (unsigned long *)PfnDb, PfnCount,
							   Write, ResultLength));
}

static REALIGN_STACK NTSTATUS LnxDrvDispatchCleanup(IN PVOID Irp,
						    IN PVOID DeviceObject,
						    IN PVOID Event,
						    IN PVOID FileObject)
{
	return errno_to_ntstatus(lnxdrv_dispatch_cleanup(Irp, DeviceObject, Event, FileObject));
}

static ntos_fb_damage_handler_t fb_damage_handler;
void ntos_register_framebuffer_damage_handler(ntos_fb_damage_handler_t f)
{
	fb_damage_handler = f;
}
EXPORT_SYMBOL(ntos_register_framebuffer_damage_handler);

static REALIGN_STACK VOID LnxDrvHandleFrameBufferDamage(IN PVOID Event,
							IN PVOID VirtBase,
							IN ULONG StartWidth,
							IN ULONG StartHeight,
							IN ULONG EndWidth,
							IN ULONG EndHeight)
{
	KERNEL_THREAD_ENTER(Event);
	if (fb_damage_handler) {
		fb_damage_handler(VirtBase, StartWidth, StartHeight, EndWidth, EndHeight);
	}
	KERNEL_THREAD_EXIT;
}

static LNX_DRV_EXPORT_TABLE LnxDrvExportTable = {
	.InitializeDriver = LnxDrvInitialize,
	.AddModule = LnxDrvAddModule,
	.AddDevice = LnxDrvAddDevice,
	.StartDevice = LnxDrvStartDevice,
	.DispatchCreate = LnxDrvDispatchCreate,
	.DispatchReadWrite = LnxDrvDispatchReadWrite,
	.DispatchCleanup = LnxDrvDispatchCleanup,
	.HandleFrameBufferDamage = LnxDrvHandleFrameBufferDamage
};

LNX_DRV_ENTRY_POINT LnxDriverEntry;
NTSTATUS __init LnxDriverEntry(IN PLNX_DRV_IMPORT_TABLE ImportTable,
			       OUT PLNX_DRV_EXPORT_TABLE ExportTable)
{
	LnxDrvImportTable = *ImportTable;
	*ExportTable = LnxDrvExportTable;
	lnxdrv_init();
	return STATUS_SUCCESS;
}

MS_ABI NTAPI REALIGN_STACK VOID LnxWorkItemCallback(IN PVOID Unused,
						    IN PVOID Extension)
{
	struct lnx_work_item_extension *ext = Extension;
	ext->callback(ext->io_workitem, ext->context);
}
