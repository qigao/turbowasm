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

static turbowasm_value i8x16_value(const int8_t lanes[16]) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_I8X16,
               lanes) == TURBOWASM_OK);
    return value;
}



static turbowasm_value raw_v128_value(const void *bytes) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_RAW,
               bytes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value f32x4_value(const float lanes[4]) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_F32X4,
               lanes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value f64x2_value(const double lanes[2]) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_F64X2,
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

static void test_advanced_numeric_core(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* types:
         * 0: (v128) -> v128
         * 1: (v128, v128) -> v128
         * 2: (v128) -> i32
         */
        0x01, 0x11,
        0x03,
        0x60, 0x01, 0x7b, 0x01, 0x7b,
        0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x60, 0x01, 0x7b, 0x01, 0x7f,

        0x03, 0x07,
        0x06, 0x00, 0x01, 0x01, 0x02, 0x02, 0x02,

        0x0a, 0x2f,
        0x06,

        /* func 0: i8x16.abs */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x60,
              0x0b,

        /* func 1: i8x16.add_sat_s */
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x6f,
              0x0b,

        /* func 2: i8x16.min_s */
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x76,
              0x0b,

        /* func 3: v128.any_true */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x53,
              0x0b,

        /* func 4: i8x16.all_true */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x63,
              0x0b,

        /* func 5: i8x16.bitmask */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x64,
              0x0b
    };
    const int8_t unary_input[16] = {
        -120, -100, -50, -1, 0, 1, 2, 3,
        -4, 5, -6, 7, -8, 9, -10, 11
    };
    const int8_t abs_expected[16] = {
        120, 100, 50, 1, 0, 1, 2, 3,
        4, 5, 6, 7, 8, 9, 10, 11
    };
    const int8_t sat_left[16] = {
        120, 100, -120, -100, 1, -1, 127, -128,
        10, 20, 30, 40, -10, -20, -30, -40
    };
    const int8_t sat_right[16] = {
        20, 50, -20, -50, 2, -2, 1, -1,
        -100, 100, 100, 100, -100, -100, -100, -100
    };
    const int8_t sat_expected[16] = {
        127, 127, -128, -128, 3, -3, 127, -128,
        -90, 120, 127, 127, -110, -120, -128, -128
    };
    const int8_t min_left[16] = {
        -5, 4, -3, 2, -1, 0, 1, 2,
        3, 4, 5, 6, 7, 8, 9, 10
    };
    const int8_t min_right[16] = {
        -4, 3, -2, 1, 0, -1, 2, 1,
        4, 3, 6, 5, 8, 7, 10, 9
    };
    const int8_t min_expected[16] = {
        -5, 3, -3, 1, -1, -1, 1, 1,
        3, 3, 5, 5, 7, 7, 9, 9
    };
    const int8_t all_nonzero[16] = {
        1, -1, 2, -2, 3, -3, 4, -4,
        5, -5, 6, -6, 7, -7, 8, -8
    };
    const int8_t with_zero[16] = {
        1, -1, 2, -2, 3, 0, 4, -4,
        5, -5, 6, -6, 7, -7, 8, -8
    };
    const int8_t bitmask_lanes[16] = {
        -1, 1, -2, 2, -3, 3, -4, 4,
        -5, 5, -6, 6, -7, 7, -8, 8
    };
    const int8_t all_zero[16] = {0};
    int8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    args[0] = i8x16_value(unary_input);
    result = invoke_v128(&instance, 0u, args, 1u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, abs_expected, sizeof(actual)) == 0);

    args[0] = i8x16_value(sat_left);
    args[1] = i8x16_value(sat_right);
    result = invoke_v128(&instance, 1u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, sat_expected, sizeof(actual)) == 0);

    args[0] = i8x16_value(min_left);
    args[1] = i8x16_value(min_right);
    result = invoke_v128(&instance, 2u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I8X16);
    assert(turbowasm_v128_store(actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, min_expected, sizeof(actual)) == 0);

    args[0] = i8x16_value(all_zero);
    assert(invoke_i32(&instance, 3u, args, 1u) == 0);
    args[0] = i8x16_value(with_zero);
    assert(invoke_i32(&instance, 3u, args, 1u) == 1);

    assert(invoke_i32(&instance, 4u, args, 1u) == 0);
    args[0] = i8x16_value(all_nonzero);
    assert(invoke_i32(&instance, 4u, args, 1u) == 1);

    args[0] = i8x16_value(bitmask_lanes);
    assert(invoke_i32(&instance, 5u, args, 1u) == 0x5555);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_pseudo_minmax(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x15,
        0x02,

        /* f32x4.pmin */
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0xea, 0x01,
              0x0b,

        /* f64x2.pmax */
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0xf7, 0x01,
              0x0b
    };
    const float f32_left[4] = {1.0f, 8.0f, -3.0f, 4.0f};
    const float f32_right[4] = {2.0f, 7.0f, -4.0f, 5.0f};
    const float f32_expected[4] = {1.0f, 7.0f, -4.0f, 4.0f};
    const double f64_left[2] = {9.0, -2.0};
    const double f64_right[2] = {8.0, -3.0};
    const double f64_expected[2] = {9.0, -2.0};
    float f32_actual[4] = {0};
    double f64_actual[2] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    args[0] = f32x4_value(f32_left);
    args[1] = f32x4_value(f32_right);
    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_F32X4);
    assert(turbowasm_v128_store(
               f32_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(f32_actual, f32_expected, sizeof(f32_actual)) == 0);

    args[0] = f64x2_value(f64_left);
    args[1] = f64x2_value(f64_right);
    result = invoke_v128(&instance, 1u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_F64X2);
    assert(turbowasm_v128_store(
               f64_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(f64_actual, f64_expected, sizeof(f64_actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_rounding_and_residual_ops(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x0c,
        0x02,
        0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x60, 0x01, 0x7b, 0x01, 0x7b,

        0x03, 0x04,
        0x03, 0x00, 0x01, 0x00,

        0x0a, 0x19,
        0x03,

        /* v128.andnot */
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x4f,
              0x0b,

        /* f32x4.ceil */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x67,
              0x0b,

        /* i8x16.avgr_u */
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x7b,
              0x0b
    };
    const uint32_t andnot_left[4] = {
        UINT32_C(0xffffffff), UINT32_C(0x0f0f0f0f),
        UINT32_C(0xaaaaaaaa), UINT32_C(0x12345678)
    };
    const uint32_t andnot_right[4] = {
        UINT32_C(0x00ff00ff), UINT32_C(0xf0f0f0f0),
        UINT32_C(0x55555555), UINT32_C(0xffff0000)
    };
    const uint32_t andnot_expected[4] = {
        UINT32_C(0xff00ff00), UINT32_C(0x0f0f0f0f),
        UINT32_C(0xaaaaaaaa), UINT32_C(0x00005678)
    };
    const float round_input[4] = {1.2f, -1.2f, 2.5f, -2.5f};
    const float round_expected[4] = {2.0f, -1.0f, 3.0f, -2.0f};
    const uint8_t avg_left[16] = {
        0u, 1u, 2u, 3u, 10u, 11u, 100u, 101u,
        200u, 201u, 250u, 251u, 254u, 255u, 7u, 8u
    };
    const uint8_t avg_right[16] = {
        1u, 2u, 3u, 4u, 11u, 12u, 101u, 102u,
        201u, 202u, 251u, 252u, 255u, 255u, 8u, 9u
    };
    const uint8_t avg_expected[16] = {
        1u, 2u, 3u, 4u, 11u, 12u, 101u, 102u,
        201u, 202u, 251u, 252u, 255u, 255u, 8u, 9u
    };

    uint32_t andnot_actual[4] = {0};
    float round_actual[4] = {0};
    uint8_t avg_actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    args[0] = raw_v128_value(andnot_left);
    args[1] = raw_v128_value(andnot_right);
    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_RAW);
    assert(turbowasm_v128_store(
               andnot_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        andnot_actual, andnot_expected, sizeof(andnot_actual)) == 0);

    args[0] = f32x4_value(round_input);
    result = invoke_v128(&instance, 1u, args, 1u);
    assert(result.as.v128.shape == TURBOWASM_V128_F32X4);
    assert(turbowasm_v128_store(
               round_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(
        round_actual, round_expected, sizeof(round_actual)) == 0);

    args[0] = raw_v128_value(avg_left);
    args[1] = raw_v128_value(avg_right);
    result = invoke_v128(&instance, 2u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_U8X16);
    assert(turbowasm_v128_store(
               avg_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(avg_actual, avg_expected, sizeof(avg_actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_advanced_numeric_core();
    test_pseudo_minmax();
    test_rounding_and_residual_ops();
    return 0;
}
