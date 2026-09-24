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
    TURBOWASM_SIMD_EXEC_SELECT,
    TURBOWASM_SIMD_EXEC_SATURATING,
    TURBOWASM_SIMD_EXEC_REDUCE,
    TURBOWASM_SIMD_EXEC_NARROW,
    TURBOWASM_SIMD_EXEC_EXTEND_HALF,
    TURBOWASM_SIMD_EXEC_EXTMUL_HALF,
    TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE,
    TURBOWASM_SIMD_EXEC_MEMORY_EXTEND,
    TURBOWASM_SIMD_EXEC_MEMORY_SPLAT,
    TURBOWASM_SIMD_EXEC_MEMORY_ZERO,
    TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE,
    TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE,
    TURBOWASM_SIMD_EXEC_LANE_EXTRACT,
    TURBOWASM_SIMD_EXEC_LANE_REPLACE,
    TURBOWASM_SIMD_EXEC_SHUFFLE,
    TURBOWASM_SIMD_EXEC_SWIZZLE
} turbowasm_simd_exec_kind;

typedef struct turbowasm_simd_exec_descriptor {
    uint32_t opcode;
    turbowasm_simd_exec_kind kind;
    const cmeta_vector_desc *vector_desc;
    uint8_t op;
    turbowasm_v128_shape result_shape;
    uint8_t memory_width;
} turbowasm_simd_exec_descriptor;

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_find(uint32_t opcode);

size_t turbowasm_simd_exec_descriptor_count(void);

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_at(size_t index);

#endif /* TURBOWASM_SIMD_EXEC_TABLE_H */
