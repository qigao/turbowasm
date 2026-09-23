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

    /* First generic Salts-backed family slice. */
    assert(count == 90u);

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

    descriptor = turbowasm_simd_exec_descriptor_find(0xf3u);
    assert(descriptor != NULL);
    assert(descriptor->kind == TURBOWASM_SIMD_EXEC_BINARY);
    assert(descriptor->vector_desc == &cmeta_vector_f64x2);
    assert(descriptor->op == SALTS_SIMD_BINARY_DIV);
    assert(descriptor->result_shape == TURBOWASM_V128_F64X2);

    /* Supported by validation but intentionally not by this execution slice. */
    assert(turbowasm_simd_descriptor_find(0x60u) != NULL);
    assert(turbowasm_simd_exec_descriptor_find(0x60u) == NULL);
}

int main(void) {
    test_execution_table_is_validation_subset();
    test_representative_semantics();
    return 0;
}
