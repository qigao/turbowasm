#include "simd_exec_table.h"

#define SP(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SPLAT, &(desc_), 0u, (shape_), 0u }

#define UN(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_UNARY, &(desc_), \
      (uint8_t)(op_), (shape_), 0u }

#define BIN(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_BINARY, &(desc_), \
      (uint8_t)(op_), (shape_), 0u }

#define CMP(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_COMPARE, &(desc_), \
      (uint8_t)(op_), (shape_), 0u }

#define SH(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SHIFT, &(desc_), \
      (uint8_t)(op_), (shape_), 0u }

#define SEL(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SELECT, &cmeta_vector_i8x16, \
      0u, TURBOWASM_V128_RAW, 0u }

#define SAT(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SATURATING, &(desc_), \
      (uint8_t)(op_), (shape_), 0u }

#define RED(opcode_, desc_, op_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_REDUCE, &(desc_), \
      (uint8_t)(op_), TURBOWASM_V128_RAW, 0u }

#define NAR(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_NARROW, &(desc_), \
      0u, (shape_), 0u }

#define EXH(opcode_, desc_, half_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTEND_HALF, &(desc_), \
      (uint8_t)(half_), (shape_), 0u }

#define EMH(opcode_, desc_, half_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTMUL_HALF, &(desc_), \
      (uint8_t)(half_), (shape_), 0u }

#define PAIR(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE, &(desc_), \
      0u, (shape_), 0u }

#define Q15(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_Q15MULR, &cmeta_vector_i16x8, \
      0u, TURBOWASM_V128_I16X8, 0u }

#define DOT(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_DOT_PAIRWISE, &cmeta_vector_i32x4, \
      0u, TURBOWASM_V128_I32X4, 0u }

#define MEXT(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_EXTEND, &(desc_), \
      0u, (shape_), (width_) }

#define MSPLAT(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_SPLAT, &(desc_), \
      0u, (shape_), (width_) }

#define MZERO(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_ZERO, &(desc_), \
      0u, (shape_), (width_) }

#define MLOADLANE(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE, &(desc_), \
      0u, (shape_), (width_) }

#define MSTORELANE(opcode_, desc_, width_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE, &(desc_), \
      0u, TURBOWASM_V128_RAW, (width_) }

#define LEXT(opcode_, desc_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_LANE_EXTRACT, &(desc_), \
      0u, TURBOWASM_V128_RAW, 0u }

#define LREP(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_LANE_REPLACE, &(desc_), \
      0u, (shape_), 0u }

#define SHUF(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SHUFFLE, &cmeta_vector_i8x16, \
      0u, TURBOWASM_V128_I8X16, 0u }

#define SWIZ(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SWIZZLE, &cmeta_vector_i8x16, \
      0u, TURBOWASM_V128_I8X16, 0u }

/*
 * This is intentionally an execution-capability table, not another
 * validation table. Every opcode here must also exist in
 * validate_simd_table.c. Missing entries remain fail-closed at execution.
 *
 * CMeta owns vector semantics; Salts owns portable execution. TurboWasm owns
 * only the Wasm-opcode mapping.
 */
static const turbowasm_simd_exec_descriptor descriptors[] = {
    /* SIMD memory transforms; baseline v128.load/store stay direct. */
    MEXT(0x01u, cmeta_vector_i16x8, 8u, TURBOWASM_V128_I16X8),
    MEXT(0x02u, cmeta_vector_u16x8, 8u, TURBOWASM_V128_U16X8),
    MEXT(0x03u, cmeta_vector_i32x4, 8u, TURBOWASM_V128_I32X4),
    MEXT(0x04u, cmeta_vector_u32x4, 8u, TURBOWASM_V128_U32X4),
    MEXT(0x05u, cmeta_vector_i64x2, 8u, TURBOWASM_V128_I64X2),
    MEXT(0x06u, cmeta_vector_u64x2, 8u, TURBOWASM_V128_U64X2),

    MSPLAT(0x07u, cmeta_vector_i8x16, 1u, TURBOWASM_V128_I8X16),
    MSPLAT(0x08u, cmeta_vector_i16x8, 2u, TURBOWASM_V128_I16X8),
    MSPLAT(0x09u, cmeta_vector_i32x4, 4u, TURBOWASM_V128_I32X4),
    MSPLAT(0x0au, cmeta_vector_i64x2, 8u, TURBOWASM_V128_I64X2),

    MLOADLANE(0x54u, cmeta_vector_u8x16, 1u, TURBOWASM_V128_I8X16),
    MLOADLANE(0x55u, cmeta_vector_u16x8, 2u, TURBOWASM_V128_I16X8),
    MLOADLANE(0x56u, cmeta_vector_u32x4, 4u, TURBOWASM_V128_I32X4),
    MLOADLANE(0x57u, cmeta_vector_u64x2, 8u, TURBOWASM_V128_I64X2),

    MSTORELANE(0x58u, cmeta_vector_u8x16, 1u),
    MSTORELANE(0x59u, cmeta_vector_u16x8, 2u),
    MSTORELANE(0x5au, cmeta_vector_u32x4, 4u),
    MSTORELANE(0x5bu, cmeta_vector_u64x2, 8u),

    MZERO(0x5cu, cmeta_vector_i32x4, 4u, TURBOWASM_V128_I32X4),
    MZERO(0x5du, cmeta_vector_i64x2, 8u, TURBOWASM_V128_I64X2),

    /* lane/shuffle forms */
    SHUF(0x0du),
    SWIZ(0x0eu),

    LEXT(0x15u, cmeta_vector_i8x16),
    LEXT(0x16u, cmeta_vector_u8x16),
    LREP(0x17u, cmeta_vector_i8x16, TURBOWASM_V128_I8X16),

    LEXT(0x18u, cmeta_vector_i16x8),
    LEXT(0x19u, cmeta_vector_u16x8),
    LREP(0x1au, cmeta_vector_i16x8, TURBOWASM_V128_I16X8),

    LEXT(0x1bu, cmeta_vector_i32x4),
    LREP(0x1cu, cmeta_vector_i32x4, TURBOWASM_V128_I32X4),

    LEXT(0x1du, cmeta_vector_i64x2),
    LREP(0x1eu, cmeta_vector_i64x2, TURBOWASM_V128_I64X2),

    LEXT(0x1fu, cmeta_vector_f32x4),
    LREP(0x20u, cmeta_vector_f32x4, TURBOWASM_V128_F32X4),

    LEXT(0x21u, cmeta_vector_f64x2),
    LREP(0x22u, cmeta_vector_f64x2, TURBOWASM_V128_F64X2),

    /* splat */
    SP(0x0fu, cmeta_vector_i8x16, TURBOWASM_V128_I8X16),
    SP(0x10u, cmeta_vector_i16x8, TURBOWASM_V128_I16X8),
    SP(0x11u, cmeta_vector_i32x4, TURBOWASM_V128_I32X4),
    SP(0x12u, cmeta_vector_i64x2, TURBOWASM_V128_I64X2),
    SP(0x13u, cmeta_vector_f32x4, TURBOWASM_V128_F32X4),
    SP(0x14u, cmeta_vector_f64x2, TURBOWASM_V128_F64X2),

    /* comparisons: i8x16 */
    CMP(0x23u, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B8X16),
    CMP(0x24u, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B8X16),
    CMP(0x25u, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B8X16),
    CMP(0x26u, cmeta_vector_u8x16, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B8X16),
    CMP(0x27u, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B8X16),
    CMP(0x28u, cmeta_vector_u8x16, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B8X16),
    CMP(0x29u, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B8X16),
    CMP(0x2au, cmeta_vector_u8x16, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B8X16),
    CMP(0x2bu, cmeta_vector_i8x16, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B8X16),
    CMP(0x2cu, cmeta_vector_u8x16, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B8X16),

    /* comparisons: i16x8 */
    CMP(0x2du, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B16X8),
    CMP(0x2eu, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B16X8),
    CMP(0x2fu, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B16X8),
    CMP(0x30u, cmeta_vector_u16x8, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B16X8),
    CMP(0x31u, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B16X8),
    CMP(0x32u, cmeta_vector_u16x8, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B16X8),
    CMP(0x33u, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B16X8),
    CMP(0x34u, cmeta_vector_u16x8, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B16X8),
    CMP(0x35u, cmeta_vector_i16x8, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B16X8),
    CMP(0x36u, cmeta_vector_u16x8, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B16X8),

    /* comparisons: i32x4 */
    CMP(0x37u, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B32X4),
    CMP(0x38u, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B32X4),
    CMP(0x39u, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x3au, cmeta_vector_u32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x3bu, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x3cu, cmeta_vector_u32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x3du, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x3eu, cmeta_vector_u32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x3fu, cmeta_vector_i32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),
    CMP(0x40u, cmeta_vector_u32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),

    /* comparisons: floating point */
    CMP(0x41u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B32X4),
    CMP(0x42u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B32X4),
    CMP(0x43u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x44u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x45u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x46u, cmeta_vector_f32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),
    CMP(0x47u, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B64X2),
    CMP(0x48u, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B64X2),
    CMP(0x49u, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B64X2),
    CMP(0x4au, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B64X2),
    CMP(0x4bu, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B64X2),
    CMP(0x4cu, cmeta_vector_f64x2, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B64X2),

    /* raw bitwise/select */
    UN(0x4du, cmeta_vector_i8x16, SALTS_SIMD_UNARY_NOT,
       TURBOWASM_V128_RAW),
    BIN(0x4eu, cmeta_vector_i8x16, SALTS_SIMD_BINARY_AND,
        TURBOWASM_V128_RAW),
    BIN(0x50u, cmeta_vector_i8x16, SALTS_SIMD_BINARY_OR,
        TURBOWASM_V128_RAW),
    BIN(0x51u, cmeta_vector_i8x16, SALTS_SIMD_BINARY_XOR,
        TURBOWASM_V128_RAW),
    SEL(0x52u),

    /* reductions */
    RED(0x53u, cmeta_vector_i8x16, SALTS_SIMD_REDUCE_ANY_TRUE),

    /* advanced i8x16 numeric */
    UN(0x60u, cmeta_vector_i8x16, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I8X16),
    UN(0x61u, cmeta_vector_i8x16, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I8X16),
    UN(0x62u, cmeta_vector_i8x16, SALTS_SIMD_UNARY_POPCOUNT,
       TURBOWASM_V128_I8X16),
    RED(0x63u, cmeta_vector_i8x16, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0x64u, cmeta_vector_i8x16, SALTS_SIMD_REDUCE_BITMASK),
    NAR(0x65u, cmeta_vector_i8x16, TURBOWASM_V128_I8X16),
    NAR(0x66u, cmeta_vector_u8x16, TURBOWASM_V128_U8X16),

    /* integer shifts and wrapping arithmetic */
    SH(0x6bu, cmeta_vector_i8x16, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I8X16),
    SH(0x6cu, cmeta_vector_i8x16, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I8X16),
    SH(0x6du, cmeta_vector_u8x16, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U8X16),
    BIN(0x6eu, cmeta_vector_i8x16, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I8X16),
    SAT(0x6fu, cmeta_vector_i8x16, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_I8X16),
    SAT(0x70u, cmeta_vector_u8x16, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_U8X16),
    BIN(0x71u, cmeta_vector_i8x16, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I8X16),
    SAT(0x72u, cmeta_vector_i8x16, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_I8X16),
    SAT(0x73u, cmeta_vector_u8x16, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_U8X16),
    BIN(0x76u, cmeta_vector_i8x16, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I8X16),
    BIN(0x77u, cmeta_vector_u8x16, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U8X16),
    BIN(0x78u, cmeta_vector_i8x16, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I8X16),
    BIN(0x79u, cmeta_vector_u8x16, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U8X16),
    PAIR(0x7cu, cmeta_vector_i16x8, TURBOWASM_V128_I16X8),
    PAIR(0x7du, cmeta_vector_u16x8, TURBOWASM_V128_U16X8),
    PAIR(0x7eu, cmeta_vector_i32x4, TURBOWASM_V128_I32X4),
    PAIR(0x7fu, cmeta_vector_u32x4, TURBOWASM_V128_U32X4),

    UN(0x80u, cmeta_vector_i16x8, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I16X8),
    UN(0x81u, cmeta_vector_i16x8, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I16X8),
    Q15(0x82u),
    RED(0x83u, cmeta_vector_i16x8, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0x84u, cmeta_vector_i16x8, SALTS_SIMD_REDUCE_BITMASK),
    NAR(0x85u, cmeta_vector_i16x8, TURBOWASM_V128_I16X8),
    NAR(0x86u, cmeta_vector_u16x8, TURBOWASM_V128_U16X8),
    EXH(0x87u, cmeta_vector_i16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I16X8),
    EXH(0x88u, cmeta_vector_i16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I16X8),
    EXH(0x89u, cmeta_vector_u16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U16X8),
    EXH(0x8au, cmeta_vector_u16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U16X8),

    SH(0x8bu, cmeta_vector_i16x8, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I16X8),
    SH(0x8cu, cmeta_vector_i16x8, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I16X8),
    SH(0x8du, cmeta_vector_u16x8, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U16X8),
    BIN(0x8eu, cmeta_vector_i16x8, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I16X8),
    SAT(0x8fu, cmeta_vector_i16x8, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_I16X8),
    SAT(0x90u, cmeta_vector_u16x8, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_U16X8),
    BIN(0x91u, cmeta_vector_i16x8, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I16X8),
    SAT(0x92u, cmeta_vector_i16x8, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_I16X8),
    SAT(0x93u, cmeta_vector_u16x8, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_U16X8),
    BIN(0x95u, cmeta_vector_i16x8, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I16X8),
    BIN(0x96u, cmeta_vector_i16x8, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I16X8),
    BIN(0x97u, cmeta_vector_u16x8, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U16X8),
    BIN(0x98u, cmeta_vector_i16x8, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I16X8),
    BIN(0x99u, cmeta_vector_u16x8, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U16X8),
    EMH(0x9cu, cmeta_vector_i16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I16X8),
    EMH(0x9du, cmeta_vector_i16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I16X8),
    EMH(0x9eu, cmeta_vector_u16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U16X8),
    EMH(0x9fu, cmeta_vector_u16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U16X8),

    UN(0xa0u, cmeta_vector_i32x4, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I32X4),
    UN(0xa1u, cmeta_vector_i32x4, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I32X4),
    RED(0xa3u, cmeta_vector_i32x4, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0xa4u, cmeta_vector_i32x4, SALTS_SIMD_REDUCE_BITMASK),
    EXH(0xa7u, cmeta_vector_i32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I32X4),
    EXH(0xa8u, cmeta_vector_i32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I32X4),
    EXH(0xa9u, cmeta_vector_u32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U32X4),
    EXH(0xaau, cmeta_vector_u32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U32X4),

    SH(0xabu, cmeta_vector_i32x4, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I32X4),
    SH(0xacu, cmeta_vector_i32x4, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I32X4),
    SH(0xadu, cmeta_vector_u32x4, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U32X4),
    BIN(0xaeu, cmeta_vector_i32x4, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I32X4),
    BIN(0xb1u, cmeta_vector_i32x4, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I32X4),
    BIN(0xb5u, cmeta_vector_i32x4, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I32X4),
    BIN(0xb6u, cmeta_vector_i32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I32X4),
    BIN(0xb7u, cmeta_vector_u32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U32X4),
    BIN(0xb8u, cmeta_vector_i32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I32X4),
    BIN(0xb9u, cmeta_vector_u32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U32X4),
    DOT(0xbau),
    EMH(0xbcu, cmeta_vector_i32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I32X4),
    EMH(0xbdu, cmeta_vector_i32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I32X4),
    EMH(0xbeu, cmeta_vector_u32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U32X4),
    EMH(0xbfu, cmeta_vector_u32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U32X4),

    UN(0xc0u, cmeta_vector_i64x2, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I64X2),
    UN(0xc1u, cmeta_vector_i64x2, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I64X2),
    RED(0xc3u, cmeta_vector_i64x2, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0xc4u, cmeta_vector_i64x2, SALTS_SIMD_REDUCE_BITMASK),
    EXH(0xc7u, cmeta_vector_i64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I64X2),
    EXH(0xc8u, cmeta_vector_i64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I64X2),
    EXH(0xc9u, cmeta_vector_u64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U64X2),
    EXH(0xcau, cmeta_vector_u64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U64X2),

    SH(0xcbu, cmeta_vector_i64x2, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I64X2),
    SH(0xccu, cmeta_vector_i64x2, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I64X2),
    SH(0xcdu, cmeta_vector_u64x2, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U64X2),
    BIN(0xceu, cmeta_vector_i64x2, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I64X2),
    BIN(0xd1u, cmeta_vector_i64x2, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I64X2),
    BIN(0xd5u, cmeta_vector_i64x2, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I64X2),

    CMP(0xd6u, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B64X2),
    CMP(0xd7u, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B64X2),
    CMP(0xd8u, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B64X2),
    CMP(0xd9u, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B64X2),
    CMP(0xdau, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B64X2),
    CMP(0xdbu, cmeta_vector_i64x2, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B64X2),
    EMH(0xdcu, cmeta_vector_i64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I64X2),
    EMH(0xddu, cmeta_vector_i64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I64X2),
    EMH(0xdeu, cmeta_vector_u64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U64X2),
    EMH(0xdfu, cmeta_vector_u64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U64X2),

    /* floating-point arithmetic */
    UN(0xe0u, cmeta_vector_f32x4, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_F32X4),
    UN(0xe1u, cmeta_vector_f32x4, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_F32X4),
    UN(0xe3u, cmeta_vector_f32x4, SALTS_SIMD_UNARY_SQRT,
       TURBOWASM_V128_F32X4),
    BIN(0xe4u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_F32X4),
    BIN(0xe5u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_F32X4),
    BIN(0xe6u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_F32X4),
    BIN(0xe7u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_DIV,
        TURBOWASM_V128_F32X4),
    BIN(0xe8u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_F32X4),
    BIN(0xe9u, cmeta_vector_f32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_F32X4),
    BIN(0xeau, cmeta_vector_f32x4, SALTS_SIMD_BINARY_PSEUDO_MIN,
        TURBOWASM_V128_F32X4),
    BIN(0xebu, cmeta_vector_f32x4, SALTS_SIMD_BINARY_PSEUDO_MAX,
        TURBOWASM_V128_F32X4),

    UN(0xecu, cmeta_vector_f64x2, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_F64X2),
    UN(0xedu, cmeta_vector_f64x2, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_F64X2),
    UN(0xefu, cmeta_vector_f64x2, SALTS_SIMD_UNARY_SQRT,
       TURBOWASM_V128_F64X2),

    BIN(0xf0u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_F64X2),
    BIN(0xf1u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_F64X2),
    BIN(0xf2u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_F64X2),
    BIN(0xf3u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_DIV,
        TURBOWASM_V128_F64X2),
    BIN(0xf4u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_F64X2),
    BIN(0xf5u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_F64X2),
    BIN(0xf6u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_PSEUDO_MIN,
        TURBOWASM_V128_F64X2),
    BIN(0xf7u, cmeta_vector_f64x2, SALTS_SIMD_BINARY_PSEUDO_MAX,
        TURBOWASM_V128_F64X2)
};

#undef SWIZ
#undef SHUF
#undef LREP
#undef LEXT
#undef MSTORELANE
#undef MLOADLANE
#undef MZERO
#undef MSPLAT
#undef MEXT
#undef DOT
#undef Q15
#undef PAIR
#undef EMH
#undef EXH
#undef NAR
#undef RED
#undef SAT
#undef SEL
#undef SH
#undef CMP
#undef BIN
#undef UN
#undef SP

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_find(uint32_t opcode) {
    size_t index;

    for (index = 0u;
         index < sizeof(descriptors) / sizeof(descriptors[0]);
         ++index) {
        if (descriptors[index].opcode == opcode)
            return &descriptors[index];
    }
    return NULL;
}

size_t turbowasm_simd_exec_descriptor_count(void) {
    return sizeof(descriptors) / sizeof(descriptors[0]);
}

const turbowasm_simd_exec_descriptor *
turbowasm_simd_exec_descriptor_at(size_t index) {
    if (index >= turbowasm_simd_exec_descriptor_count())
        return NULL;
    return &descriptors[index];
}
