#pragma once

#ifdef __i386__
#include "../../x86/include/asm/cmpxchg_32.h"
#else
#include "../../x86/include/asm/cmpxchg_64.h"
#endif
