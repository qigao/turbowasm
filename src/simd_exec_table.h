#ifndef TURBOWASM_SIMD_EXEC_TABLE_H
#define TURBOWASM_SIMD_EXEC_TABLE_H

#include <turbowasm/value.h>

#include <salts/simd.h>

#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_simd_exec_kind {
    TURBOWASM_SIMD_EXEC_SPLAT = 0,
    TURBOWASM_SIMD_EXEC_UNARY,
    TURBOWASM_SIMD_EXEC_BINARY,
    TURBOWASM_SIMD_EXEC_COMPARE,
    TURBOWASM_SIMD_EXEC_SHIFT,
    TURBOWASM_SIMD_EXEC_SELECT
} turbowasm_simd_exec_kind;

typedef struct turbowasm_simd_exec_descriptor {
    uint32_t opcode;
    turbowasm_simd_exec_kind kind;
    const cmeta_vector_desc *vector_desc;
    uint8_t op;
    turbowasm_v128_shape result_shape;
} turbowasm_simd_exec_descriptor;

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_find(uint32_t opcode);

size_t turbowasm_simd_exec_descriptor_count(void);

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_at(size_t index);

#endif /* TURBOWASM_SIMD_EXEC_TABLE_H */
