#ifndef TURBOWASM_JIT_SIMD_HELPER_H
#define TURBOWASM_JIT_SIMD_HELPER_H

#include "instance_internal.h"

#include <stdint.h>

/*
 * Private backend-neutral SIMD helper ABI.
 *
 * JIT backends keep temporary v128 values in invocation-owned salts_v128
 * slots.  Generated code refers to slots only by compact integer index and
 * never exposes MIR/SIMDe/native vector types.
 */

int64_t turbowasm_jit_simd_const(
    turbowasm_jit_invocation_context *context,
    int64_t out_slot,
    int64_t low_bits,
    int64_t high_bits);

int64_t turbowasm_jit_simd_copy(
    turbowasm_jit_invocation_context *context,
    int64_t out_slot,
    int64_t in_slot);

int64_t turbowasm_jit_simd_splat_i64(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    int64_t scalar);

int64_t turbowasm_jit_simd_splat_f32(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    float scalar);

int64_t turbowasm_jit_simd_splat_f64(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    double scalar);

int64_t turbowasm_jit_simd_op(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    int64_t a_slot,
    int64_t b_slot,
    int64_t c_slot,
    int64_t scalar_count);

int64_t turbowasm_jit_simd_reduce(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t in_slot);

int64_t turbowasm_jit_simd_memory(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t slot,
    int64_t address,
    int64_t offset);

#endif /* TURBOWASM_JIT_SIMD_HELPER_H */
