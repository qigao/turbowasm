#include <turbowasm/value.h>

static const cmeta_type_identity turbowasm_v128_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("turbowasm.value.v128");
static const cmeta_type_identity turbowasm_funcref_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("turbowasm.value.funcref");
static const cmeta_type_identity turbowasm_externref_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("turbowasm.value.externref");

static const cmeta_type_desc turbowasm_v128_type = {
    .name = "turbowasm_v128",
    .size = sizeof(turbowasm_v128),
    .align = _Alignof(turbowasm_v128),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &turbowasm_v128_type_identity
};

static const cmeta_type_desc turbowasm_funcref_type = {
    .name = "turbowasm_funcref",
    .size = sizeof(turbowasm_funcref),
    .align = _Alignof(turbowasm_funcref),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &turbowasm_funcref_type_identity
};

static const cmeta_type_desc turbowasm_externref_type = {
    .name = "turbowasm_externref",
    .size = sizeof(turbowasm_externref),
    .align = _Alignof(turbowasm_externref),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &turbowasm_externref_type_identity
};

const cmeta_type_desc *
turbowasm_value_type_descriptor(turbowasm_value_kind kind) {
    switch (kind) {
        case TURBOWASM_VALUE_I32: return &cmeta_type_int32;
        case TURBOWASM_VALUE_I64: return &cmeta_type_int64;
        case TURBOWASM_VALUE_F32: return &cmeta_type_float;
        case TURBOWASM_VALUE_F64: return &cmeta_type_double;
        case TURBOWASM_VALUE_V128: return &turbowasm_v128_type;
        case TURBOWASM_VALUE_FUNCREF: return &turbowasm_funcref_type;
        case TURBOWASM_VALUE_EXTERNREF: return &turbowasm_externref_type;
        default: return NULL;
    }
}

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
    if (out == NULL || bytes == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (shape != TURBOWASM_V128_RAW &&
        turbowasm_v128_descriptor(shape) == NULL)
        return TURBOWASM_UNSUPPORTED;
    salts_simd_v128_load(&out->bits, bytes);
    out->shape = shape;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_v128_store(void *bytes,
                                      const turbowasm_v128 *value) {
    if (bytes == NULL || value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (value->shape != TURBOWASM_V128_RAW &&
        turbowasm_v128_descriptor(value->shape) == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    salts_simd_v128_store(bytes, &value->bits);
    return TURBOWASM_OK;
}
