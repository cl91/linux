#include "atomic_ll_sc.h"

#define __lse_ll_sc_body(op, ...)                                       \
({                                                                      \
                __ll_sc_##op(__VA_ARGS__);                              \
})

#define ARM64_LSE_ATOMIC_INSN(llsc, lse)                                \
        llsc
