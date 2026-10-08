#include "../src/instance_internal.h"
#include "../src/jit_simd_helper.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void reset_status(turbowasm_jit_invocation_context *context) {
    context->call_status = TURBOWASM_OK;
    context->call_trap = TURBOWASM_TRAP_NONE;
}

static void test_slot_ops_and_reduce(void) {
    turbowasm_v128 slots[4] = {0};
    turbowasm_jit_invocation_context context = {0};
    uint8_t raw[16] = {0};
    int32_t lanes[4] = {0};
    int64_t reduced;

    context.simd_slots = slots;
    context.simd_slot_count = 4u;
    reset_status(&context);

    assert(turbowasm_jit_simd_const(
               &context, 0,
               INT64_C(0x0706050403020100),
               INT64_C(0x0f0e0d0c0b0a0908)) ==
           TURBOWASM_OK);
    cmeta_simd_v128_store(raw, &slots[0].bits);
    for (uint32_t index = 0u; index < 16u; ++index)
        assert(raw[index] == (uint8_t)index);

    assert(turbowasm_jit_simd_splat_i64(
               &context, 0x11, 1, 7) == TURBOWASM_OK);
    cmeta_simd_v128_store(lanes, &slots[1].bits);
    assert(lanes[0] == 7 && lanes[1] == 7 &&
           lanes[2] == 7 && lanes[3] == 7);

    assert(turbowasm_jit_simd_op(
               &context,
               0xae, /* i32x4.add */
               2, 1, 1, -1, 0) == TURBOWASM_OK);
    cmeta_simd_v128_store(lanes, &slots[2].bits);
    assert(lanes[0] == 14 && lanes[1] == 14 &&
           lanes[2] == 14 && lanes[3] == 14);

    assert(turbowasm_jit_simd_op(
               &context,
               0x37, /* i32x4.eq */
               3, 2, 2, -1, 0) == TURBOWASM_OK);

    reduced = turbowasm_jit_simd_reduce(
        &context,
        0xa4, /* i32x4.bitmask */
        3);
    assert(reduced == 15);
    assert(context.call_status == TURBOWASM_OK);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_slot_copy(void) {
    turbowasm_v128 slots[3] = {0};
    turbowasm_jit_invocation_context context = {0};
    uint8_t expected[16];
    uint8_t actual[16] = {0};
    uint32_t index;

    for (index = 0u; index < 16u; ++index)
        expected[index] = (uint8_t)(index * 7u + 3u);

    context.simd_slots = slots;
    context.simd_slot_count = 3u;
    reset_status(&context);

    cmeta_simd_v128_load(&slots[0].bits, expected);
    assert(turbowasm_jit_simd_copy(
               &context, 2, 0) == TURBOWASM_OK);
    cmeta_simd_v128_store(actual, &slots[2].bits);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    assert(context.call_status == TURBOWASM_OK);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);

    reset_status(&context);
    assert(turbowasm_jit_simd_copy(
               &context, 3, 0) ==
           TURBOWASM_INVALID_ARGUMENT);
    assert(context.call_status ==
           TURBOWASM_INVALID_ARGUMENT);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_memory_helper_and_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* memory0 min=1, memory1 min=1 */
        0x05, 0x05, 0x02, 0x00, 0x01, 0x00, 0x01
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_v128 slots[2] = {0};
    turbowasm_jit_invocation_context context = {0};
    int32_t input[4] = {1, 2, 3, 4};
    int32_t output[4] = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    context.instance =
        (turbowasm_instance_impl *)instance.impl;
    context.simd_slots = slots;
    context.simd_slot_count = 2u;
    reset_status(&context);

    cmeta_simd_v128_load(&slots[0].bits, input);

    assert(turbowasm_jit_simd_memory(
               &context,
               0x0b, /* v128.store */
               0, 0, 0, 0) == TURBOWASM_OK);
    assert(turbowasm_jit_simd_memory(
               &context,
               0x00, /* v128.load */
               0, 1, 0, 0) == TURBOWASM_OK);

    cmeta_simd_v128_store(output, &slots[1].bits);
    assert(memcmp(input, output, sizeof(input)) == 0);

    {
        int32_t input1[4] = {5, 6, 7, 8};
        memset(output, 0, sizeof(output));
        cmeta_simd_v128_load(&slots[0].bits, input1);
        assert(turbowasm_jit_simd_memory(
                   &context,
                   0x0b, /* v128.store */
                   1, 0, 0, 0) == TURBOWASM_OK);
        assert(turbowasm_jit_simd_memory(
                   &context,
                   0x00, /* v128.load */
                   1, 1, 0, 0) == TURBOWASM_OK);
        cmeta_simd_v128_store(output, &slots[1].bits);
        assert(memcmp(input1, output, sizeof(input1)) == 0);
    }

    reset_status(&context);
    assert(turbowasm_jit_simd_memory(
               &context,
               0x00,
               0, 1, 65530, 0) == TURBOWASM_TRAPPED);
    assert(context.call_status == TURBOWASM_TRAPPED);
    assert(context.call_trap ==
           TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_invalid_slot_is_invalid_argument(void) {
    turbowasm_v128 slots[1] = {0};
    turbowasm_jit_invocation_context context = {0};

    context.simd_slots = slots;
    context.simd_slot_count = 1u;
    reset_status(&context);

    assert(turbowasm_jit_simd_const(
               &context, 1, 0, 0) ==
           TURBOWASM_INVALID_ARGUMENT);
    assert(context.call_status == TURBOWASM_INVALID_ARGUMENT);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_success_does_not_set_trap(void) {
    turbowasm_v128 slots[2] = {0};
    turbowasm_jit_invocation_context context = {0};

    context.simd_slots = slots;
    context.simd_slot_count = 2u;
    reset_status(&context);

    assert(turbowasm_jit_simd_splat_i64(
               &context, 0x11, 0, 3) == TURBOWASM_OK);
    assert(turbowasm_jit_simd_op(
               &context, 0xae, 1, 0, 0, -1, 0) ==
           TURBOWASM_OK);
    assert(context.call_status == TURBOWASM_OK);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_relaxed_helper_parity(void) {
    turbowasm_v128 slots[4] = {0};
    turbowasm_jit_invocation_context context = {0};
    uint8_t source[16];
    uint8_t indexes[16] = {0};
    uint8_t actual_bytes[16] = {0};
    float trunc_source[4] = {1.9f, -2.9f, 1.0e30f, -1.0e30f};
    int32_t trunc_actual[4] = {0};
    const int32_t trunc_expected[4] = {1, -2, INT32_MAX, INT32_MIN};
    uint32_t left[4] = {1u, 2u, 3u, 4u};
    uint32_t right[4] = {10u, 20u, 30u, 40u};
    uint32_t mask[4] = {UINT32_MAX, 0u, UINT32_MAX, 0u};
    uint32_t select_actual[4] = {0};
    const uint32_t select_expected[4] = {1u, 20u, 3u, 40u};
    uint32_t index;

    context.simd_slots = slots;
    context.simd_slot_count = 4u;
    reset_status(&context);

    for (index = 0u; index < 16u; ++index)
        source[index] = UINT8_C(0x80) + (uint8_t)index;
    cmeta_simd_v128_load(&slots[0].bits, source);
    cmeta_simd_v128_load(&slots[1].bits, indexes);

    assert(turbowasm_jit_simd_op(
               &context,
               0x100, /* i8x16.relaxed_swizzle */
               2, 0, 1, -1, 0) == TURBOWASM_OK);
    cmeta_simd_v128_store(actual_bytes, &slots[2].bits);
    for (index = 0u; index < 16u; ++index)
        assert(actual_bytes[index] == source[0]);

    cmeta_simd_v128_load(&slots[0].bits, trunc_source);
    assert(turbowasm_jit_simd_op(
               &context,
               0x101, /* i32x4.relaxed_trunc_f32x4_s */
               2, 0, -1, -1, 0) == TURBOWASM_OK);
    cmeta_simd_v128_store(trunc_actual, &slots[2].bits);
    assert(memcmp(
        trunc_actual, trunc_expected, sizeof(trunc_actual)) == 0);

    cmeta_simd_v128_load(&slots[0].bits, left);
    cmeta_simd_v128_load(&slots[1].bits, right);
    cmeta_simd_v128_load(&slots[2].bits, mask);
    assert(turbowasm_jit_simd_op(
               &context,
               0x10b, /* i32x4.relaxed_laneselect */
               3, 0, 1, 2, 0) == TURBOWASM_OK);
    cmeta_simd_v128_store(select_actual, &slots[3].bits);
    assert(memcmp(
        select_actual, select_expected,
        sizeof(select_actual)) == 0);

    assert(context.call_status == TURBOWASM_OK);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_unsupported_kind_records_status(void) {
    turbowasm_v128 slots[3] = {0};
    turbowasm_jit_invocation_context context = {0};

    context.simd_slots = slots;
    context.simd_slot_count = 3u;
    reset_status(&context);

    assert(turbowasm_jit_simd_op(
               &context,
               0x0d, /* shuffle needs its immediate-specific helper */
               2, 0, 1, -1, 0) ==
           TURBOWASM_UNSUPPORTED);
    assert(context.call_status == TURBOWASM_UNSUPPORTED);
    assert(context.call_trap == TURBOWASM_TRAP_NONE);
}

static void test_value_cells_preserve_shape(void) {
    turbowasm_v128 slots[2] = {0};
    turbowasm_jit_invocation_context context = {0};
    turbowasm_value input = {0}, output = {0};
    const uint64_t bits[2] = {UINT64_C(0x7ff8000000001234), UINT64_C(0x8000000000000000)};
    context.simd_slots = slots;
    context.simd_slot_count = 2u;
    input.kind = TURBOWASM_VALUE_V128;
    input.as.v128.shape = TURBOWASM_V128_F64X2;
    cmeta_simd_v128_load(&input.as.v128.bits, bits);
    assert(turbowasm_jit_simd_value_load(&context, 0, &input) == TURBOWASM_OK);
    assert(turbowasm_jit_simd_copy(&context, 1, 0) == TURBOWASM_OK);
    assert(turbowasm_jit_simd_value_store(&context, 1, &output) == TURBOWASM_OK);
    assert(output.kind == input.kind);
    assert(output.as.v128.shape == input.as.v128.shape);
    assert(memcmp(&output.as.v128.bits, &input.as.v128.bits, sizeof(input.as.v128.bits)) == 0);
    assert(turbowasm_jit_simd_const(&context, 1, 0, 0) == TURBOWASM_OK);
    assert(slots[1].shape == TURBOWASM_V128_RAW);
    assert(turbowasm_jit_simd_splat_i64(&context, 0x11, 1, 7) == TURBOWASM_OK);
    assert(slots[1].shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_jit_simd_value_load(&context, -1, &input) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_jit_simd_value_store(&context, 2, &output) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_jit_simd_value_store(&context, 0, NULL) == TURBOWASM_INVALID_ARGUMENT);
    input.kind = TURBOWASM_VALUE_I32;
    assert(turbowasm_jit_simd_value_load(&context, 0, &input) == TURBOWASM_TYPE_MISMATCH);
    assert(context.call_status == TURBOWASM_TYPE_MISMATCH);
}

int main(void) {
    test_value_cells_preserve_shape();
    test_slot_ops_and_reduce();
    test_slot_copy();
    test_memory_helper_and_trap();
    test_invalid_slot_is_invalid_argument();
    test_success_does_not_set_trap();
    test_relaxed_helper_parity();
    test_unsupported_kind_records_status();
    return 0;
}
