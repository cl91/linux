/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/vmlinux.lds.h>
#include <asm/thread_info.h>
#include <asm/page.h>
#include <asm/cache.h>
#include <linux/export.h>
#include <ntlnxdrv.h>

OUTPUT_FORMAT(CONFIG_OUTPUT_FORMAT)

SECTIONS
{
	. = KERNEL_IMAGE_START;	       /* Base virtual address */

	.text :
	{
		_stext = .;
		_text = . ;
		text = . ;

		. = ALIGN(PAGE_SIZE);
		_sinittext = .;
		INIT_TEXT
		_einittext = .;

		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT

		_etext = .;
	}

	__init_begin = .;
	INIT_DATA_SECTION(16)
	PERCPU_SECTION(L1_CACHE_BYTES)
	__init_end = .;

	_sdata = .;
	RO_DATA(PAGE_SIZE)
	RW_DATA(L1_CACHE_BYTES, PAGE_SIZE, THREAD_SIZE)
	_edata = .;

	__start_ro_after_init = .;
	.data..ro_after_init : { *(.data..ro_after_init)}
	EXCEPTION_TABLE(16)
	__end_ro_after_init = .;
	NOTES

	BSS_SECTION(0, 0, 0)
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG

	// Don't include DISCARDS section to build the target with code coverage.
	DISCARDS
}
