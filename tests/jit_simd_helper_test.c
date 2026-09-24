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
    salts_v128 slots[4] = {{{0}}};
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
    salts_simd_v128_store(raw, &slots[0]);
    for (uint32_t index = 0u; index < 16u; ++index)
        assert(raw[index] == (uint8_t)index);

    assert(turbowasm_jit_simd_splat_i64(
               &context, 0x11, 1, 7) == TURBOWASM_OK);
    salts_simd_v128_store(lanes, &slots[1]);
    assert(lanes[0] == 7 && lanes[1] == 7 &&
           lanes[2] == 7 && lanes[3] == 7);

    assert(turbowasm_jit_simd_op(
               &context,
               0xae, /* i32x4.add */
               2, 1, 1, -1, 0) == TURBOWASM_OK);
    salts_simd_v128_store(lanes, &slots[2]);
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

static void test_memory_helper_and_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* memory0 min=1 */
        0x05, 0x03, 0x01, 0x00, 0x01
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    salts_v128 slots[2] = {{{0}}};
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

    salts_simd_v128_load(&slots[0], input);

    assert(turbowasm_jit_simd_memory(
               &context,
               0x0b, /* v128.store */
               0, 0, 0) == TURBOWASM_OK);
    assert(turbowasm_jit_simd_memory(
               &context,
               0x00, /* v128.load */
               1, 0, 0) == TURBOWASM_OK);

    salts_simd_v128_store(output, &slots[1]);
    assert(memcmp(input, output, sizeof(input)) == 0);

    reset_status(&context);
    assert(turbowasm_jit_simd_memory(
               &context,
               0x00,
               1, 65530, 0) == TURBOWASM_TRAPPED);
    assert(context.call_status == TURBOWASM_TRAPPED);
    assert(context.call_trap ==
           TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_invalid_slot_is_invalid_argument(void) {
    salts_v128 slots[1] = {{{0}}};
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
    salts_v128 slots[2] = {{{0}}};
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

static void test_unsupported_kind_records_status(void) {
    salts_v128 slots[3] = {{{0}}};
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

int main(void) {
    test_slot_ops_and_reduce();
    test_memory_helper_and_trap();
    test_invalid_slot_is_invalid_argument();
    test_success_does_not_set_trap();
    test_unsupported_kind_records_status();
    return 0;
}
