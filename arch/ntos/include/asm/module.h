#if defined(__i386__) || defined(__x86_64__)
#include <asm-generic/module.h>
#elif defined(__aarch64__)
#define cpus_have_final_cap(...) false
#include "../../arm64/include/asm/module.h"
#undef cpus_have_final_cap
#endif
