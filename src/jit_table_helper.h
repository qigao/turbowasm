#ifndef TURBOWASM_JIT_TABLE_HELPER_H
#define TURBOWASM_JIT_TABLE_HELPER_H

#include "instance_internal.h"

typedef enum turbowasm_jit_table_opcode {
    TURBOWASM_JIT_TABLE_INIT = 12,
    TURBOWASM_JIT_ELEMENT_DROP = 13,
    TURBOWASM_JIT_TABLE_COPY = 14,
    TURBOWASM_JIT_TABLE_SIZE = 16
} turbowasm_jit_table_opcode;

/* Addresses carry unsigned guest bits. Runtime owns table/segment state and
 * publishes errors through context; no entry pointer escapes the helper. */
int64_t turbowasm_jit_table(
    turbowasm_jit_invocation_context *context, int64_t opcode,
    int64_t table, int64_t secondary, int64_t a, int64_t b, int64_t c);

#endif
