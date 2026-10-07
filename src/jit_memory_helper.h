#ifndef TURBOWASM_JIT_MEMORY_HELPER_H
#define TURBOWASM_JIT_MEMORY_HELPER_H

#include "instance_internal.h"

enum { TURBOWASM_JIT_MEMORY_BULK = 0x100, TURBOWASM_JIT_MEMORY_ATOMIC = 0x200 };

/* Integer arguments carry unsigned guest bit patterns. Helpers borrow backing
 * only during the call and publish failure through the invocation context. */
int64_t turbowasm_jit_memory(
    turbowasm_jit_invocation_context *context, int64_t opcode,
    int64_t memory, int64_t secondary, int64_t offset,
    int64_t a, int64_t b, int64_t c);
float turbowasm_jit_memory_load_f32(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address);
double turbowasm_jit_memory_load_f64(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address);
int64_t turbowasm_jit_memory_store_f32(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address, float value);
int64_t turbowasm_jit_memory_store_f64(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address, double value);

#endif
