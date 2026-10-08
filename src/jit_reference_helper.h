#ifndef TURBOWASM_JIT_REFERENCE_HELPER_H
#define TURBOWASM_JIT_REFERENCE_HELPER_H
#include "instance_internal.h"

enum {
    TURBOWASM_JIT_UNREACHABLE = 0x00,
    TURBOWASM_JIT_GLOBAL_GET = 0x23,
    TURBOWASM_JIT_GLOBAL_SET = 0x24,
    TURBOWASM_JIT_REF_IS_NULL = 0xd1,
    TURBOWASM_JIT_REF_FUNC = 0xd2,
    TURBOWASM_JIT_REF_EQ = 0xd3,
    TURBOWASM_JIT_REF_AS_NON_NULL = 0xd4,
    TURBOWASM_JIT_BR_ON_NULL = 0xd5,
    TURBOWASM_JIT_BR_ON_NON_NULL = 0xd6
};
int64_t turbowasm_jit_reference(turbowasm_jit_invocation_context *context,
    int64_t opcode, int64_t immediate, turbowasm_value *out,
    const turbowasm_value *a, const turbowasm_value *b);
#endif
