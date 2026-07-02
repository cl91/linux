/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_NTOS_CURRENT_H
#define _ASM_NTOS_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLER__

struct task_struct;

extern struct task_struct *current;

#endif /* __ASSEMBLER__ */

#endif /* _ASM_X86_CURRENT_H */
