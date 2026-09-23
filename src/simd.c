#include <turbowasm/simd.h>

static turbowasm_status turbowasm_require_shape(const turbowasm_v128 *value,
                                                turbowasm_v128_shape shape) {
    if (value == NULL) return TURBOWASM_INVALID_ARGUMENT;
    return value->shape == shape ? TURBOWASM_OK : TURBOWASM_TYPE_MISMATCH;
}

turbowasm_status turbowasm_simd_i32x4_add(turbowasm_v128 *out,
                                          const turbowasm_v128 *left,
                                          const turbowasm_v128 *right) {
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (turbowasm_require_shape(left, TURBOWASM_V128_I32X4) != TURBOWASM_OK ||
        turbowasm_require_shape(right, TURBOWASM_V128_I32X4) != TURBOWASM_OK)
        return TURBOWASM_TYPE_MISMATCH;
    salts_simd_i32x4_add(&out->bits, &left->bits, &right->bits);
    out->shape = TURBOWASM_V128_I32X4;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_simd_i32x4_eq(turbowasm_v128 *out_mask,
                                         const turbowasm_v128 *left,
                                         const turbowasm_v128 *right) {
    if (out_mask == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (turbowasm_require_shape(left, TURBOWASM_V128_I32X4) != TURBOWASM_OK ||
        turbowasm_require_shape(right, TURBOWASM_V128_I32X4) != TURBOWASM_OK)
        return TURBOWASM_TYPE_MISMATCH;
    salts_simd_i32x4_eq(&out_mask->bits, &left->bits, &right->bits);
    out_mask->shape = TURBOWASM_V128_B32X4;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_simd_f32x4_mul(turbowasm_v128 *out,
                                          const turbowasm_v128 *left,
                                          const turbowasm_v128 *right) {
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (turbowasm_require_shape(left, TURBOWASM_V128_F32X4) != TURBOWASM_OK ||
        turbowasm_require_shape(right, TURBOWASM_V128_F32X4) != TURBOWASM_OK)
        return TURBOWASM_TYPE_MISMATCH;
    salts_simd_f32x4_mul(&out->bits, &left->bits, &right->bits);
    out->shape = TURBOWASM_V128_F32X4;
    return TURBOWASM_OK;
}
