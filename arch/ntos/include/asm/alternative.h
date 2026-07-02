#pragma once

#if defined(__i386__) || defined(__x86_64__)

#ifdef __ASSEMBLER__
.macro LOCK_PREFIX
.endm
#else
#define LOCK_PREFIX ""
#endif

/*
 * Combine multiple asm inline constraint args into a single arg for passing to
 * another macro.
 */
#define ASM_OUTPUT(x...)        x
#define ASM_INPUT(x...)         x

#ifdef __ASSEMBLER__
# define __ASM_FORM(x, ...)		x,## __VA_ARGS__
# define __ASM_FORM_RAW(x, ...)		x,## __VA_ARGS__
# define __ASM_FORM_COMMA(x, ...)	x,## __VA_ARGS__,
# define __ASM_REGPFX			%
#else
#include <linux/stringify.h>
# define __ASM_FORM(x, ...)		" " __stringify(x,##__VA_ARGS__) " "
# define __ASM_FORM_RAW(x, ...)		    __stringify(x,##__VA_ARGS__)
# define __ASM_FORM_COMMA(x, ...)	" " __stringify(x,##__VA_ARGS__) ","
# define __ASM_REGPFX			%%
#endif

#define _ASM_BYTES(x, ...)	__ASM_FORM(.byte x,##__VA_ARGS__ ;)

#ifndef __x86_64__
/* 32 bit */
# define __ASM_SEL(a,b)		__ASM_FORM(a)
# define __ASM_SEL_RAW(a,b)	__ASM_FORM_RAW(a)
#else
/* 64 bit */
# define __ASM_SEL(a,b)		__ASM_FORM(b)
# define __ASM_SEL_RAW(a,b)	__ASM_FORM_RAW(b)
#endif

#define __ASM_SIZE(inst, ...)	__ASM_SEL(inst##l##__VA_ARGS__, \
					  inst##q##__VA_ARGS__)
#define __ASM_REG(reg)         __ASM_SEL_RAW(e##reg, r##reg)

#ifndef __ASSEMBLY__
#define _ASM_SP		__ASM_REG(sp)
register unsigned long current_stack_pointer asm(_ASM_SP);
#define ASM_CALL_CONSTRAINT "+r" (current_stack_pointer)
#define ALT_OUTPUT_SP(...) ASM_CALL_CONSTRAINT, ## __VA_ARGS__
#endif

/* Insert a comma if args are non-empty */
#define COMMA(x...)		__COMMA(x)
#define __COMMA(...)		, ##__VA_ARGS__

#define ALTERNATIVE(_1, inst, _2) inst

#endif
