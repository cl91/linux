/*
 * This header defines the interfaces used by the Linux kernel drivers to
 * communicate with the Neptune OS host process.
 */

#pragma once

#ifndef __ASSEMBLY__

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif

#include "ntlnxdrv.h"
#include <linux/errno.h>
#include <linux/mm.h>

/*
 * On i386, Linux assumes a 16-byte aligned stack (as opposed to the default
 * 4-byte alignment required by the ELF/PE ABI, which Windows assumes). We
 * therefore need to mark all ELF routines that may be called from the PE side
 * with force_align_arg_pointer on i386, so the compiler will generate code
 * to align the stack pointer by 16 bytes in their function prologues.
 */
#ifdef _M_IX86
#define REALIGN_STACK __attribute__((force_align_arg_pointer))
#else
#define REALIGN_STACK
#endif

#define PRESERVE_CURRENT_TASK(fcn_call)			\
	struct task_struct *saved_current = current;	\
	current = NULL;					\
	fcn_call;					\
	current = saved_current

#define CALL_NT_PAGED_CODE(fcn_call)					\
	({								\
		PRESERVE_CURRENT_TASK(int ret =				\
				      ntstatus_to_errno(fcn_call));	\
		ret;							\
	})

static inline int ntstatus_to_errno(IN NTSTATUS Status)
{
	switch (Status) {
	case STATUS_NO_MEMORY:
	case STATUS_INSUFFICIENT_RESOURCES:
		return -ENOMEM;
	case STATUS_INVALID_PARAMETER:
	case STATUS_INVALID_PARAMETER_1:
	case STATUS_INVALID_PARAMETER_2:
	case STATUS_INVALID_PARAMETER_3:
	case STATUS_INVALID_PARAMETER_4:
	case STATUS_INVALID_PARAMETER_5:
	case STATUS_INVALID_PARAMETER_6:
	case STATUS_INVALID_PARAMETER_7:
	case STATUS_INVALID_PARAMETER_8:
	case STATUS_INVALID_PARAMETER_9:
	case STATUS_INVALID_PARAMETER_10:
	case STATUS_INVALID_PARAMETER_11:
	case STATUS_INVALID_PARAMETER_12:
	case STATUS_OBJECT_NAME_INVALID:
	case STATUS_OBJECT_PATH_INVALID:
	case STATUS_OBJECT_PATH_SYNTAX_BAD:
		return -EINVAL;
	case STATUS_OBJECT_NAME_COLLISION:
	case STATUS_OBJECT_NAME_EXISTS:
		return -EEXIST;
	case STATUS_OBJECT_NAME_NOT_FOUND:
	case STATUS_OBJECT_PATH_NOT_FOUND:
		return -ENODEV;
	default:
		return NT_SUCCESS(Status) ? 0 : -EIO;
	}
}

static inline NTSTATUS errno_to_ntstatus(int ret)
{
	switch (ret) {
	case -EAGAIN:
		return STATUS_PENDING;
	case -EINVAL:
		return STATUS_INVALID_PARAMETER;
	case -EEXIST:
		return STATUS_OBJECT_NAME_EXISTS;
	case -EPERM:
	case -EACCES:
		return STATUS_ACCESS_DENIED;
	case -ENOENT:
		return STATUS_OBJECT_PATH_NOT_FOUND;
	case -ESRCH:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	case -EIO:
		return STATUS_IO_DEVICE_ERROR;
	case -ENOMEM:
		return STATUS_INSUFFICIENT_RESOURCES;
	case -ENODEV:
	case -ENXIO:
		return STATUS_NO_SUCH_DEVICE;
	case -EFAULT:
		return STATUS_INVALID_ADDRESS;
	case -ENOTDIR:
		return STATUS_NOT_A_DIRECTORY;
	case -EISDIR:
		return STATUS_FILE_IS_A_DIRECTORY;
	default:
		if (ret < 0) {
			return STATUS_UNSUCCESSFUL;
		}
		return STATUS_SUCCESS;
	}
}

extern LNX_DRV_IMPORT_TABLE LnxDrvImportTable;

/* Allocate 2^order pages, ie. the allocation size is 2^(order + PAGE_SHIFT). */
static inline int ntos_allocate_physical_memory(unsigned int order,
						unsigned long flags,
						void **va,
						unsigned long *pa)
{
	return ntstatus_to_errno(LnxDrvImportTable.AllocatePhysicalMemory(order, &flags,
									  va, (void *)pa));
}

/* order and va must match exactly those in ntos_allocate_physical_memory */
static inline void ntos_free_physical_memory(unsigned int order,
					     unsigned long flags,
					     void *va)
{
	LnxDrvImportTable.FreePhysicalMemory(order, flags, va);
}

static inline int ntos_map_physical_memory(unsigned long *pfn_db,
					   int pfn_count,
					   void **va)
{
	return ntstatus_to_errno(LnxDrvImportTable.MapPhysicalMemory(pfn_db, pfn_count, va));
}

static inline void *ntos_map_io_space(u64 phys,
				      unsigned long length,
				      LNXDRV_MEMORY_CACHING_TYPE attr)
{
	LARGE_INTEGER PhysicalAddress = { .QuadPart = phys };
	LARGE_INTEGER Length = { .QuadPart = length };
	return LnxDrvImportTable.MapIoSpace(&PhysicalAddress, &Length, attr);
}

static inline void ntos_unmap_io_space(void *va,
				       unsigned long length)
{
	LARGE_INTEGER Length = { .QuadPart = length };
	LnxDrvImportTable.UnmapIoSpace(va, &Length);
}

static inline int ntos_reserve_virtual_memory(unsigned long size,
					      unsigned int flags,
					      void **va)
{
	return ntstatus_to_errno(LnxDrvImportTable.ReserveVirtualMemory(size, flags, va));
}

static inline int ntos_commit_virtual_memory(void *va,
					     unsigned long size,
					     unsigned long pfndb_size,
					     unsigned long *pfndb)
{
	return ntstatus_to_errno(LnxDrvImportTable.CommitVirtualMemory(va, size,
								       pfndb_size, pfndb));
}

static inline void ntos_free_virtual_memory(const void *ptr,
					    unsigned long size, bool unreserve)
{
	LnxDrvImportTable.FreeVirtualMemory(ptr, size, unreserve);
}

static inline int ntos_allocate_virtual_memory(unsigned long size,
					       unsigned int flags,
					       void **va)
{
	size = PAGE_ALIGN(size);
	NTSTATUS status = LnxDrvImportTable.ReserveVirtualMemory(size, flags, va);
	if (!NT_SUCCESS(status)) {
		return ntstatus_to_errno(status);
	}
	status = LnxDrvImportTable.CommitVirtualMemory(*va, size, 0, NULL);
	if (!NT_SUCCESS(status)) {
		ntos_free_virtual_memory(*va, size, true);
		return ntstatus_to_errno(status);
	}
	return 0;
}

static inline void *ntos_allocate_pool(unsigned long size)
{
	return LnxDrvImportTable.AllocatePool(size);
}

static inline void ntos_free_pool(void *ptr)
{
	LnxDrvImportTable.FreePool(ptr);
}

static inline void ntos_get_system_ram_info(unsigned long *totalram_pages,
					    unsigned long *freeram_pages)
{
	LnxDrvImportTable.GetSystemRamInfo((void *)totalram_pages, (void *)freeram_pages);
}

static inline void ntos_initialize_softirq_dpc(PLNX_DPC_CALLBACK callback,
					       void *ctx)
{
	LnxDrvImportTable.InitializeSoftirqDpc(callback, ctx);
}

static inline void ntos_queue_softirq_dpc(void *arg1, void *arg2)
{
	LnxDrvImportTable.QueueSoftirqDpc(arg1, arg2);
}

static inline void *ntos_allocate_event(int wait_all)
{
	return LnxDrvImportTable.AllocateEvent(wait_all);
}

static inline void ntos_free_event(void *kevent)
{
	LnxDrvImportTable.FreeEvent(kevent);
}

static inline void ntos_set_event(void *kevent)
{
	LnxDrvImportTable.SetEvent(kevent);
}

static inline void ntos_clear_event(void *kevent)
{
	LnxDrvImportTable.ClearEvent(kevent);
}

static inline void ntos_wait_for_single_object(void *kevent, int alertable)
{
	PRESERVE_CURRENT_TASK(LnxDrvImportTable.WaitForSingleObject(kevent, alertable));
}

struct lnx_work_item_extension {
	void *io_workitem;
	void (*callback)(void *io_workitem, void *ctx);
	void *context;
};

static inline void *ntos_allocate_work_item(void)
{
	return LnxDrvImportTable.AllocateWorkItem(sizeof(struct lnx_work_item_extension));
}

static inline void ntos_free_work_item(void *io_workitem)
{
	LnxDrvImportTable.FreeWorkItem(io_workitem);
}

extern MS_ABI NTAPI VOID LnxWorkItemCallback(IN PVOID Unused,
					     IN PVOID Extension);

static inline void ntos_queue_work_item(void *io_workitem,
					void (*callback)(void *io_workitem,
							 void *ctx),
					void *ctx)
{
	struct lnx_work_item_extension *ext =
		LnxDrvImportTable.GetWorkItemExtension(io_workitem);
	ext->io_workitem = io_workitem;
	ext->callback = callback;
	ext->context = ctx;
	LnxDrvImportTable.QueueWorkItem(io_workitem, LnxWorkItemCallback);
}

static inline u64 ntos_get_tsc_frequency_in_mhz(void)
{
	return LnxDrvImportTable.GetTscFrequencyInMHz();
}

static inline u64 ntos_get_system_time(void)
{
	return LnxDrvImportTable.GetSystemTime();
}

static inline void ntos_set_global_timer(u64 delta,
					 PLNX_TIMER_CALLBACK callback, void *ctx)
{
	BUG_ON((s64)delta < 0);
	return LnxDrvImportTable.SetGlobalTimer((PVOID)&delta, callback, ctx);
}

static inline int ntos_register_module(const char *name, void *start, size_t size)
{
	BUG_ON(!name);
	BUG_ON(!start);
	BUG_ON(!size);
	return ntstatus_to_errno(LnxDrvImportTable.RegisterModule(name, start, size));
}

static inline int ntos_request_module(const char *alias_path, void *event, bool wait)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.RequestModule(alias_path, event, wait));
}

static inline int ntos_request_firmware(const char *name, void **data, size_t *size)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.RequestFirmware(name, data,
								    (SIZE_T *)size));
}

static inline void ntos_release_firmware(const void *data)
{
	LnxDrvImportTable.ReleaseFirmware(data);
}

static inline void __attribute((noreturn)) ntos_panic(void)
{
	LnxDrvImportTable.RaiseStatus(STATUS_DRIVER_INTERNAL_ERROR);
}

static inline void ntos_dbgprint(const char *str)
{
	LnxDrvImportTable.DbgPrint(str);
}

static inline int ntos_connect_interrupt(void **introbj, PLNX_ISR_CALLBACK callback,
					 void *ctx, int irq)
{
	return ntstatus_to_errno(LnxDrvImportTable.ConnectInterrupt(introbj, callback,
								    ctx, irq));
}

static inline void ntos_disconnect_interrupt(void *introbj)
{
	LnxDrvImportTable.DisconnectInterrupt(introbj);
}

static inline int ntos_create_device(void **nt_handle,
				     void *nt_driver_object_handle,
				     int devext_size,
				     const char *devname,
				     LNX_DEVICE_TYPE device_type,
				     bool exclusive)
{
	return ntstatus_to_errno(LnxDrvImportTable.CreateDevice(nt_handle,
								nt_driver_object_handle,
								devext_size,
								devname, device_type,
								exclusive));
}

static inline void *ntos_get_device_extension(void *dev_handle)
{
	return LnxDrvImportTable.GetDeviceExtension(dev_handle);
}

static inline int ntos_attach_device(void *src_dev,
				     void *target_dev,
				     void **previous_top)
{
	return ntstatus_to_errno(LnxDrvImportTable.AttachDevice(src_dev, target_dev,
								previous_top));
}

static inline void ntos_delete_device(void *nt_handle)
{
	LnxDrvImportTable.DeleteDevice(nt_handle);
}

static inline int ntos_get_device_slot_address(void *nt_handle,
					       unsigned int *busnr,
					       unsigned int *devfn)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.GetDeviceSlotAddress(nt_handle,
									 busnr,
									 devfn));
}

static inline int ntos_read_pci_config_space(void *pdo, int where,
					     void *val, int size)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.ReadPciConfig(pdo, where,
								  val, size));
}

static inline int ntos_write_pci_config_space(void *pdo, int where,
					      const void *val, int size)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.WritePciConfig(pdo, where,
								   val, size));
}

static inline void ntos_set_file_extension(void *file_object, void *file_extension)
{
	LnxDrvImportTable.SetFileExtension(file_object, file_extension);
}

static inline void *ntos_get_file_extension(void *file_object)
{
	return LnxDrvImportTable.GetFileExtension(file_object);
}

static inline const char *ntos_get_file_name(void *file_object)
{
	return LnxDrvImportTable.GetFileName(file_object);
}

static inline int ntos_is_file_readable(void *file_object)
{
	return LnxDrvImportTable.IsFileReadable(file_object);
}

static inline int ntos_is_file_writable(void *file_object)
{
	return LnxDrvImportTable.IsFileWritable(file_object);
}

static inline int ntos_forward_irp(void *device_object,
				   void *irp)
{
	return CALL_NT_PAGED_CODE(LnxDrvImportTable.ForwardIrp(device_object, irp));
}

static inline void ntos_complete_irp(void *irp,
				    int ret,
				    unsigned long info)
{
	LnxDrvImportTable.CompleteIrp(irp, errno_to_ntstatus(ret), info);
}

static inline void *ntos_get_irp_driver_context(void *irp)
{
	return LnxDrvImportTable.GetIrpDriverContext(irp);
}

static inline void ntos_set_irp_driver_context(void *irp, void *ctx)
{
	LnxDrvImportTable.SetIrpDriverContext(irp, ctx);
}

static inline void *ntos_irp_get_request_buffer(void *irp)
{
	return LnxDrvImportTable.IrpGetRequestBuffer(irp);
}

static inline unsigned int ntos_irp_get_request_length(void *irp)
{
	return LnxDrvImportTable.IrpGetRequestLength(irp);
}

static inline void ntos_register_bugcheck_callback(PLNX_BUGCHECK_CALLBACK callback)
{
	LnxDrvImportTable.RegisterBugcheckCallback(callback);
}

extern int ntos_register_framebuffer(void *vaddr,
				     size_t size,
				     int offset,
				     int width,
				     int height,
				     int pitch,
				     char bits_per_pixel,
				     char blue_index,
				     char green_index,
				     char red_index,
				     bool need_flush);

static inline int ntos_unregister_framebuffer(void *va)
{
	return ntstatus_to_errno(LnxDrvImportTable.UnregisterFramebuffer(va));
}

typedef void (*ntos_fb_damage_handler_t)(void *virt_base,
					 int start_width,
					 int start_height,
					 int end_width,
					 int end_height);
extern void ntos_register_framebuffer_damage_handler(ntos_fb_damage_handler_t f);

static inline void *ntos_get_current_tib(void)
{
	return LnxDrvImportTable.GetCurrentTib();
}

static inline bool ntos_is_isr_thread(void)
{
	return LnxDrvImportTable.IsIsrThread();
}

static inline bool ntos_is_dpc_thread(void)
{
	return LnxDrvImportTable.IsDpcThread();
}

int lnxdrv_init(void);
int lnxdrv_init_driver(void *driver_object_handle, void *kevent);
int lnxdrv_add_module(void *view_base, size_t view_size, const char *args,
		      void *kevent);
int lnxdrv_register_user_buffer(void *va,
				unsigned long size,
				unsigned long pfndb_size,
				unsigned long *pfndb);
int lnxdrv_unregister_user_buffer(void *va,
				  unsigned long size);
int lnxdrv_add_device(void *driver_object, void *kevent,
		      void *pdo, const char *dev_inst_path);
int lnxdrv_start_device(void *irp, void *driver_object, void *kevent,
			unsigned int res_count,
			PLNXDRV_RESOURCE resources);
int lnxdrv_dispatch_create(void *irp, void *device_object, void *kevent,
			   void *file_object);
int lnxdrv_dispatch_readwrite(void *irp, void *device_object, void *kevent,
			      void *file_object, u64 file_offset,
			      void *buffer, unsigned int buffer_length,
			      unsigned long *pfn_db, unsigned int pfn_count,
			      int write, unsigned int *result_length);
int lnxdrv_dispatch_cleanup(void *irp, void *device_object, void *kevent,
			    void *file_object);

#endif	/* __ASSEMBLY__ */
