# SPDX-License-Identifier: GPL-2.0

ifeq (,$(CLANG_TARGET_FLAGS))
$(error You must define CLANG_TARGET_FLAGS)
endif

TARGET_ARCH=$(firstword $(subst -, ,$(CLANG_TARGET_FLAGS)))
export TARGET_ARCH

ifeq (i686,$(TARGET_ARCH))
OUTPUT_FORMAT = elf32-i386
TARGET_ARCH_64BIT = n
TARGET_ARCH_I686 = y
TARGET_ARCH_AMD64 = n
TARGET_ARCH_X86 = y
TARGET_ARCH_ARM64 = n
else ifeq (x86_64,$(TARGET_ARCH))
OUTPUT_FORMAT = elf64-x86-64
TARGET_ARCH_64BIT = y
TARGET_ARCH_I686 = n
TARGET_ARCH_AMD64 = y
TARGET_ARCH_X86 = y
TARGET_ARCH_ARM64 = n
else ifeq (aarch64,$(TARGET_ARCH))
OUTPUT_FORMAT = elf64-littleaarch64
TARGET_ARCH_64BIT = y
TARGET_ARCH_I686 = n
TARGET_ARCH_AMD64 = n
TARGET_ARCH_X86 = n
TARGET_ARCH_ARM64 = y
else
$(error Unsupported target triple: $(CLANG_TARGET_FLAGS))
endif
export OUTPUT_FORMAT
export TARGET_ARCH_64BIT
export TARGET_ARCH_I686
export TARGET_ARCH_AMD64
export TARGET_ARCH_X86
export TARGET_ARCH_ARM64

KBUILD_CFLAGS += -fno-builtin -DDEBUG
LDFLAGS_vmlinux += --entry=LnxDriverEntry --error-limit=0

ifeq (aarch64,$(TARGET_ARCH))
archprepare:
	$(Q)$(MAKE) $(build)=arch/arm64/tools kapi
endif

core-y += arch/ntos/kernel/
core-y += arch/ntos/mm/
core-y += arch/ntos/fs/
core-y += arch/ntos/drivers/
core-y += arch/ntos/net/

core-$(TARGET_ARCH_I686) += arch/x86/lib/atomic64_32.o arch/x86/lib/atomic64_cx8_32.o
