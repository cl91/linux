/*
 * Minimal header file for the NT definitions used by LNXDRV.
 */

#pragma once

#ifdef __i386__
#ifndef _M_IX86
#define _M_IX86
#endif
#endif

#ifdef __x86_64__
#ifndef _M_AMD64
#define _M_AMD64
#define _WIN64
#endif
#endif

#ifdef __aarch64__
#ifndef _M_ARM64
#define _M_ARM64
#define _WIN64
#endif
#endif

#ifdef _M_IX86
#define NTAPI __stdcall
#else
#define NTAPI
#endif

#define C_ASSERT(x, msg)	_Static_assert(x, msg)

#define FORCEINLINE		static inline __attribute__((always_inline))

#define IN
#define OUT
#define OPTIONAL

#define CONST const
#define VOID void
typedef void *PVOID, **PPVOID;
typedef CONST VOID *PCVOID;

typedef char CHAR;
typedef unsigned char UCHAR;
typedef CHAR *PCHAR;
typedef UCHAR *PUCHAR;
typedef CONST CHAR *PCSTR;

typedef _Bool BOOLEAN;
typedef BOOLEAN *PBOOLEAN;
/* TRUE and FALSE are defined in include/acpi/actypes.h */

typedef unsigned short USHORT, *PUSHORT;

typedef int LONG, *PLONG;
typedef unsigned int ULONG, *PULONG;

typedef unsigned long long ULONGLONG, *PULONGLONG, ULONG64, *PULONG64;
typedef long long LONGLONG, *PLONGLONG, LONG64, *PLONG64;

typedef unsigned long ULONG_PTR, SIZE_T, *PSIZE_T, *PULONG_PTR;
typedef long LONG_PTR, SSIZE_T, *PSSIZE_T, *PLONG_PTR;

C_ASSERT(sizeof(ULONG_PTR) == sizeof(PVOID), "This file cannot be included for PE targets");

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef LONG NTSTATUS;
typedef NTSTATUS *PNTSTATUS;

#define NT_SUCCESS(Status)	(((NTSTATUS)(Status)) >= 0)

#define STATUS_SUCCESS                          ((NTSTATUS)0x00000000UL)
#define STATUS_PENDING                          ((NTSTATUS)0x00000103UL)
#define STATUS_OBJECT_NAME_EXISTS               ((NTSTATUS)0x40000000UL)
#define STATUS_UNSUCCESSFUL                     ((NTSTATUS)0xC0000001UL)
#define STATUS_INVALID_PARAMETER                ((NTSTATUS)0xC000000DUL)
#define STATUS_NO_SUCH_DEVICE                   ((NTSTATUS)0xC000000EUL)
#define STATUS_NO_MEMORY                        ((NTSTATUS)0xC0000017UL)
#define STATUS_ACCESS_DENIED                    ((NTSTATUS)0xC0000022UL)
#define STATUS_OBJECT_NAME_INVALID              ((NTSTATUS)0xC0000033UL)
#define STATUS_OBJECT_NAME_NOT_FOUND            ((NTSTATUS)0xC0000034UL)
#define STATUS_OBJECT_NAME_COLLISION            ((NTSTATUS)0xC0000035UL)
#define STATUS_OBJECT_PATH_INVALID              ((NTSTATUS)0xC0000039UL)
#define STATUS_OBJECT_PATH_NOT_FOUND            ((NTSTATUS)0xC000003AUL)
#define STATUS_OBJECT_PATH_SYNTAX_BAD           ((NTSTATUS)0xC000003BUL)
#define STATUS_INSUFFICIENT_RESOURCES           ((NTSTATUS)0xC000009AUL)
#define STATUS_FILE_IS_A_DIRECTORY              ((NTSTATUS)0xC00000BAUL)
#define STATUS_INVALID_PARAMETER_1              ((NTSTATUS)0xC00000EFUL)
#define STATUS_INVALID_PARAMETER_2              ((NTSTATUS)0xC00000F0UL)
#define STATUS_INVALID_PARAMETER_3              ((NTSTATUS)0xC00000F1UL)
#define STATUS_INVALID_PARAMETER_4              ((NTSTATUS)0xC00000F2UL)
#define STATUS_INVALID_PARAMETER_5              ((NTSTATUS)0xC00000F3UL)
#define STATUS_INVALID_PARAMETER_6              ((NTSTATUS)0xC00000F4UL)
#define STATUS_INVALID_PARAMETER_7              ((NTSTATUS)0xC00000F5UL)
#define STATUS_INVALID_PARAMETER_8              ((NTSTATUS)0xC00000F6UL)
#define STATUS_INVALID_PARAMETER_9              ((NTSTATUS)0xC00000F7UL)
#define STATUS_INVALID_PARAMETER_10             ((NTSTATUS)0xC00000F8UL)
#define STATUS_INVALID_PARAMETER_11             ((NTSTATUS)0xC00000F9UL)
#define STATUS_INVALID_PARAMETER_12             ((NTSTATUS)0xC00000FAUL)
#define STATUS_NOT_A_DIRECTORY                  ((NTSTATUS)0xC0000103UL)
#define STATUS_INVALID_ADDRESS                  ((NTSTATUS)0xC0000141UL)
#define STATUS_DRIVER_INTERNAL_ERROR            ((NTSTATUS)0xC0000183UL)
#define STATUS_IO_DEVICE_ERROR                  ((NTSTATUS)0xC0000185UL)
#define STATUS_DRIVER_UNABLE_TO_LOAD            ((NTSTATUS)0xC000026CUL)
