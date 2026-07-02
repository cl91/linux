#if defined(__i386__) || defined(__x86_64__)
#define CONFIG_X86_CX8
#include "../../x86/include/asm/atomic.h"
#elif defined(__aarch64__)
#include "../../arm64/include/asm/atomic.h"
#endif
