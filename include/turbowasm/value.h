#ifndef TURBOWASM_VALUE_H
#define TURBOWASM_VALUE_H

#include <turbowasm/status.h>

#include <cmeta/vector.h>
#include <salts/simd.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbowasm_value_kind {
    TURBOWASM_VALUE_I32 = 0x7f,
    TURBOWASM_VALUE_I64 = 0x7e,
    TURBOWASM_VALUE_F32 = 0x7d,
    TURBOWASM_VALUE_F64 = 0x7c,
    TURBOWASM_VALUE_V128 = 0x7b,
    TURBOWASM_VALUE_FUNCREF = 0x70
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

typedef struct turbowasm_funcref {
    bool is_null;
    uint32_t function_index;

    /*
     * Opaque borrowed identity of the function's owning TurboWasm instance.
     * Runtime-produced non-null refs always set this token. A host-supplied
     * non-null ref may leave it NULL to mean "the receiving instance" for
     * backward-compatible instance-relative construction.
     *
     * Do not dereference or manufacture this value. It becomes invalid when
     * its owning instance is destroyed.
     */
    const void *owner;
} turbowasm_funcref;

typedef struct turbowasm_value {
    turbowasm_value_kind kind;
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        turbowasm_v128 v128;
        turbowasm_funcref funcref;
    } as;
} turbowasm_value;

/*
 * Canonical CMeta reflection for TurboWasm host value carriers.
 *
 * Scalar kinds reuse Salts-owned semantic identities. v128 and funcref use
 * TurboWasm-owned descriptors because their host carriers are TurboWasm
 * structs. Wasm binary type bytes remain the parser/validator source of truth;
 * this is a read-only semantic projection for higher layers.
 */
const cmeta_type_desc *
turbowasm_value_type_descriptor(turbowasm_value_kind kind);

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
