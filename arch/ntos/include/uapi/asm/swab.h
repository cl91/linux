#ifndef _ASM_NTOS_SWAB_H
#define _ASM_NTOS_SWAB_H

#ifndef __arch_swab32
#define __arch_swab32(x) ___constant_swab32(x)
#endif

#include <asm-generic/swab.h>

#endif /* _ASM_NTOS_SWAB_H */
