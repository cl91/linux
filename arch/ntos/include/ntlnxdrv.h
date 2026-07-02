/*++

  Copyright (c) 2026  Dr. Chang Liu, PhD.

  Module Name:

  ntlnxdrv.h

  Abstract:

  This header file defines the interfaces shared between the PE sub-component of
  a linkable extension driver (lnxdrv) and its ELF sub-component. A lnxdrv (a name
  deliberately chosen that evokes the connotation of a "Linux driver") contains
  two sub-components: a PE part that is a standard NT driver speaking the standard
  NT driver interface, and an ELF part that is an ELF executable loaded into the
  process of the PE driver object and communicates with the PE driver object
  through a standard interface, defined in this header. The term "linkable" refers
  to the fact that what we are implementing is effectively a form of runtime
  linking of an ELF object with a PE object. When the ELF object is loaded into
  the process of the PE driver object, the PE code supplies the entry point of
  the ELF executable with an "import table", containing function pointers that
  the ELF object can call. The ELF entry point in turn returns an "export table"
  containing function pointers for the PE object to call. Due to ABI differences
  between PE and ELF, the function pointers in the import and export tables must
  be marked with an appropriate ABI attribute (MS_ABI or ELF_ABI), so the compiler
  can use the correct calling conventions when calling these function pointers.
  (This is essentially the same technique that Microsoft calls "thunking", which
  is used, for instance, in the Win16 subsystem of NT to call 32-bit routines
  from 16-bit code.) Likewise, all callback routines that cross the PE/ELF border
  must also be appropriately marked with an ABI attribute.

  The ELF part of the lnxdrv should include this master header and should NOT
  include the lnxdrv.h master header, which is for the PE part of the driver to
  include.

  Revision History:

  2026-01-05  File created
  2026-03-09  Moved into the Linux kernel repository
*/

#pragma once

#if !defined(_NTDDK_) && !defined(__ASSEMBLY__)
#include "ntlnxdef.h"
#endif

#ifdef __i386__
#define LARGE_PAGE_SHIFT	22
#elif defined(__x86_64__)
#define LARGE_PAGE_SHIFT	21
#elif defined(__aarch64__)
#define LARGE_PAGE_SHIFT	21
#else
#error "Unsupported architecture"
#endif
#define LARGE_PAGE_SIZE		(1ULL << LARGE_PAGE_SHIFT)

/*
 * The kernel image (vmlinux) is mapped at a fixed address so we do not have to
 * do any relocation fixups for the kernel image. The Linux kernel modules are
 * mapped in the address range defined below, which must be within 2GB of the
 * kernel image base address, in order for PC-relative addressing to work.
 */
#define KERNEL_IMAGE_START	0x18000000
#define MODULES_VADDR		0x20000000
#define MODULES_LEN		0x10000000
#define MODULES_END		(MODULES_VADDR + MODULES_LEN)

/*
 * Structure of the page frame database.
 *
 * ULONG_PTR PfnEntry;
 * |==============================================================|
 * | STARTING PHYSICAL PAGE FRAME NUMBER | PAGE COUNT | ATTR BITS |
 * |--------------------------------------------------------------|
 * | 31/63 .......................... 12 | 11 ..... 3 |  2  1  0  |
 * |==============================================================|
 *                                                       ^  ^  ^
 *                                                       |__|  |
 *                                           Cache attributes  Page size
 * If bit 0 is set, all pages frames in this pfn entry are large pages
 * (second lowest level of page size offered by the architecture).
 * Otherwise they are all pages with the lowest level of page size.
 *
 * Bit 1 and 2 encode the caching attributes:
 *   0 0 --- Cached
 *   0 1 --- Write Combine
 *   1 0 --- Write Through
 *   1 1 --- Uncached
 *
 * The page count bits encode the number of pages for this PFN entry.
 * Note since the number of pages always start from one, an all-zero
 * page count bits represent one page, and 0xff represent 256 pages.
 * In other words, the number of pages is the page count bits plus one.
 */
#define LNXDRV_PFN_ATTR_BITS		(3)
#define LNXDRV_PFN_PAGE_COUNT_BITS	(9)
#define LNXDRV_PFN_ATTR_LARGE_PAGE	(0x1ULL)
#define LNXDRV_PFN_PAGE_COUNT_MASK	((1ULL << LNXDRV_PFN_PAGE_COUNT_BITS) - 1)
#define LNXDRV_PFN_ATTR_CACHED		(0)
#define LNXDRV_PFN_ATTR_WC		(1)
#define LNXDRV_PFN_ATTR_WT		(2)
#define LNXDRV_PFN_ATTR_UNCACHED	(3)

#define LNXDRV_PFN_PAGE_SIZE(Pfn)					\
	(((Pfn) & LNXDRV_PFN_ATTR_LARGE_PAGE) ? LARGE_PAGE_SIZE : PAGE_SIZE)
#define LNXDRV_PFN_PAGE_SHIFT(Pfn)					\
	(((Pfn) & LNXDRV_PFN_ATTR_LARGE_PAGE) ? LARGE_PAGE_SHIFT : PAGE_SHIFT)
#define LNXDRV_PFN_PAGE_COUNT(Pfn)					\
	((((Pfn) >> LNXDRV_PFN_ATTR_BITS) & LNXDRV_PFN_PAGE_COUNT_MASK) + 1)
#define LNXDRV_PFN_CACHE_ATTR(Pfn)	(((Pfn) >> 1) & 3)
#define LNXDRV_PFN_PAGE_ADDRESS(Pfn)	(((Pfn) >> PAGE_SHIFT) << PAGE_SHIFT)
#define LNXDRV_PFN_APPLY_CACHED_ATTR(Pfn)			\
    (((Pfn) & ~(3ULL << 1)) | (LNXDRV_PFN_ATTR_CACHED << 1))
#define LNXDRV_PFN_APPLY_WC_ATTR(Pfn)				\
    (((Pfn) & ~(3ULL << 1)) | (LNXDRV_PFN_ATTR_WC << 1))
#define LNXDRV_PFN_APPLY_WT_ATTR(Pfn)				\
    (((Pfn) & ~(3ULL << 1)) | (LNXDRV_PFN_ATTR_WT << 1))
#define LNXDRV_PFN_APPLY_UNCACHED_ATTR(Pfn)			\
    (((Pfn) & ~(3ULL << 1)) | (LNXDRV_PFN_ATTR_UNCACHED << 1))
#define LNXDRV_PFN_APPLY_CACHE_ATTR(Pfn, Attr)			\
    (((Pfn) & ~(3ULL << 1)) | ((Attr) << 1))

#ifndef __ASSEMBLY__

typedef enum _LNXDRV_MEMORY_CACHING_TYPE {
	LnxDrvMemNonCached,
	LnxDrvMemCached,
	LnxDrvMemWriteCombined,
	LnxDrvMemWriteThrough = 6
} LNXDRV_MEMORY_CACHING_TYPE;

FORCEINLINE ULONG_PTR LnxDrvCacheTypeToPfnAttr(IN LNXDRV_MEMORY_CACHING_TYPE CacheType)
{
    switch (CacheType) {
    case LnxDrvMemCached:
	return LNXDRV_PFN_ATTR_CACHED;
    case LnxDrvMemWriteCombined:
	return LNXDRV_PFN_ATTR_WC;
    case LnxDrvMemWriteThrough:
	return LNXDRV_PFN_ATTR_WT;
    default:
	return LNXDRV_PFN_ATTR_UNCACHED;
    }
}

#define LNXDRV_FORM_PFN(Addr, NumPages, CacheType, LargePage)		\
	(LNXDRV_PFN_APPLY_CACHE_ATTR((Addr) & ~((ULONG_PTR)PAGE_SIZE - 1) | \
				     (((NumPages) - 1) << LNXDRV_PFN_ATTR_BITS), \
				     LnxDrvCacheTypeToPfnAttr(CacheType)) | \
	 ((LargePage) ? 1 : 0))

/*
 * Device types supported by LNXDRV
 */
typedef enum _LNXDRV_DEVICE_TYPE {
	LnxCharDev,
	LnxNetDev,
	LnxMaxDevType
} LNX_DEVICE_TYPE;

/*
 * IO resources
 */
typedef enum _LXNDRV_RESOURCE_TYPE {
	LnxResBusNumber,
	LnxResIoPort,
	LnxResMemory,
	LnxResInterrupt
} LNXDRV_RESOURCE_TYPE;

typedef struct _LNXDRV_RESOURCE_BUS_NUMBER {
	ULONG Start;
	ULONG Length;
} LNXDRV_RESOURCE_BUS_NUMBER, *PLNXDRV_RESOURCE_BUS_NUMBER;

typedef struct _LNXDRV_RESOURCE_IO_RANGE {
	ULONG64 Start;
	ULONG64 Length;
	ULONG Index;
	ULONG Padding;
} LNXDRV_RESOURCE_IO_RANGE, *PLNXDRV_RESOURCE_IO_RANGE;

typedef enum _LNXDRV_INTERRUPT_TYPE {
	LnxDrvInterruptTypeLegacy,
	LnxDrvInterruptTypeMsi,
	LnxDrvInterruptTypeMsiX
} LNXDRV_INTERRUPT_TYPE;

typedef struct _LNXDRV_RESOURCE_INTERRUPT {
	ULONG Irq; /* Starting IRQ number if multiple IRQ is assigned */
	ULONG Count;
	LNXDRV_INTERRUPT_TYPE Type;
	ULONG Padding;
} LNXDRV_RESOURCE_INTERRUPT, *PLNXDRV_RESOURCE_INTERRUPT;

typedef struct _LXNDRV_RESOURCE {
	LNXDRV_RESOURCE_TYPE Type;
	ULONG Padding;
	union {
		LNXDRV_RESOURCE_BUS_NUMBER BusNumber;
		LNXDRV_RESOURCE_IO_RANGE IoRange;
		LNXDRV_RESOURCE_INTERRUPT Interrupt;
	};
} LNXDRV_RESOURCE, *PLNXDRV_RESOURCE;

#ifdef _M_AMD64
#define MS_ABI __attribute__((ms_abi))
#define ELF_ABI __attribute__((sysv_abi))
#else
/* On i386, there is largely no difference between the default calling conventions
 * used by the PE and the ELF ABI, assuming we never pass arguments larger than 32
 * bits. Since clang does not support the ms_abi attribute on i686-unknown-linux
 * or the sysv_abi attribute on i686-pc-windows, the routines below marked with the
 * MS_ABI or the ELF_ABI attribute cannot have ULONG64 or struct parameters.
 *
 * On arm64, PE and ELF share the same ABI so we are safe. */
#define ELF_ABI
#define MS_ABI
#endif

typedef VOID (MS_ABI NTAPI *PLNX_DPC_CALLBACK)(IN PVOID Dpc,
					       IN OPTIONAL PVOID DeferredContext,
					       IN OPTIONAL PVOID SystemArgument1,
					       IN OPTIONAL PVOID SystemArgument2);

typedef VOID (MS_ABI NTAPI *PLNX_WORKITEM_CALLBACK)(PVOID Unused,
						    PVOID Extension);

typedef VOID (MS_ABI *PLNX_TIMER_CALLBACK)(PVOID Context);

typedef BOOLEAN (MS_ABI NTAPI *PLNX_ISR_CALLBACK)(IN PVOID InterruptObject,
						  IN PVOID ServiceContext);

typedef VOID (ELF_ABI *PLNX_BUGCHECK_CALLBACK)(PCSTR BugcheckMsg);

typedef struct _LNX_DRV_IMPORT_TABLE {
	VOID (MS_ABI *DbgPrint)(IN PCSTR String);
	NTSTATUS (MS_ABI *AllocatePhysicalMemory)(IN ULONG Order,
						  IN OUT PULONG_PTR Flags,
						  OUT PVOID *VirtAddr,
						  OUT ULONG_PTR *PhyAddr);
	VOID (MS_ABI *FreePhysicalMemory)(IN ULONG Order,
					  IN ULONG_PTR Flags,
					  IN PVOID VirtAddr);
	NTSTATUS (MS_ABI *MapPhysicalMemory)(IN PULONG_PTR PfnDb,
					     IN ULONG PfnCount,
					     OUT PVOID *VirtBase);
	PVOID (MS_ABI *MapIoSpace)(IN PLARGE_INTEGER PhysicalAddress,
				   IN PLARGE_INTEGER Length,
				   IN LNXDRV_MEMORY_CACHING_TYPE PageAttribute);
	VOID (MS_ABI *UnmapIoSpace)(IN PVOID BaseAddress,
				    IN PLARGE_INTEGER Length);
	NTSTATUS (MS_ABI *ReserveVirtualMemory)(IN SIZE_T Size,
						IN ULONG_PTR Flags,
						OUT PVOID *VirtAddr);
	NTSTATUS (MS_ABI *CommitVirtualMemory)(IN PVOID VirtAddr,
					       IN SIZE_T Size,
					       IN OPTIONAL SIZE_T PfndbSize,
					       OUT OPTIONAL ULONG_PTR *Pfndb);
	VOID (MS_ABI *FreeVirtualMemory)(IN PCVOID Ptr,
					 IN SIZE_T Size,
					 IN BOOLEAN Unreserve);
	PVOID (MS_ABI *AllocatePool)(IN SIZE_T Size);
	VOID (MS_ABI *FreePool)(IN PCVOID Ptr);
	VOID (MS_ABI *GetSystemRamInfo)(OUT ULONG_PTR *TotalRam, OUT ULONG_PTR *FreeRam);
	VOID (MS_ABI *InitializeSoftirqDpc)(IN PLNX_DPC_CALLBACK Callback,
					    IN PVOID Context);
	VOID (MS_ABI *QueueSoftirqDpc)(IN PVOID Arg1, IN PVOID Arg2);
	PVOID (MS_ABI *AllocateEvent)(IN BOOLEAN WaitAll);
	VOID (MS_ABI *FreeEvent)(IN PVOID Event);
	VOID (MS_ABI *SetEvent)(IN PVOID Event);
	VOID (MS_ABI *ClearEvent)(IN PVOID Event);
	VOID (MS_ABI *WaitForSingleObject)(IN PVOID Event, IN BOOLEAN Alertable);
	PVOID (MS_ABI *AllocateWorkItem)(IN ULONG ExtensionSize);
	VOID (MS_ABI *FreeWorkItem)(IN PVOID IoWorkItem);
	PVOID (MS_ABI *GetWorkItemExtension)(IN PVOID IoWorkItem);
	VOID (MS_ABI *QueueWorkItem)(IN PVOID IoWorkItem,
				     IN PLNX_WORKITEM_CALLBACK Callback);
	NTSTATUS (MS_ABI *ConnectInterrupt)(OUT PVOID *InterruptObject,
					    IN PLNX_ISR_CALLBACK Callback,
					    IN PVOID Context,
					    IN ULONG Irq);
	VOID (MS_ABI *DisconnectInterrupt)(IN PVOID InterruptObject);
	NTSTATUS (MS_ABI *CreateDevice)(OUT PVOID *Handle,
					IN PVOID DriverObjectHandle,
					IN ULONG DevExtSize,
					IN PCSTR DevName,
					IN LNX_DEVICE_TYPE DeviceType,
					IN BOOLEAN Exclusive);
	NTSTATUS (MS_ABI *AttachDevice)(IN PVOID SourceDevice,
					IN PVOID TargetDevice,
					OUT PVOID *PreviousTopDevice);
	NTSTATUS (MS_ABI *GetDeviceSlotAddress)(IN PVOID DeviceObject,
						OUT ULONG *BusNumber,
						OUT ULONG *SlotId);
	VOID (MS_ABI *DeleteDevice)(IN PVOID Handle);
	PVOID (MS_ABI *GetDeviceExtension)(IN PVOID Handle);
	NTSTATUS (MS_ABI *ReadPciConfig)(IN PVOID Pdo,
					 IN ULONG Offset,
					 OUT PVOID Buffer,
					 IN ULONG Size);
	NTSTATUS (MS_ABI *WritePciConfig)(IN PVOID Pdo,
					  IN ULONG Offset,
					  IN PCVOID Buffer,
					  IN ULONG Size);
#if defined(__i386__) || defined (__x86_64__)
	UCHAR (MS_ABI *ReadIoPort8)(IN USHORT PortNum);
	USHORT (MS_ABI *ReadIoPort16)(IN USHORT PortNum);
	ULONG (MS_ABI *ReadIoPort32)(IN USHORT PortNum);
	VOID (MS_ABI *WriteIoPort8)(IN USHORT PortNum, IN UCHAR Value);
	VOID (MS_ABI *WriteIoPort16)(IN USHORT PortNum, IN USHORT Value);
	VOID (MS_ABI *WriteIoPort32)(IN USHORT PortNum, IN ULONG Value);
#endif
	VOID (MS_ABI *SetFileExtension)(IN PVOID FileObject, IN PVOID FileExtension);
	PVOID (MS_ABI *GetFileExtension)(IN PVOID FileObject);
	PCSTR (MS_ABI *GetFileName)(IN PVOID FileObject);
	ULONG (MS_ABI *IsFileReadable)(IN PVOID FileObject);
	ULONG (MS_ABI *IsFileWritable)(IN PVOID FileObject);
	NTSTATUS (MS_ABI *ForwardIrp)(IN PVOID DeviceObject,
				      IN PVOID Irp);
	VOID (MS_ABI *CompleteIrp)(IN PVOID Irp,
				   IN NTSTATUS Status,
				   IN ULONG_PTR Information);
	PVOID (MS_ABI *GetIrpDriverContext)(IN PVOID Irp);
	VOID (MS_ABI *SetIrpDriverContext)(IN PVOID Irp, IN PVOID Ctx);
	PVOID (MS_ABI *IrpGetRequestBuffer)(IN PVOID Irp);
	ULONG (MS_ABI *IrpGetRequestLength)(IN PVOID Irp);
	NTSTATUS (MS_ABI *RegisterModule)(IN PCSTR Name,
					  IN PVOID StartAddr,
					  IN SIZE_T Size);
	NTSTATUS (MS_ABI *RequestModule)(IN PCSTR AliasPath,
					 IN PVOID Event,
					 IN BOOLEAN Wait);
	NTSTATUS (MS_ABI *RequestFirmware)(IN PCSTR Name,
					   OUT PVOID *Data,
					   OUT SIZE_T *Size);
	VOID (MS_ABI *ReleaseFirmware)(IN PCVOID Data);
	/* Returning ULONG64 is fine since ELF and PE both use EDX:EAX to return
	 * 64-bit values on i386. */
	ULONG64 (MS_ABI *GetTscFrequencyInMHz)(VOID);
	ULONG64 (MS_ABI *GetSystemTime)(VOID);
	VOID (MS_ABI *SetGlobalTimer)(IN PLARGE_INTEGER DeltaIn100Ns,
				      IN PLNX_TIMER_CALLBACK Callback,
				      IN PVOID Context);
	VOID (MS_ABI *RegisterBugcheckCallback)(IN PLNX_BUGCHECK_CALLBACK Callback);
	NTSTATUS (MS_ABI *RegisterFramebuffer)(IN PVOID VirtBase,
					       IN SIZE_T Size,
					       IN ULONG Offset,
					       IN ULONG Width,
					       IN ULONG Height,
					       IN ULONG Pitch,
					       IN UCHAR BitsPerPixel,
					       IN UCHAR BlueIndex,
					       IN UCHAR GreenIndex,
					       IN UCHAR RedIndex,
					       IN BOOLEAN NeedFlush);
	NTSTATUS (MS_ABI *UnregisterFramebuffer)(IN PVOID VirtBase);
	PVOID (MS_ABI *GetCurrentTib)(VOID);
	BOOLEAN (MS_ABI *IsIsrThread)(VOID);
	BOOLEAN (MS_ABI *IsDpcThread)(VOID);
	VOID (MS_ABI __attribute((noreturn)) *RaiseStatus)(IN NTSTATUS Status);
} LNX_DRV_IMPORT_TABLE, *PLNX_DRV_IMPORT_TABLE;

typedef struct _LNX_DRV_EXPORT_TABLE {
	NTSTATUS (ELF_ABI *InitializeDriver)(IN PVOID DriverObjectHandle,
					     IN PVOID Event);
	NTSTATUS (ELF_ABI *AddModule)(IN PVOID ViewBase,
				      IN SIZE_T ViewSize,
				      IN PCSTR Args,
				      IN PVOID Event);
	NTSTATUS (ELF_ABI *AddDevice)(IN PVOID DriverObject,
				      IN PVOID Event,
				      IN PVOID Pdo,
				      IN PCSTR DeviceInstancePath);
	NTSTATUS (ELF_ABI *StartDevice)(IN PVOID Irp,
					IN PVOID DeviceObject,
					IN PVOID Event,
					IN ULONG ResourceCount,
					IN PLNXDRV_RESOURCE Resources);
	NTSTATUS (ELF_ABI *DispatchCreate)(IN PVOID Irp,
					   IN PVOID DeviceObject,
					   IN PVOID Event,
					   IN PVOID FileObject);
	NTSTATUS (ELF_ABI *DispatchReadWrite)(IN PVOID Irp,
					      IN PVOID DeviceObject,
					      IN PVOID Event,
					      IN PVOID FileObject,
					      IN PLARGE_INTEGER FileOffset,
					      IN OUT PVOID Buffer,
					      IN OUT ULONG BufferLength,
					      IN PULONG_PTR PfnDb,
					      IN ULONG PfnCount,
					      IN BOOLEAN Write,
					      OUT ULONG *ResultLength);
	NTSTATUS (ELF_ABI *DispatchCleanup)(IN PVOID Irp,
					    IN PVOID DeviceObject,
					    IN PVOID Event,
					    IN PVOID FileObject);
	VOID (ELF_ABI *HandleFrameBufferDamage)(IN PVOID Event,
						IN PVOID VirtBase,
						IN ULONG StartWidth,
						IN ULONG StartHeight,
						IN ULONG EndWidth,
						IN ULONG EndHeight);
} LNX_DRV_EXPORT_TABLE, *PLNX_DRV_EXPORT_TABLE;

typedef NTSTATUS (ELF_ABI LNX_DRV_ENTRY_POINT)(IN PLNX_DRV_IMPORT_TABLE ImportTable,
					       OUT PLNX_DRV_EXPORT_TABLE ExportTable);
typedef LNX_DRV_ENTRY_POINT *PLNX_DRV_ENTRY_POINT;

#endif	/* __ASSEMBLY__ */
