#ifndef TURBOWASM_VALIDATE_SIMD_TABLE_H
#define TURBOWASM_VALIDATE_SIMD_TABLE_H

#include <stdint.h>

typedef enum turbowasm_simd_stack_form {
    TURBOWASM_SIMD_CONST_V128 = 0,
    TURBOWASM_SIMD_SHUFFLE,
    TURBOWASM_SIMD_V128_UNARY,
    TURBOWASM_SIMD_V128_BINARY,
    TURBOWASM_SIMD_V128_TERNARY,
    TURBOWASM_SIMD_V128_TEST_I32,
    TURBOWASM_SIMD_SCALAR_SPLAT,
    TURBOWASM_SIMD_LANE_EXTRACT,
    TURBOWASM_SIMD_LANE_REPLACE,
    TURBOWASM_SIMD_V128_SHIFT
} turbowasm_simd_stack_form;

typedef struct turbowasm_simd_descriptor {
    uint32_t opcode;
    turbowasm_simd_stack_form form;
    uint8_t scalar_type;
    uint8_t lane_count;
} turbowasm_simd_descriptor;

const turbowasm_simd_descriptor *
turbowasm_simd_descriptor_find(uint32_t opcode);

#endif /* TURBOWASM_VALIDATE_SIMD_TABLE_H */
