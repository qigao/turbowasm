#ifndef TURBOWASM_JIT_GC_HELPER_H
#define TURBOWASM_JIT_GC_HELPER_H
#include "instance_internal.h"
#include "reader.h"

enum {
    TURBOWASM_JIT_GC_PREFIX = 0xfb,
    TURBOWASM_GC_FIXED_INPUT_LIMIT = 5,
    TURBOWASM_GC_STRUCT_NEW = 0, TURBOWASM_GC_STRUCT_NEW_DEFAULT,
    TURBOWASM_GC_STRUCT_GET, TURBOWASM_GC_STRUCT_GET_S, TURBOWASM_GC_STRUCT_GET_U,
    TURBOWASM_GC_STRUCT_SET, TURBOWASM_GC_ARRAY_NEW, TURBOWASM_GC_ARRAY_NEW_DEFAULT,
    TURBOWASM_GC_ARRAY_NEW_FIXED, TURBOWASM_GC_ARRAY_NEW_DATA, TURBOWASM_GC_ARRAY_NEW_ELEM,
    TURBOWASM_GC_ARRAY_GET, TURBOWASM_GC_ARRAY_GET_S, TURBOWASM_GC_ARRAY_GET_U,
    TURBOWASM_GC_ARRAY_SET, TURBOWASM_GC_ARRAY_LEN, TURBOWASM_GC_ARRAY_FILL,
    TURBOWASM_GC_ARRAY_COPY, TURBOWASM_GC_ARRAY_INIT_DATA, TURBOWASM_GC_ARRAY_INIT_ELEM,
    TURBOWASM_GC_REF_TEST, TURBOWASM_GC_REF_TEST_NULL,
    TURBOWASM_GC_REF_CAST, TURBOWASM_GC_REF_CAST_NULL,
    TURBOWASM_GC_BR_ON_CAST, TURBOWASM_GC_BR_ON_CAST_FAIL,
    TURBOWASM_GC_ANY_CONVERT_EXTERN, TURBOWASM_GC_EXTERN_CONVERT_ANY,
    TURBOWASM_GC_REF_I31, TURBOWASM_GC_I31_GET_S, TURBOWASM_GC_I31_GET_U
};

/* Borrowed validated bytes/fields; no allocation and no heap addresses. */
typedef struct turbowasm_jit_gc_instruction {
    const uint8_t *code;
    uint32_t size, opcode, input_count, label;
    uint8_t result_type;
    uint8_t inputs[TURBOWASM_GC_FIXED_INPUT_LIMIT];
    const turbowasm_validation_field *fields;
    uint8_t repeated_type;
    bool branches;
} turbowasm_jit_gc_instruction;
bool turbowasm_jit_gc_decode(const turbowasm_validation_context *validation,
    turbowasm_reader *reader, turbowasm_jit_gc_instruction *out);
uint8_t turbowasm_jit_gc_input_type(const turbowasm_jit_gc_instruction *op, uint32_t index);
/* Args/results belong to the caller's registered native root source. Returns
 * the cast-branch decision; status/trap are carried by the invocation context. */
int64_t turbowasm_jit_gc(turbowasm_jit_invocation_context *context,
    const uint8_t *code, int64_t size, const turbowasm_value *args, int64_t count,
    turbowasm_value *result);
#endif
