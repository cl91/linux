#include <linux/kmod.h>
#include <linux/moduleloader.h>
#include <linux/syscalls.h>
#include <host_ops.h>
#include <init.h>

int __request_module(bool wait, const char *fmt, ...)
{
	char path[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(path, sizeof(path), fmt, args);
	va_end(args);
	return ntos_request_module(path, current->thread_info.kevent, wait);
}
EXPORT_SYMBOL(__request_module);

int lnxdrv_add_module(void *view_base, size_t view_size, const char *args,
		      void *kevent)
{
	BUG_ON(!args);
	KERNEL_THREAD_ENTER(kevent);
	int ret = sys_init_module(view_base, view_size, args);
	KERNEL_THREAD_EXIT;
	return ret;
}

static inline const char *mod_mem_type_to_str(enum mod_mem_type ty)
{
	switch (ty) {
	case MOD_TEXT:
		return ".text";
	case MOD_DATA:
		return ".data";
	case MOD_RODATA:
		return ".rodata";
	case MOD_RO_AFTER_INIT:
		return ".ro_after_init";
	case MOD_INIT_TEXT:
		return ".init.text";
	case MOD_INIT_DATA:
		return ".init.data";
	case MOD_INIT_RODATA:
		return ".init.rodata";
	default:
		return "INVALID";
	}
}

int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *me)
{
	for (int i = 0; i < MOD_MEM_NUM_TYPES; i++) {
		if (!me->mem[i].base || !me->mem[i].size) {
			continue;
		}
		char buf[256] = {};
		snprintf(buf, sizeof(buf), "%s.ko!%s", me->name, mod_mem_type_to_str(i));
		ntos_register_module(buf, me->mem[i].base, me->mem[i].size);
	}
	return 0;
}

/**
 * aarch64_insn_copy - Copy instructions into (an unused part of) RX memory
 * @dst: address to modify
 * @src: source of the copy
 * @len: length to copy
 *
 * Useful for JITs to dump new code blocks into unused regions of RX memory.
 */
#include "../../arm64/include/asm/text-patching.h"
void *aarch64_insn_copy(void *dst, void *src, size_t len)
{
        /* A64 instructions must be word aligned */
        if ((uintptr_t)dst & 0x3)
                return NULL;

        return memcpy(dst, src, len);
}
