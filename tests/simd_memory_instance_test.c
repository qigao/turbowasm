#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static turbowasm_value invoke_v128(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_V128);
    return result;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static turbowasm_value raw_v128(const uint8_t bytes[16]) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_RAW,
               bytes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value u16x8_lane3(uint16_t lane3) {
    const uint16_t lanes[8] = {
        UINT16_C(0x0102), UINT16_C(0x0304),
        UINT16_C(0x0506), lane3,
        UINT16_C(0x090a), UINT16_C(0x0b0c),
        UINT16_C(0x0d0e), UINT16_C(0x0f10)
    };
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_U16X8,
               lanes) == TURBOWASM_OK);
    return value;
}

static void test_widening_loads(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7b,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x13,
        0x02,
        0x08, 0x00,
              0x41, 0x00,
              0xfd, 0x01, 0x03, 0x00,
              0x0b,
        0x08, 0x00,
              0x41, 0x00,
              0xfd, 0x02, 0x03, 0x00,
              0x0b,

        0x0b, 0x0e,
        0x01, 0x00, 0x41, 0x00, 0x0b,
        0x08,
        0xff, 0x02, 0xfd, 0x04,
        0xfb, 0x06, 0xf9, 0x08
    };
    const int16_t expected_signed[8] = {
        -1, 2, -3, 4, -5, 6, -7, 8
    };
    const uint16_t expected_unsigned[8] = {
        255u, 2u, 253u, 4u, 251u, 6u, 249u, 8u
    };
    int16_t signed_lanes[8] = {0};
    uint16_t unsigned_lanes[8] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, NULL, 0u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               signed_lanes, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
               signed_lanes,
               expected_signed,
               sizeof(signed_lanes)) == 0);

    result = invoke_v128(&instance, 1u, NULL, 0u);
    assert(result.as.v128.shape == TURBOWASM_V128_U16X8);
    assert(turbowasm_v128_store(
               unsigned_lanes, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
               unsigned_lanes,
               expected_unsigned,
               sizeof(unsigned_lanes)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_splat_and_zero_loads(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7b,

        0x03, 0x04,
        0x03, 0x00, 0x00, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x1c,
        0x03,
        0x08, 0x00,
              0x41, 0x00,
              0xfd, 0x07, 0x00, 0x00,
              0x0b,
        0x08, 0x00,
              0x41, 0x00,
              0xfd, 0x5c, 0x02, 0x00,
              0x0b,
        0x08, 0x00,
              0x41, 0x00,
              0xfd, 0x5d, 0x03, 0x00,
              0x0b,

        0x0b, 0x0e,
        0x01, 0x00, 0x41, 0x00, 0x0b,
        0x08,
        0xa5, 0xef, 0xcd, 0xab,
        0x89, 0x67, 0x45, 0x23
    };
    static const uint8_t source[8] = {
        0xa5, 0xef, 0xcd, 0xab,
        0x89, 0x67, 0x45, 0x23
    };
    uint8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result;
    size_t index;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, NULL, 0u);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    for (index = 0u; index < sizeof(actual); ++index)
        assert(actual[index] == UINT8_C(0xa5));

    memset(actual, 0xff, sizeof(actual));
    result = invoke_v128(&instance, 1u, NULL, 0u);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, source, 4u) == 0);
    for (index = 4u; index < sizeof(actual); ++index)
        assert(actual[index] == 0u);

    memset(actual, 0xff, sizeof(actual));
    result = invoke_v128(&instance, 2u, NULL, 0u);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, source, 8u) == 0);
    for (index = 8u; index < sizeof(actual); ++index)
        assert(actual[index] == 0u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_lane_load_preserves_other_lanes(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x0d,
        0x01,
        0x0b, 0x00,
              0x41, 0x00,
              0x20, 0x00,
              0xfd, 0x54, 0x00, 0x00, 0x05,
              0x0b,

        0x0b, 0x07,
        0x01, 0x00, 0x41, 0x00, 0x0b,
        0x01, 0xaa
    };
    const uint8_t original[16] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    uint8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = raw_v128(original);
    turbowasm_value result;
    size_t index;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, &argument, 1u);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    for (index = 0u; index < sizeof(actual); ++index) {
        if (index == 5u)
            assert(actual[index] == UINT8_C(0xaa));
        else
            assert(actual[index] == original[index]);
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_lane_store_writes_only_selected_width(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x12,
        0x01,
        0x10, 0x00,
              0x41, 0x01,
              0x20, 0x00,
              0xfd, 0x59, 0x01, 0x00, 0x03,
              0x41, 0x00,
              0x28, 0x02, 0x00,
              0x0b,

        0x0b, 0x0a,
        0x01, 0x00, 0x41, 0x00, 0x0b,
        0x04, 0xaa, 0xbb, 0xcc, 0xdd
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument =
        u16x8_lane3(UINT16_C(0x1234));

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert((uint32_t)invoke_i32(
               &instance, 0u, &argument, 1u) ==
           UINT32_C(0xdd1234aa));

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_simd_memory_oob_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x0a,
        0x01,
        0x08, 0x00,
              0x41, 0x7c,
              0xfd, 0x0a, 0x03, 0x00,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_widening_loads();
    test_splat_and_zero_loads();
    test_lane_load_preserves_other_lanes();
    test_lane_store_writes_only_selected_width();
    test_simd_memory_oob_traps();
    return 0;
}
