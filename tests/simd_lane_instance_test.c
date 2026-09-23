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

static turbowasm_value raw_v128(const uint8_t bytes[16]) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_RAW,
               bytes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value i16x8_value(void) {
    const int16_t lanes[8] = {
        10, 20, 30, 40, 50, 60, 70, 80
    };
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_I16X8,
               lanes) == TURBOWASM_OK);
    return value;
}

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

static void test_signed_unsigned_extract_lane(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7f,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x11,
        0x02,
        0x07, 0x00,
              0x20, 0x00,
              0xfd, 0x15, 0x0f,
              0x0b,
        0x07, 0x00,
              0x20, 0x00,
              0xfd, 0x16, 0x0f,
              0x0b
    };
    uint8_t lanes[16] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 255u
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = raw_v128(lanes);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i32(
               &instance, 0u, &argument, 1u) == -1);
    assert(invoke_i32(
               &instance, 1u, &argument, 1u) == 255);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_replace_lane_truncates_and_preserves(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7f, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x0b,
        0x01,
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x1a, 0x03,
              0x0b
    };
    const int16_t expected[8] = {
        10, 20, 30, -1234, 50, 60, 70, 80
    };
    int16_t actual[8] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    args[0] = i16x8_value();
    args[1].kind = TURBOWASM_VALUE_I32;
    args[1].as.i32 = -1234;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_shuffle_crosses_both_inputs(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x1a,
        0x01,
        0x18, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x0d,
              0x1f, 0x00, 0x10, 0x0f,
              0x11, 0x01, 0x1e, 0x02,
              0x1d, 0x03, 0x1c, 0x04,
              0x1b, 0x05, 0x1a, 0x06,
              0x0b
    };
    const uint8_t left_bytes[16] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    const uint8_t right_bytes[16] = {
        16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u,
        24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u
    };
    const uint8_t expected[16] = {
        31u, 0u, 16u, 15u,
        17u, 1u, 30u, 2u,
        29u, 3u, 28u, 4u,
        27u, 5u, 26u, 6u
    };
    uint8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    args[0] = raw_v128(left_bytes);
    args[1] = raw_v128(right_bytes);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_swizzle_zeroes_out_of_range_indices(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x0a,
        0x01,
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x0e,
              0x0b
    };
    const uint8_t value_bytes[16] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    const uint8_t index_bytes[16] = {
        15u, 0u, 1u, 16u,
        2u, 17u, 3u, 255u,
        4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u
    };
    const uint8_t expected[16] = {
        15u, 0u, 1u, 0u,
        2u, 0u, 3u, 0u,
        4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u
    };
    uint8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    args[0] = raw_v128(value_bytes);
    args[1] = raw_v128(index_bytes);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_signed_unsigned_extract_lane();
    test_replace_lane_truncates_and_preserves();
    test_shuffle_crosses_both_inputs();
    test_swizzle_zeroes_out_of_range_indices();
    return 0;
}
