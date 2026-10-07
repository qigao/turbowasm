#ifndef TURBOWASM_JIT_EXCEPTION_HELPER_H
#define TURBOWASM_JIT_EXCEPTION_HELPER_H
#include "instance_internal.h"
enum {
    TURBOWASM_JIT_THROW = 0x08,
    TURBOWASM_JIT_THROW_REF = 0x0a,
    TURBOWASM_JIT_TRY_TABLE = 0x1f
};
/* Borrowed, rooted call cells. Payloads commit through Runtime's exception list. */
int64_t turbowasm_jit_throw(turbowasm_jit_invocation_context *context,
    int64_t opcode, int64_t tag, turbowasm_value *arguments, int64_t count);
/* -1: error, 0: no match, 1: matched and pending exception consumed. */
int64_t turbowasm_jit_catch(turbowasm_jit_invocation_context *context,
    const turbowasm_validation_catch *clause, turbowasm_value *results, int64_t capacity);
#endif
