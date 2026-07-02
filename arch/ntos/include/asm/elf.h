#ifndef _ASM_NTOS_ELF_H
#define _ASM_NTOS_ELF_H

#include <linux/elf-em.h>

#if defined(__i386__) || defined(__x86_64__)
#include "../../x86/include/asm/elf.h"
#elif defined(__aarch64__)
#define ELF_ARCH        EM_AARCH64
#else
#error "Unsupported architecture"
#endif

#define elf_check_arch(x) ((x)->e_machine == ELF_ARCH)

#ifdef CONFIG_64BIT
#define ELF_CLASS ELFCLASS64
#else
#define ELF_CLASS ELFCLASS32
#endif

#define elf_gregset_t long
#define elf_fpregset_t double
#endif
