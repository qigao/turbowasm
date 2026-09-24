#include "simd_exec_table.h"
#include "validate_simd_table.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void test_execution_table_is_validation_subset(void) {
    size_t count = turbowasm_simd_exec_descriptor_count();
    size_t index;
    size_t other;

    /* 221 prior mappings + q15 and dot mappings. */
    assert(count == 223u);

    for (index = 0u; index < count; ++index) {
        const turbowasm_simd_exec_descriptor *descriptor =
            turbowasm_simd_exec_descriptor_at(index);

        assert(descriptor != NULL);
        assert(descriptor->vector_desc != NULL);
        assert(cmeta_vector_desc_valid(descriptor->vector_desc));
        assert(turbowasm_simd_descriptor_find(
                   descriptor->opcode) != NULL);

        for (other = index + 1u; other < count; ++other) {
            const turbowasm_simd_exec_descriptor *candidate =
                turbowasm_simd_exec_descriptor_at(other);
            assert(candidate != NULL);
            assert(candidate->opcode != descriptor->opcode);
        }
    }

    assert(turbowasm_simd_exec_descriptor_at(count) == NULL);
}

static void test_representative_semantics(void) {
    const turbowasm_simd_exec_descriptor *descriptor;

    descriptor = turbowasm_simd_exec_descriptor_find(0x01u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_EXTEND);
    assert(descriptor->vector_desc == &cmeta_vector_i16x8);
    assert(descriptor->memory_width == 8u);
    assert(descriptor->result_shape == TURBOWASM_V128_I16X8);

    descriptor = turbowasm_simd_exec_descriptor_find(0x57u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE);
    assert(descriptor->vector_desc == &cmeta_vector_u64x2);
    assert(descriptor->memory_width == 8u);
    assert(descriptor->result_shape == TURBOWASM_V128_I64X2);

    descriptor = turbowasm_simd_exec_descriptor_find(0x5bu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE);
    assert(descriptor->vector_desc == &cmeta_vector_u64x2);
    assert(descriptor->memory_width == 8u);

    descriptor = turbowasm_simd_exec_descriptor_find(0x0du);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_SHUFFLE);
    assert(descriptor->result_shape == TURBOWASM_V128_I8X16);

    descriptor = turbowasm_simd_exec_descriptor_find(0x16u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_LANE_EXTRACT);
    assert(descriptor->vector_desc == &cmeta_vector_u8x16);

    descriptor = turbowasm_simd_exec_descriptor_find(0x20u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_LANE_REPLACE);
    assert(descriptor->vector_desc == &cmeta_vector_f32x4);
    assert(descriptor->result_shape == TURBOWASM_V128_F32X4);

    descriptor = turbowasm_simd_exec_descriptor_find(0x26u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_COMPARE);
    assert(descriptor->vector_desc == &cmeta_vector_u8x16);
    assert(descriptor->op == SALTS_SIMD_COMPARE_LT);
    assert(descriptor->result_shape == TURBOWASM_V128_B8X16);

    descriptor = turbowasm_simd_exec_descriptor_find(0xadu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_SHIFT);
    assert(descriptor->vector_desc == &cmeta_vector_u32x4);
    assert(descriptor->op == SALTS_SIMD_SHIFT_RIGHT);
    assert(descriptor->result_shape == TURBOWASM_V128_U32X4);

    descriptor = turbowasm_simd_exec_descriptor_find(0x53u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_REDUCE);
    assert(descriptor->op == SALTS_SIMD_REDUCE_ANY_TRUE);

    descriptor = turbowasm_simd_exec_descriptor_find(0x6fu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_SATURATING);
    assert(descriptor->vector_desc == &cmeta_vector_i8x16);
    assert(descriptor->op == SALTS_SIMD_SATURATING_ADD);

    descriptor = turbowasm_simd_exec_descriptor_find(0xe3u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_UNARY);
    assert(descriptor->vector_desc == &cmeta_vector_f32x4);
    assert(descriptor->op == SALTS_SIMD_UNARY_SQRT);

    descriptor = turbowasm_simd_exec_descriptor_find(0xeau);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_BINARY);
    assert(descriptor->vector_desc == &cmeta_vector_f32x4);
    assert(descriptor->op == SALTS_SIMD_BINARY_PSEUDO_MIN);
    assert(descriptor->result_shape == TURBOWASM_V128_F32X4);

    descriptor = turbowasm_simd_exec_descriptor_find(0xf7u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_BINARY);
    assert(descriptor->vector_desc == &cmeta_vector_f64x2);
    assert(descriptor->op == SALTS_SIMD_BINARY_PSEUDO_MAX);
    assert(descriptor->result_shape == TURBOWASM_V128_F64X2);

    descriptor = turbowasm_simd_exec_descriptor_find(0x65u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_NARROW);
    assert(descriptor->vector_desc == &cmeta_vector_i8x16);

    descriptor = turbowasm_simd_exec_descriptor_find(0x88u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_EXTEND_HALF);
    assert(descriptor->vector_desc == &cmeta_vector_i16x8);
    assert(descriptor->op == SALTS_SIMD_HALF_HIGH);

    descriptor = turbowasm_simd_exec_descriptor_find(0x9eu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_EXTMUL_HALF);
    assert(descriptor->vector_desc == &cmeta_vector_u16x8);
    assert(descriptor->op == SALTS_SIMD_HALF_LOW);

    descriptor = turbowasm_simd_exec_descriptor_find(0x7eu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE);
    assert(descriptor->vector_desc == &cmeta_vector_i32x4);

    descriptor = turbowasm_simd_exec_descriptor_find(0x4fu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_BINARY);
    assert(descriptor->op == SALTS_SIMD_BINARY_AND_NOT);

    descriptor = turbowasm_simd_exec_descriptor_find(0x67u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_UNARY);
    assert(descriptor->vector_desc == &cmeta_vector_f32x4);
    assert(descriptor->op == SALTS_SIMD_UNARY_CEIL);

    descriptor = turbowasm_simd_exec_descriptor_find(0x9bu);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_BINARY);
    assert(descriptor->vector_desc == &cmeta_vector_u16x8);
    assert(descriptor->op == SALTS_SIMD_BINARY_AVERAGE_ROUND_UNSIGNED);

    descriptor = turbowasm_simd_exec_descriptor_find(0x82u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_Q15MULR);
    assert(descriptor->vector_desc == &cmeta_vector_i16x8);

    descriptor = turbowasm_simd_exec_descriptor_find(0xbau);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_DOT_PAIRWISE);
    assert(descriptor->vector_desc == &cmeta_vector_i32x4);

    /* conversions remain intentionally deferred under #54. */
    assert(turbowasm_simd_descriptor_find(0xf8u) != NULL);
    assert(turbowasm_simd_exec_descriptor_find(0xf8u) == NULL);
}

int main(void) {
    test_execution_table_is_validation_subset();
    test_representative_semantics();
    return 0;
}
