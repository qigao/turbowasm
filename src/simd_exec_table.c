#include "simd_exec_table.h"

#define SP(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SPLAT, &(desc_), 0u, (shape_), 0u, NULL, 0u }

#define UN(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_UNARY, &(desc_), \
      (uint8_t)(op_), (shape_), 0u, NULL, 0u }

#define BIN(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_BINARY, &(desc_), \
      (uint8_t)(op_), (shape_), 0u, NULL, 0u }

#define CMP(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_COMPARE, &(desc_), \
      (uint8_t)(op_), (shape_), 0u, NULL, 0u }

#define SH(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SHIFT, &(desc_), \
      (uint8_t)(op_), (shape_), 0u, NULL, 0u }

#define SEL(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SELECT, &salts_simd_i8x16, \
      0u, TURBOWASM_V128_RAW, 0u, NULL, 0u }

#define SAT(opcode_, desc_, op_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SATURATING, &(desc_), \
      (uint8_t)(op_), (shape_), 0u, NULL, 0u }

#define RED(opcode_, desc_, op_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_REDUCE, &(desc_), \
      (uint8_t)(op_), TURBOWASM_V128_RAW, 0u, NULL, 0u }

#define NAR(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_NARROW, &(desc_), \
      0u, (shape_), 0u, NULL, 0u }

#define EXH(opcode_, desc_, half_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTEND_HALF, &(desc_), \
      (uint8_t)(half_), (shape_), 0u, NULL, 0u }

#define EMH(opcode_, desc_, half_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTMUL_HALF, &(desc_), \
      (uint8_t)(half_), (shape_), 0u, NULL, 0u }

#define PAIR(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE, &(desc_), \
      0u, (shape_), 0u, NULL, 0u }

#define Q15(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_Q15MULR, &salts_simd_i16x8, \
      0u, TURBOWASM_V128_I16X8, 0u, NULL, 0u }

#define DOT(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_DOT_PAIRWISE, &salts_simd_i32x4, \
      0u, TURBOWASM_V128_I32X4, 0u, NULL, 0u }

#define CONV(opcode_, dst_, src_, op_, policy_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_CONVERT, &(dst_), \
      (uint8_t)(op_), (shape_), 0u, &(src_), (uint8_t)(policy_) }

#define MEXT(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_EXTEND, &(desc_), \
      0u, (shape_), (width_), NULL, 0u }

#define MSPLAT(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_SPLAT, &(desc_), \
      0u, (shape_), (width_), NULL, 0u }

#define MZERO(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_ZERO, &(desc_), \
      0u, (shape_), (width_), NULL, 0u }

#define MLOADLANE(opcode_, desc_, width_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE, &(desc_), \
      0u, (shape_), (width_), NULL, 0u }

#define MSTORELANE(opcode_, desc_, width_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE, &(desc_), \
      0u, TURBOWASM_V128_RAW, (width_), NULL, 0u }

#define LEXT(opcode_, desc_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_LANE_EXTRACT, &(desc_), \
      0u, TURBOWASM_V128_RAW, 0u, NULL, 0u }

#define LREP(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_LANE_REPLACE, &(desc_), \
      0u, (shape_), 0u, NULL, 0u }

#define SHUF(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SHUFFLE, &salts_simd_i8x16, \
      0u, TURBOWASM_V128_I8X16, 0u, NULL, 0u }

#define SWIZ(opcode_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_SWIZZLE, &salts_simd_i8x16, \
      0u, TURBOWASM_V128_I8X16, 0u, NULL, 0u }

#define REL(opcode_, desc_, shape_) \
    { (opcode_), TURBOWASM_SIMD_EXEC_RELAXED, &(desc_), \
      0u, (shape_), 0u, NULL, 0u }

/*
 * This is intentionally an execution-capability table, not another
 * validation table. Every opcode here must also exist in
 * validate_simd_table.c. Missing entries remain fail-closed at execution.
 *
 * Salts::SIMD owns native lane semantics and portable execution; CMeta owns
 * reflection. TurboWasm owns only the Wasm-opcode mapping.
 */
static const turbowasm_simd_exec_descriptor descriptors[] = {
    /* SIMD memory transforms; baseline v128.load/store stay direct. */
    MEXT(0x01u, salts_simd_i16x8, 8u, TURBOWASM_V128_I16X8),
    MEXT(0x02u, salts_simd_u16x8, 8u, TURBOWASM_V128_U16X8),
    MEXT(0x03u, salts_simd_i32x4, 8u, TURBOWASM_V128_I32X4),
    MEXT(0x04u, salts_simd_u32x4, 8u, TURBOWASM_V128_U32X4),
    MEXT(0x05u, salts_simd_i64x2, 8u, TURBOWASM_V128_I64X2),
    MEXT(0x06u, salts_simd_u64x2, 8u, TURBOWASM_V128_U64X2),

    MSPLAT(0x07u, salts_simd_i8x16, 1u, TURBOWASM_V128_I8X16),
    MSPLAT(0x08u, salts_simd_i16x8, 2u, TURBOWASM_V128_I16X8),
    MSPLAT(0x09u, salts_simd_i32x4, 4u, TURBOWASM_V128_I32X4),
    MSPLAT(0x0au, salts_simd_i64x2, 8u, TURBOWASM_V128_I64X2),

    MLOADLANE(0x54u, salts_simd_u8x16, 1u, TURBOWASM_V128_I8X16),
    MLOADLANE(0x55u, salts_simd_u16x8, 2u, TURBOWASM_V128_I16X8),
    MLOADLANE(0x56u, salts_simd_u32x4, 4u, TURBOWASM_V128_I32X4),
    MLOADLANE(0x57u, salts_simd_u64x2, 8u, TURBOWASM_V128_I64X2),

    MSTORELANE(0x58u, salts_simd_u8x16, 1u),
    MSTORELANE(0x59u, salts_simd_u16x8, 2u),
    MSTORELANE(0x5au, salts_simd_u32x4, 4u),
    MSTORELANE(0x5bu, salts_simd_u64x2, 8u),

    MZERO(0x5cu, salts_simd_i32x4, 4u, TURBOWASM_V128_I32X4),
    MZERO(0x5du, salts_simd_i64x2, 8u, TURBOWASM_V128_I64X2),

    CONV(0x5eu, salts_simd_f32x4, salts_simd_f64x2,
         SALTS_SIMD_CONVERT_DEMOTE, SALTS_SIMD_LANES_LOW_ZERO,
         TURBOWASM_V128_F32X4),
    CONV(0x5fu, salts_simd_f64x2, salts_simd_f32x4,
         SALTS_SIMD_CONVERT_PROMOTE, SALTS_SIMD_LANES_LOW,
         TURBOWASM_V128_F64X2),

    /* lane/shuffle forms */
    SHUF(0x0du),
    SWIZ(0x0eu),

    LEXT(0x15u, salts_simd_i8x16),
    LEXT(0x16u, salts_simd_u8x16),
    LREP(0x17u, salts_simd_i8x16, TURBOWASM_V128_I8X16),

    LEXT(0x18u, salts_simd_i16x8),
    LEXT(0x19u, salts_simd_u16x8),
    LREP(0x1au, salts_simd_i16x8, TURBOWASM_V128_I16X8),

    LEXT(0x1bu, salts_simd_i32x4),
    LREP(0x1cu, salts_simd_i32x4, TURBOWASM_V128_I32X4),

    LEXT(0x1du, salts_simd_i64x2),
    LREP(0x1eu, salts_simd_i64x2, TURBOWASM_V128_I64X2),

    LEXT(0x1fu, salts_simd_f32x4),
    LREP(0x20u, salts_simd_f32x4, TURBOWASM_V128_F32X4),

    LEXT(0x21u, salts_simd_f64x2),
    LREP(0x22u, salts_simd_f64x2, TURBOWASM_V128_F64X2),

    /* splat */
    SP(0x0fu, salts_simd_i8x16, TURBOWASM_V128_I8X16),
    SP(0x10u, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    SP(0x11u, salts_simd_i32x4, TURBOWASM_V128_I32X4),
    SP(0x12u, salts_simd_i64x2, TURBOWASM_V128_I64X2),
    SP(0x13u, salts_simd_f32x4, TURBOWASM_V128_F32X4),
    SP(0x14u, salts_simd_f64x2, TURBOWASM_V128_F64X2),

    /* comparisons: i8x16 */
    CMP(0x23u, salts_simd_i8x16, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B8X16),
    CMP(0x24u, salts_simd_i8x16, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B8X16),
    CMP(0x25u, salts_simd_i8x16, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B8X16),
    CMP(0x26u, salts_simd_u8x16, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B8X16),
    CMP(0x27u, salts_simd_i8x16, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B8X16),
    CMP(0x28u, salts_simd_u8x16, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B8X16),
    CMP(0x29u, salts_simd_i8x16, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B8X16),
    CMP(0x2au, salts_simd_u8x16, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B8X16),
    CMP(0x2bu, salts_simd_i8x16, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B8X16),
    CMP(0x2cu, salts_simd_u8x16, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B8X16),

    /* comparisons: i16x8 */
    CMP(0x2du, salts_simd_i16x8, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B16X8),
    CMP(0x2eu, salts_simd_i16x8, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B16X8),
    CMP(0x2fu, salts_simd_i16x8, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B16X8),
    CMP(0x30u, salts_simd_u16x8, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B16X8),
    CMP(0x31u, salts_simd_i16x8, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B16X8),
    CMP(0x32u, salts_simd_u16x8, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B16X8),
    CMP(0x33u, salts_simd_i16x8, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B16X8),
    CMP(0x34u, salts_simd_u16x8, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B16X8),
    CMP(0x35u, salts_simd_i16x8, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B16X8),
    CMP(0x36u, salts_simd_u16x8, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B16X8),

    /* comparisons: i32x4 */
    CMP(0x37u, salts_simd_i32x4, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B32X4),
    CMP(0x38u, salts_simd_i32x4, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B32X4),
    CMP(0x39u, salts_simd_i32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x3au, salts_simd_u32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x3bu, salts_simd_i32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x3cu, salts_simd_u32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x3du, salts_simd_i32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x3eu, salts_simd_u32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x3fu, salts_simd_i32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),
    CMP(0x40u, salts_simd_u32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),

    /* comparisons: floating point */
    CMP(0x41u, salts_simd_f32x4, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B32X4),
    CMP(0x42u, salts_simd_f32x4, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B32X4),
    CMP(0x43u, salts_simd_f32x4, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B32X4),
    CMP(0x44u, salts_simd_f32x4, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B32X4),
    CMP(0x45u, salts_simd_f32x4, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B32X4),
    CMP(0x46u, salts_simd_f32x4, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B32X4),
    CMP(0x47u, salts_simd_f64x2, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B64X2),
    CMP(0x48u, salts_simd_f64x2, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B64X2),
    CMP(0x49u, salts_simd_f64x2, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B64X2),
    CMP(0x4au, salts_simd_f64x2, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B64X2),
    CMP(0x4bu, salts_simd_f64x2, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B64X2),
    CMP(0x4cu, salts_simd_f64x2, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B64X2),

    /* raw bitwise/select */
    UN(0x4du, salts_simd_i8x16, SALTS_SIMD_UNARY_NOT,
       TURBOWASM_V128_RAW),
    BIN(0x4eu, salts_simd_i8x16, SALTS_SIMD_BINARY_AND,
        TURBOWASM_V128_RAW),
    BIN(0x4fu, salts_simd_i8x16, SALTS_SIMD_BINARY_AND_NOT,
        TURBOWASM_V128_RAW),
    BIN(0x50u, salts_simd_i8x16, SALTS_SIMD_BINARY_OR,
        TURBOWASM_V128_RAW),
    BIN(0x51u, salts_simd_i8x16, SALTS_SIMD_BINARY_XOR,
        TURBOWASM_V128_RAW),
    SEL(0x52u),

    /* reductions */
    RED(0x53u, salts_simd_i8x16, SALTS_SIMD_REDUCE_ANY_TRUE),

    /* advanced i8x16 numeric */
    UN(0x60u, salts_simd_i8x16, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I8X16),
    UN(0x61u, salts_simd_i8x16, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I8X16),
    UN(0x62u, salts_simd_i8x16, SALTS_SIMD_UNARY_POPCOUNT,
       TURBOWASM_V128_I8X16),
    RED(0x63u, salts_simd_i8x16, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0x64u, salts_simd_i8x16, SALTS_SIMD_REDUCE_BITMASK),
    NAR(0x65u, salts_simd_i8x16, TURBOWASM_V128_I8X16),
    NAR(0x66u, salts_simd_u8x16, TURBOWASM_V128_U8X16),
    UN(0x67u, salts_simd_f32x4, SALTS_SIMD_UNARY_CEIL,
       TURBOWASM_V128_F32X4),
    UN(0x68u, salts_simd_f32x4, SALTS_SIMD_UNARY_FLOOR,
       TURBOWASM_V128_F32X4),
    UN(0x69u, salts_simd_f32x4, SALTS_SIMD_UNARY_TRUNC,
       TURBOWASM_V128_F32X4),
    UN(0x6au, salts_simd_f32x4, SALTS_SIMD_UNARY_NEAREST,
       TURBOWASM_V128_F32X4),

    /* integer shifts and wrapping arithmetic */
    SH(0x6bu, salts_simd_i8x16, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I8X16),
    SH(0x6cu, salts_simd_i8x16, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I8X16),
    SH(0x6du, salts_simd_u8x16, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U8X16),
    BIN(0x6eu, salts_simd_i8x16, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I8X16),
    SAT(0x6fu, salts_simd_i8x16, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_I8X16),
    SAT(0x70u, salts_simd_u8x16, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_U8X16),
    BIN(0x71u, salts_simd_i8x16, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I8X16),
    SAT(0x72u, salts_simd_i8x16, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_I8X16),
    SAT(0x73u, salts_simd_u8x16, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_U8X16),
    BIN(0x76u, salts_simd_i8x16, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I8X16),
    BIN(0x77u, salts_simd_u8x16, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U8X16),
    BIN(0x78u, salts_simd_i8x16, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I8X16),
    BIN(0x79u, salts_simd_u8x16, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U8X16),
    UN(0x74u, salts_simd_f64x2, SALTS_SIMD_UNARY_CEIL,
       TURBOWASM_V128_F64X2),
    UN(0x75u, salts_simd_f64x2, SALTS_SIMD_UNARY_FLOOR,
       TURBOWASM_V128_F64X2),
    UN(0x7au, salts_simd_f64x2, SALTS_SIMD_UNARY_TRUNC,
       TURBOWASM_V128_F64X2),
    BIN(0x7bu, salts_simd_u8x16,
        SALTS_SIMD_BINARY_AVERAGE_ROUND_UNSIGNED,
        TURBOWASM_V128_U8X16),
    PAIR(0x7cu, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    PAIR(0x7du, salts_simd_u16x8, TURBOWASM_V128_U16X8),
    PAIR(0x7eu, salts_simd_i32x4, TURBOWASM_V128_I32X4),
    PAIR(0x7fu, salts_simd_u32x4, TURBOWASM_V128_U32X4),

    UN(0x80u, salts_simd_i16x8, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I16X8),
    UN(0x81u, salts_simd_i16x8, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I16X8),
    Q15(0x82u),
    RED(0x83u, salts_simd_i16x8, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0x84u, salts_simd_i16x8, SALTS_SIMD_REDUCE_BITMASK),
    NAR(0x85u, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    NAR(0x86u, salts_simd_u16x8, TURBOWASM_V128_U16X8),
    EXH(0x87u, salts_simd_i16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I16X8),
    EXH(0x88u, salts_simd_i16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I16X8),
    EXH(0x89u, salts_simd_u16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U16X8),
    EXH(0x8au, salts_simd_u16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U16X8),

    SH(0x8bu, salts_simd_i16x8, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I16X8),
    SH(0x8cu, salts_simd_i16x8, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I16X8),
    SH(0x8du, salts_simd_u16x8, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U16X8),
    BIN(0x8eu, salts_simd_i16x8, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I16X8),
    SAT(0x8fu, salts_simd_i16x8, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_I16X8),
    SAT(0x90u, salts_simd_u16x8, SALTS_SIMD_SATURATING_ADD,
        TURBOWASM_V128_U16X8),
    BIN(0x91u, salts_simd_i16x8, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I16X8),
    SAT(0x92u, salts_simd_i16x8, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_I16X8),
    SAT(0x93u, salts_simd_u16x8, SALTS_SIMD_SATURATING_SUB,
        TURBOWASM_V128_U16X8),
    BIN(0x95u, salts_simd_i16x8, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I16X8),
    BIN(0x96u, salts_simd_i16x8, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I16X8),
    BIN(0x97u, salts_simd_u16x8, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U16X8),
    BIN(0x98u, salts_simd_i16x8, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I16X8),
    BIN(0x99u, salts_simd_u16x8, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U16X8),
    UN(0x94u, salts_simd_f64x2, SALTS_SIMD_UNARY_NEAREST,
       TURBOWASM_V128_F64X2),
    BIN(0x9bu, salts_simd_u16x8,
        SALTS_SIMD_BINARY_AVERAGE_ROUND_UNSIGNED,
        TURBOWASM_V128_U16X8),
    EMH(0x9cu, salts_simd_i16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I16X8),
    EMH(0x9du, salts_simd_i16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I16X8),
    EMH(0x9eu, salts_simd_u16x8, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U16X8),
    EMH(0x9fu, salts_simd_u16x8, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U16X8),

    UN(0xa0u, salts_simd_i32x4, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I32X4),
    UN(0xa1u, salts_simd_i32x4, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I32X4),
    RED(0xa3u, salts_simd_i32x4, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0xa4u, salts_simd_i32x4, SALTS_SIMD_REDUCE_BITMASK),
    EXH(0xa7u, salts_simd_i32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I32X4),
    EXH(0xa8u, salts_simd_i32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I32X4),
    EXH(0xa9u, salts_simd_u32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U32X4),
    EXH(0xaau, salts_simd_u32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U32X4),

    SH(0xabu, salts_simd_i32x4, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I32X4),
    SH(0xacu, salts_simd_i32x4, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I32X4),
    SH(0xadu, salts_simd_u32x4, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U32X4),
    BIN(0xaeu, salts_simd_i32x4, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I32X4),
    BIN(0xb1u, salts_simd_i32x4, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I32X4),
    BIN(0xb5u, salts_simd_i32x4, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I32X4),
    BIN(0xb6u, salts_simd_i32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_I32X4),
    BIN(0xb7u, salts_simd_u32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_U32X4),
    BIN(0xb8u, salts_simd_i32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_I32X4),
    BIN(0xb9u, salts_simd_u32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_U32X4),
    DOT(0xbau),
    EMH(0xbcu, salts_simd_i32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I32X4),
    EMH(0xbdu, salts_simd_i32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I32X4),
    EMH(0xbeu, salts_simd_u32x4, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U32X4),
    EMH(0xbfu, salts_simd_u32x4, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U32X4),

    UN(0xc0u, salts_simd_i64x2, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_I64X2),
    UN(0xc1u, salts_simd_i64x2, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_I64X2),
    RED(0xc3u, salts_simd_i64x2, SALTS_SIMD_REDUCE_ALL_TRUE),
    RED(0xc4u, salts_simd_i64x2, SALTS_SIMD_REDUCE_BITMASK),
    EXH(0xc7u, salts_simd_i64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I64X2),
    EXH(0xc8u, salts_simd_i64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I64X2),
    EXH(0xc9u, salts_simd_u64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U64X2),
    EXH(0xcau, salts_simd_u64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U64X2),

    SH(0xcbu, salts_simd_i64x2, SALTS_SIMD_SHIFT_LEFT,
       TURBOWASM_V128_I64X2),
    SH(0xccu, salts_simd_i64x2, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_I64X2),
    SH(0xcdu, salts_simd_u64x2, SALTS_SIMD_SHIFT_RIGHT,
       TURBOWASM_V128_U64X2),
    BIN(0xceu, salts_simd_i64x2, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_I64X2),
    BIN(0xd1u, salts_simd_i64x2, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_I64X2),
    BIN(0xd5u, salts_simd_i64x2, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_I64X2),

    CMP(0xd6u, salts_simd_i64x2, SALTS_SIMD_COMPARE_EQ,
        TURBOWASM_V128_B64X2),
    CMP(0xd7u, salts_simd_i64x2, SALTS_SIMD_COMPARE_NE,
        TURBOWASM_V128_B64X2),
    CMP(0xd8u, salts_simd_i64x2, SALTS_SIMD_COMPARE_LT,
        TURBOWASM_V128_B64X2),
    CMP(0xd9u, salts_simd_i64x2, SALTS_SIMD_COMPARE_GT,
        TURBOWASM_V128_B64X2),
    CMP(0xdau, salts_simd_i64x2, SALTS_SIMD_COMPARE_LE,
        TURBOWASM_V128_B64X2),
    CMP(0xdbu, salts_simd_i64x2, SALTS_SIMD_COMPARE_GE,
        TURBOWASM_V128_B64X2),
    EMH(0xdcu, salts_simd_i64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_I64X2),
    EMH(0xddu, salts_simd_i64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_I64X2),
    EMH(0xdeu, salts_simd_u64x2, SALTS_SIMD_HALF_LOW,
        TURBOWASM_V128_U64X2),
    EMH(0xdfu, salts_simd_u64x2, SALTS_SIMD_HALF_HIGH,
        TURBOWASM_V128_U64X2),

    /* floating-point arithmetic */
    UN(0xe0u, salts_simd_f32x4, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_F32X4),
    UN(0xe1u, salts_simd_f32x4, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_F32X4),
    UN(0xe3u, salts_simd_f32x4, SALTS_SIMD_UNARY_SQRT,
       TURBOWASM_V128_F32X4),
    BIN(0xe4u, salts_simd_f32x4, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_F32X4),
    BIN(0xe5u, salts_simd_f32x4, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_F32X4),
    BIN(0xe6u, salts_simd_f32x4, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_F32X4),
    BIN(0xe7u, salts_simd_f32x4, SALTS_SIMD_BINARY_DIV,
        TURBOWASM_V128_F32X4),
    BIN(0xe8u, salts_simd_f32x4, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_F32X4),
    BIN(0xe9u, salts_simd_f32x4, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_F32X4),
    BIN(0xeau, salts_simd_f32x4, SALTS_SIMD_BINARY_PSEUDO_MIN,
        TURBOWASM_V128_F32X4),
    BIN(0xebu, salts_simd_f32x4, SALTS_SIMD_BINARY_PSEUDO_MAX,
        TURBOWASM_V128_F32X4),

    UN(0xecu, salts_simd_f64x2, SALTS_SIMD_UNARY_ABS,
       TURBOWASM_V128_F64X2),
    UN(0xedu, salts_simd_f64x2, SALTS_SIMD_UNARY_NEG,
       TURBOWASM_V128_F64X2),
    UN(0xefu, salts_simd_f64x2, SALTS_SIMD_UNARY_SQRT,
       TURBOWASM_V128_F64X2),

    BIN(0xf0u, salts_simd_f64x2, SALTS_SIMD_BINARY_ADD,
        TURBOWASM_V128_F64X2),
    BIN(0xf1u, salts_simd_f64x2, SALTS_SIMD_BINARY_SUB,
        TURBOWASM_V128_F64X2),
    BIN(0xf2u, salts_simd_f64x2, SALTS_SIMD_BINARY_MUL,
        TURBOWASM_V128_F64X2),
    BIN(0xf3u, salts_simd_f64x2, SALTS_SIMD_BINARY_DIV,
        TURBOWASM_V128_F64X2),
    BIN(0xf4u, salts_simd_f64x2, SALTS_SIMD_BINARY_MIN,
        TURBOWASM_V128_F64X2),
    BIN(0xf5u, salts_simd_f64x2, SALTS_SIMD_BINARY_MAX,
        TURBOWASM_V128_F64X2),
    BIN(0xf6u, salts_simd_f64x2, SALTS_SIMD_BINARY_PSEUDO_MIN,
        TURBOWASM_V128_F64X2),
    BIN(0xf7u, salts_simd_f64x2, SALTS_SIMD_BINARY_PSEUDO_MAX,
        TURBOWASM_V128_F64X2),

    CONV(0xf8u, salts_simd_i32x4, salts_simd_f32x4,
         SALTS_SIMD_CONVERT_TRUNC_SAT, SALTS_SIMD_LANES_FULL,
         TURBOWASM_V128_I32X4),
    CONV(0xf9u, salts_simd_u32x4, salts_simd_f32x4,
         SALTS_SIMD_CONVERT_TRUNC_SAT, SALTS_SIMD_LANES_FULL,
         TURBOWASM_V128_U32X4),
    CONV(0xfau, salts_simd_f32x4, salts_simd_i32x4,
         SALTS_SIMD_CONVERT_NUMERIC, SALTS_SIMD_LANES_FULL,
         TURBOWASM_V128_F32X4),
    CONV(0xfbu, salts_simd_f32x4, salts_simd_u32x4,
         SALTS_SIMD_CONVERT_NUMERIC, SALTS_SIMD_LANES_FULL,
         TURBOWASM_V128_F32X4),
    CONV(0xfcu, salts_simd_i32x4, salts_simd_f64x2,
         SALTS_SIMD_CONVERT_TRUNC_SAT, SALTS_SIMD_LANES_LOW_ZERO,
         TURBOWASM_V128_I32X4),
    CONV(0xfdu, salts_simd_u32x4, salts_simd_f64x2,
         SALTS_SIMD_CONVERT_TRUNC_SAT, SALTS_SIMD_LANES_LOW_ZERO,
         TURBOWASM_V128_U32X4),
    CONV(0xfeu, salts_simd_f64x2, salts_simd_i32x4,
         SALTS_SIMD_CONVERT_NUMERIC, SALTS_SIMD_LANES_LOW,
         TURBOWASM_V128_F64X2),
    CONV(0xffu, salts_simd_f64x2, salts_simd_u32x4,
         SALTS_SIMD_CONVERT_NUMERIC, SALTS_SIMD_LANES_LOW,
         TURBOWASM_V128_F64X2),

    /* Relaxed SIMD: each descriptor chooses one proposal-permitted projection. */
    REL(0x100u, salts_simd_i8x16, TURBOWASM_V128_I8X16),
    REL(0x101u, salts_simd_i32x4, TURBOWASM_V128_I32X4),
    REL(0x102u, salts_simd_u32x4, TURBOWASM_V128_U32X4),
    REL(0x103u, salts_simd_i32x4, TURBOWASM_V128_I32X4),
    REL(0x104u, salts_simd_u32x4, TURBOWASM_V128_U32X4),
    REL(0x105u, salts_simd_f32x4, TURBOWASM_V128_F32X4),
    REL(0x106u, salts_simd_f32x4, TURBOWASM_V128_F32X4),
    REL(0x107u, salts_simd_f64x2, TURBOWASM_V128_F64X2),
    REL(0x108u, salts_simd_f64x2, TURBOWASM_V128_F64X2),
    REL(0x109u, salts_simd_i8x16, TURBOWASM_V128_I8X16),
    REL(0x10au, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    REL(0x10bu, salts_simd_i32x4, TURBOWASM_V128_I32X4),
    REL(0x10cu, salts_simd_i64x2, TURBOWASM_V128_I64X2),
    REL(0x10du, salts_simd_f32x4, TURBOWASM_V128_F32X4),
    REL(0x10eu, salts_simd_f32x4, TURBOWASM_V128_F32X4),
    REL(0x10fu, salts_simd_f64x2, TURBOWASM_V128_F64X2),
    REL(0x110u, salts_simd_f64x2, TURBOWASM_V128_F64X2),
    REL(0x111u, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    REL(0x112u, salts_simd_i16x8, TURBOWASM_V128_I16X8),
    REL(0x113u, salts_simd_i32x4, TURBOWASM_V128_I32X4)
};

#undef REL
#undef SWIZ
#undef SHUF
#undef LREP
#undef LEXT
#undef MSTORELANE
#undef MLOADLANE
#undef MZERO
#undef MSPLAT
#undef MEXT
#undef CONV
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
