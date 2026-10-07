#include "relaxed_simd.h"

#include <cmeta/vector.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t value = 0u;
    size_t index;
    for (index = 0u; index < 8u; ++index)
        value |= (uint64_t)p[index] << (8u * index);
    return value;
}

static void write_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static void write_u64_le(uint8_t *p, uint64_t value) {
    size_t index;
    for (index = 0u; index < 8u; ++index)
        p[index] = (uint8_t)(value >> (8u * index));
}

static int16_t clamp_i16(int32_t value) {
    if (value < INT16_MIN)
        return INT16_MIN;
    if (value > INT16_MAX)
        return INT16_MAX;
    return (int16_t)value;
}

static int32_t relaxed_trunc_s32(double value) {
    if (isnan(value))
        return 0;
    if (value <= -2147483648.0)
        return INT32_MIN;
    if (value >= 2147483648.0)
        return INT32_MAX;
    return (int32_t)trunc(value);
}

static uint32_t relaxed_trunc_u32(double value) {
    if (isnan(value) || value <= 0.0)
        return 0u;
    if (value >= 4294967296.0)
        return UINT32_MAX;
    return (uint32_t)trunc(value);
}

static bool relaxed_trunc_f32x4(
    cmeta_v128 *out,
    const cmeta_v128 *value,
    bool is_signed) {
    uint8_t source[16];
    uint8_t result[16];
    size_t lane;

    salts_simd_v128_store(source, value);
    for (lane = 0u; lane < 4u; ++lane) {
        uint32_t bits = read_u32_le(source + lane * 4u);
        float input;

        memcpy(&input, &bits, sizeof(input));
        write_u32_le(
            result + lane * 4u,
            is_signed
                ? (uint32_t)relaxed_trunc_s32((double)input)
                : relaxed_trunc_u32((double)input));
    }

    salts_simd_v128_load(out, result);
    return true;
}

static bool relaxed_trunc_f64x2_zero(
    cmeta_v128 *out,
    const cmeta_v128 *value,
    bool is_signed) {
    uint8_t source[16];
    uint8_t result[16] = {0};
    size_t lane;

    salts_simd_v128_store(source, value);
    for (lane = 0u; lane < 2u; ++lane) {
        uint64_t bits = read_u64_le(source + lane * 8u);
        double input;

        memcpy(&input, &bits, sizeof(input));
        write_u32_le(
            result + lane * 4u,
            is_signed
                ? (uint32_t)relaxed_trunc_s32(input)
                : relaxed_trunc_u32(input));
    }

    salts_simd_v128_load(out, result);
    return true;
}

static bool relaxed_minmax_f32(
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b,
    bool is_min) {
    uint8_t left[16];
    uint8_t right[16];
    uint8_t result[16];
    size_t lane;

    salts_simd_v128_store(left, a);
    salts_simd_v128_store(right, b);

    for (lane = 0u; lane < 4u; ++lane) {
        uint32_t abits = read_u32_le(left + lane * 4u);
        uint32_t bbits = read_u32_le(right + lane * 4u);
        float av;
        float bv;
        uint32_t chosen;

        memcpy(&av, &abits, sizeof(av));
        memcpy(&bv, &bbits, sizeof(bv));

        /*
         * Relaxed min/max permits either input for NaNs and opposite signed
         * zero. Choosing the left input for every numerically equal pair is
         * therefore a stable legal projection.
         */
        if (isnan(av) || isnan(bv) || av == bv)
            chosen = abits;
        else if (is_min)
            chosen = av < bv ? abits : bbits;
        else
            chosen = av > bv ? abits : bbits;

        write_u32_le(result + lane * 4u, chosen);
    }

    salts_simd_v128_load(out, result);
    return true;
}

static bool relaxed_minmax_f64(
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b,
    bool is_min) {
    uint8_t left[16];
    uint8_t right[16];
    uint8_t result[16];
    size_t lane;

    salts_simd_v128_store(left, a);
    salts_simd_v128_store(right, b);

    for (lane = 0u; lane < 2u; ++lane) {
        uint64_t abits = read_u64_le(left + lane * 8u);
        uint64_t bbits = read_u64_le(right + lane * 8u);
        double av;
        double bv;
        uint64_t chosen;

        memcpy(&av, &abits, sizeof(av));
        memcpy(&bv, &bbits, sizeof(bv));

        if (isnan(av) || isnan(bv) || av == bv)
            chosen = abits;
        else if (is_min)
            chosen = av < bv ? abits : bbits;
        else
            chosen = av > bv ? abits : bbits;

        write_u64_le(result + lane * 8u, chosen);
    }

    salts_simd_v128_load(out, result);
    return true;
}

static void relaxed_dot_i8_i7_pairs(
    int16_t out[8],
    const cmeta_v128 *a,
    const cmeta_v128 *b) {
    uint8_t left[16];
    uint8_t right[16];
    size_t pair;

    salts_simd_v128_store(left, a);
    salts_simd_v128_store(right, b);

    for (pair = 0u; pair < 8u; ++pair) {
        size_t lane = pair * 2u;
        int32_t sum =
            (int32_t)(int8_t)left[lane] *
                (int32_t)(int8_t)right[lane] +
            (int32_t)(int8_t)left[lane + 1u] *
                (int32_t)(int8_t)right[lane + 1u];

        /*
         * For right lanes with the high bit set, signed interpretation is
         * one permitted relaxed projection. Saturating each adjacent-pair sum
         * is likewise explicitly permitted.
         */
        out[pair] = clamp_i16(sum);
    }
}

static bool relaxed_dot_i16(
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b) {
    int16_t pairs[8];
    uint8_t result[16];
    size_t lane;

    relaxed_dot_i8_i7_pairs(pairs, a, b);
    for (lane = 0u; lane < 8u; ++lane)
        write_u16_le(
            result + lane * 2u,
            (uint16_t)pairs[lane]);

    salts_simd_v128_load(out, result);
    return true;
}

static bool relaxed_dot_add_i32(
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b,
    const cmeta_v128 *c) {
    int16_t pairs[8];
    uint8_t addend[16];
    uint8_t result[16];
    size_t lane;

    relaxed_dot_i8_i7_pairs(pairs, a, b);
    salts_simd_v128_store(addend, c);

    for (lane = 0u; lane < 4u; ++lane) {
        int32_t dot =
            (int32_t)pairs[lane * 2u] +
            (int32_t)pairs[lane * 2u + 1u];
        uint32_t sum =
            (uint32_t)dot +
            read_u32_le(addend + lane * 4u);
        write_u32_le(result + lane * 4u, sum);
    }

    salts_simd_v128_load(out, result);
    return true;
}

static bool relaxed_madd(
    const cmeta_vector_desc *desc,
    bool negative,
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b,
    const cmeta_v128 *c) {
    cmeta_v128 product;
    cmeta_v128 negated_a;
    const cmeta_v128 *multiplicand = a;

    /*
     * Choose the proposal-permitted unfused projection:
     *   madd  = (a * b) + c
     *   nmadd = ((-a) * b) + c
     * Negating the first operand before multiplication preserves the exact
     * relaxed-nmadd operation order for signed zero and NaN propagation.
     */
    if (negative) {
        if (!salts_simd_unary(
                desc, SALTS_SIMD_UNARY_NEG,
                &negated_a, a))
            return false;
        multiplicand = &negated_a;
    }

    if (!salts_simd_binary(
            desc, SALTS_SIMD_BINARY_MUL,
            &product, multiplicand, b))
        return false;

    return salts_simd_binary(
        desc, SALTS_SIMD_BINARY_ADD,
        out, &product, c);
}

uint8_t turbowasm_relaxed_simd_arity(uint32_t opcode) {
    if (opcode >= 0x101u && opcode <= 0x104u)
        return 1u;

    if (opcode == 0x100u ||
        (opcode >= 0x10du && opcode <= 0x112u))
        return 2u;

    if ((opcode >= 0x105u && opcode <= 0x10cu) ||
        opcode == 0x113u)
        return 3u;

    return 0u;
}

bool turbowasm_relaxed_simd_execute(
    uint32_t opcode,
    cmeta_v128 *out,
    const cmeta_v128 *a,
    const cmeta_v128 *b,
    const cmeta_v128 *c) {
    if (out == NULL || a == NULL)
        return false;

    switch (opcode) {
        case 0x100u:
            return b != NULL &&
                   salts_simd_swizzle_bytes(out, a, b);

        case 0x101u:
            return relaxed_trunc_f32x4(out, a, true);
        case 0x102u:
            return relaxed_trunc_f32x4(out, a, false);
        case 0x103u:
            return relaxed_trunc_f64x2_zero(out, a, true);
        case 0x104u:
            return relaxed_trunc_f64x2_zero(out, a, false);

        case 0x105u:
            return b != NULL && c != NULL &&
                   relaxed_madd(
                       &cmeta_vector_f32x4, false,
                       out, a, b, c);
        case 0x106u:
            return b != NULL && c != NULL &&
                   relaxed_madd(
                       &cmeta_vector_f32x4, true,
                       out, a, b, c);
        case 0x107u:
            return b != NULL && c != NULL &&
                   relaxed_madd(
                       &cmeta_vector_f64x2, false,
                       out, a, b, c);
        case 0x108u:
            return b != NULL && c != NULL &&
                   relaxed_madd(
                       &cmeta_vector_f64x2, true,
                       out, a, b, c);

        case 0x109u:
        case 0x10au:
        case 0x10bu:
        case 0x10cu:
            return b != NULL && c != NULL &&
                   salts_simd_select(
                       &cmeta_vector_i8x16,
                       out, a, b, c);

        case 0x10du:
            return b != NULL &&
                   relaxed_minmax_f32(out, a, b, true);
        case 0x10eu:
            return b != NULL &&
                   relaxed_minmax_f32(out, a, b, false);
        case 0x10fu:
            return b != NULL &&
                   relaxed_minmax_f64(out, a, b, true);
        case 0x110u:
            return b != NULL &&
                   relaxed_minmax_f64(out, a, b, false);

        case 0x111u:
            return b != NULL &&
                   salts_simd_q15mulr_sat(
                       &cmeta_vector_i16x8, out, a, b);

        case 0x112u:
            return b != NULL &&
                   relaxed_dot_i16(out, a, b);

        case 0x113u:
            return b != NULL && c != NULL &&
                   relaxed_dot_add_i32(out, a, b, c);

        default:
            return false;
    }
}
