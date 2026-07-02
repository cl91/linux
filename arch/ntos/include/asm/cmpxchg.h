#pragma once

#if defined(__i386__) || defined(__x86_64__)
#include "../../x86/include/asm/cmpxchg.h"
#elif defined(__aarch64__)
#include "../../arm64/include/asm/cmpxchg.h"
#endif
