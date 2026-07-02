#ifndef __NTOS_BITSPERLONG_H
#define __NTOS_BITSPERLONG_H

#ifdef CONFIG_64BIT
#define __BITS_PER_LONG 64
#else
#define __BITS_PER_LONG 32
#endif

#define BITS_PER_LONG __BITS_PER_LONG

#define BITS_PER_LONG_LONG 64

#define small_const_nbits(nbits) \
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)

#endif

