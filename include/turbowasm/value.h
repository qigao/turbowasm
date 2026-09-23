#ifndef TURBOWASM_VALUE_H
#define TURBOWASM_VALUE_H

#include <turbowasm/status.h>

#include <cmeta/vector.h>
#include <salts/simd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbowasm_value_kind {
    TURBOWASM_VALUE_I32 = 0x7f,
    TURBOWASM_VALUE_I64 = 0x7e,
    TURBOWASM_VALUE_F32 = 0x7d,
    TURBOWASM_VALUE_F64 = 0x7c,
    TURBOWASM_VALUE_V128 = 0x7b
} turbowasm_value_kind;

/* Wasm binary typing stops at v128.  The validated TurboWasm IR may refine the
 * lane interpretation so backends can preserve operation semantics without
 * making storage itself type-specific. */
typedef enum turbowasm_v128_shape {
    TURBOWASM_V128_RAW = 0,
    TURBOWASM_V128_I8X16,
    TURBOWASM_V128_U8X16,
    TURBOWASM_V128_I16X8,
    TURBOWASM_V128_U16X8,
    TURBOWASM_V128_I32X4,
    TURBOWASM_V128_U32X4,
    TURBOWASM_V128_I64X2,
    TURBOWASM_V128_U64X2,
    TURBOWASM_V128_F32X4,
    TURBOWASM_V128_F64X2,
    TURBOWASM_V128_B8X16,
    TURBOWASM_V128_B16X8,
    TURBOWASM_V128_B32X4,
    TURBOWASM_V128_B64X2
} turbowasm_v128_shape;

typedef struct turbowasm_v128 {
    salts_v128 bits;
    turbowasm_v128_shape shape;
} turbowasm_v128;

typedef struct turbowasm_value {
    turbowasm_value_kind kind;
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        turbowasm_v128 v128;
    } as;
} turbowasm_value;

const cmeta_vector_desc *turbowasm_v128_descriptor(turbowasm_v128_shape shape);
turbowasm_status turbowasm_v128_load(turbowasm_v128 *out,
                                     turbowasm_v128_shape shape,
                                     const void *bytes);
turbowasm_status turbowasm_v128_store(void *bytes,
                                      const turbowasm_v128 *value);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_VALUE_H */
