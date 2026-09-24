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

static turbowasm_value v128_value(
    turbowasm_v128_shape shape,
    const void *lanes) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128, shape, lanes) == TURBOWASM_OK);
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

static void test_widen_narrow_runtime(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x0c,
        0x02,
        0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x60, 0x01, 0x7b, 0x01, 0x7b,

        0x03, 0x05,
        0x04, 0x00, 0x01, 0x00, 0x01,

        0x0a, 0x23,
        0x04,

        /* i8x16.narrow_i16x8_s */
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x65,
              0x0b,

        /* i16x8.extend_high_i8x16_s */
        0x07, 0x00,
              0x20, 0x00,
              0xfd, 0x88, 0x01,
              0x0b,

        /* i16x8.extmul_high_i8x16_s */
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x9d, 0x01,
              0x0b,

        /* i16x8.extadd_pairwise_i8x16_s */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x7c,
              0x0b
    };

    const int16_t narrow_low[8] = {
        -200, -128, -1, 0, 1, 127, 128, 300
    };
    const int16_t narrow_high[8] = {
        -300, -129, -2, 2, 126, 200, 1000, -1000
    };
    const int8_t narrow_expected[16] = {
        -128, -128, -1, 0, 1, 127, 127, 127,
        -128, -128, -2, 2, 126, 127, 127, -128
    };

    const int8_t extend_source[16] = {
        -8, -7, -6, -5, -4, -3, -2, -1,
        1, 2, 3, 4, 5, 6, 7, 8
    };
    const int16_t extend_expected[8] = {
        1, 2, 3, 4, 5, 6, 7, 8
    };

    const int8_t mul_left[16] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16
    };
    const int8_t mul_right[16] = {
        2, 3, 4, 5, 6, 7, 8, 9,
        10, 11, 12, 13, 14, 15, 16, 17
    };
    const int16_t mul_expected[8] = {
        90, 110, 132, 156, 182, 210, 240, 272
    };

    const int8_t pairwise_source[16] = {
        1, 2, 3, 4, -1, -2, -3, -4,
        5, 6, 7, 8, -5, -6, -7, -8
    };
    const int16_t pairwise_expected[8] = {
        3, 7, -3, -7, 11, 15, -11, -15
    };

    int8_t narrow_actual[16] = {0};
    int16_t wide_actual[8] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    args[0] = v128_value(TURBOWASM_V128_I16X8, narrow_low);
    args[1] = v128_value(TURBOWASM_V128_I16X8, narrow_high);
    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(
               narrow_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        narrow_actual, narrow_expected, sizeof(narrow_actual)) == 0);

    args[0] = v128_value(TURBOWASM_V128_I8X16, extend_source);
    result = invoke_v128(&instance, 1u, args, 1u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               wide_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        wide_actual, extend_expected, sizeof(wide_actual)) == 0);

    args[0] = v128_value(TURBOWASM_V128_I8X16, mul_left);
    args[1] = v128_value(TURBOWASM_V128_I8X16, mul_right);
    result = invoke_v128(&instance, 2u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               wide_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        wide_actual, mul_expected, sizeof(wide_actual)) == 0);

    args[0] = v128_value(TURBOWASM_V128_I8X16, pairwise_source);
    result = invoke_v128(&instance, 3u, args, 1u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               wide_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        wide_actual, pairwise_expected, sizeof(wide_actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_q15_and_dot_runtime(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x15,
        0x02,

        /* i16x8.q15mulr_sat_s */
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x82, 0x01,
              0x0b,

        /* i32x4.dot_i16x8_s */
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0xba, 0x01,
              0x0b
    };
    const int16_t q15_left[8] = {
        16384, 32767, -32768, 8192,
        -16384, 1000, -1000, 0
    };
    const int16_t q15_right[8] = {
        16384, 32767, -32768, 16384,
        16384, 2000, 2000, 32767
    };
    const int16_t q15_expected[8] = {
        8192, 32766, 32767, 4096,
        -8192, 61, -61, 0
    };
    const int16_t dot_left[8] = {
        1, 2, 3, 4, -1, -2, 100, 200
    };
    const int16_t dot_right[8] = {
        10, 20, 30, 40, 5, 6, -2, 3
    };
    const int32_t dot_expected[4] = {
        50, 250, -17, 400
    };

    int16_t q15_actual[8] = {0};
    int32_t dot_actual[4] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    args[0] = v128_value(TURBOWASM_V128_I16X8, q15_left);
    args[1] = v128_value(TURBOWASM_V128_I16X8, q15_right);
    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               q15_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(q15_actual, q15_expected, sizeof(q15_actual)) == 0);

    args[0] = v128_value(TURBOWASM_V128_I16X8, dot_left);
    args[1] = v128_value(TURBOWASM_V128_I16X8, dot_right);
    result = invoke_v128(&instance, 1u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_v128_store(
               dot_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(dot_actual, dot_expected, sizeof(dot_actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_widen_narrow_runtime();
    test_q15_and_dot_runtime();
    return 0;
}
