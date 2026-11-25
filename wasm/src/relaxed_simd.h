#ifndef TURBOWASM_RELAXED_SIMD_H
#define TURBOWASM_RELAXED_SIMD_H

#include <salts/simd.h>

#include <stdbool.h>
#include <stdint.h>

uint8_t turbowasm_relaxed_simd_arity(uint32_t opcode);

bool turbowasm_relaxed_simd_execute(
    uint32_t opcode,
    simd_v128 *out,
    const simd_v128 *a,
    const simd_v128 *b,
    const simd_v128 *c);

#endif /* TURBOWASM_RELAXED_SIMD_H */
