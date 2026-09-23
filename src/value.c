#include <turbowasm/value.h>

const cmeta_vector_desc *turbowasm_v128_descriptor(turbowasm_v128_shape shape) {
    switch (shape) {
        case TURBOWASM_V128_I8X16: return &cmeta_vector_i8x16;
        case TURBOWASM_V128_U8X16: return &cmeta_vector_u8x16;
        case TURBOWASM_V128_I16X8: return &cmeta_vector_i16x8;
        case TURBOWASM_V128_U16X8: return &cmeta_vector_u16x8;
        case TURBOWASM_V128_I32X4: return &cmeta_vector_i32x4;
        case TURBOWASM_V128_U32X4: return &cmeta_vector_u32x4;
        case TURBOWASM_V128_I64X2: return &cmeta_vector_i64x2;
        case TURBOWASM_V128_U64X2: return &cmeta_vector_u64x2;
        case TURBOWASM_V128_F32X4: return &cmeta_vector_f32x4;
        case TURBOWASM_V128_F64X2: return &cmeta_vector_f64x2;
        case TURBOWASM_V128_B8X16: return &cmeta_vector_b8x16;
        case TURBOWASM_V128_B16X8: return &cmeta_vector_b16x8;
        case TURBOWASM_V128_B32X4: return &cmeta_vector_b32x4;
        case TURBOWASM_V128_B64X2: return &cmeta_vector_b64x2;
        case TURBOWASM_V128_RAW:
        default:
            return NULL;
    }
}

turbowasm_status turbowasm_v128_load(turbowasm_v128 *out,
                                     turbowasm_v128_shape shape,
                                     const void *bytes) {
    if (out == NULL || bytes == NULL || shape == TURBOWASM_V128_RAW)
        return TURBOWASM_INVALID_ARGUMENT;
    if (turbowasm_v128_descriptor(shape) == NULL)
        return TURBOWASM_UNSUPPORTED;
    salts_simd_v128_load(&out->bits, bytes);
    out->shape = shape;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_v128_store(void *bytes,
                                      const turbowasm_v128 *value) {
    if (bytes == NULL || value == NULL ||
        turbowasm_v128_descriptor(value->shape) == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    salts_simd_v128_store(bytes, &value->bits);
    return TURBOWASM_OK;
}
