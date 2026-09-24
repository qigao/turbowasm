#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
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
    const turbowasm_value *argument) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_V128);
    return result;
}

static void test_conversion_runtime(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (v128) -> v128 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,

        0x03, 0x05,
        0x04, 0x00, 0x00, 0x00, 0x00,

        0x0a, 0x1f,
        0x04,

        /* f32x4.demote_f64x2_zero */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x5e,
              0x0b,

        /* f64x2.promote_low_f32x4 */
        0x06, 0x00,
              0x20, 0x00,
              0xfd, 0x5f,
              0x0b,

        /* i32x4.trunc_sat_f32x4_s */
        0x07, 0x00,
              0x20, 0x00,
              0xfd, 0xf8, 0x01,
              0x0b,

        /* f64x2.convert_low_i32x4_u */
        0x07, 0x00,
              0x20, 0x00,
              0xfd, 0xff, 0x01,
              0x0b
    };

    const double demote_source[2] = {1.5, -2.25};
    const float demote_expected[4] = {1.5f, -2.25f, 0.0f, 0.0f};
    const float promote_source[4] = {1.5f, -2.25f, 99.0f, 100.0f};
    const double promote_expected[2] = {1.5, -2.25};
    const float trunc_source[4] = {
        1.9f, -2.9f, 1.0e30f, -1.0e30f
    };
    const int32_t trunc_expected[4] = {
        1, -2, INT32_MAX, INT32_MIN
    };
    const uint32_t convert_source[4] = {1u, 2u, 3u, 4u};
    const double convert_expected[2] = {1.0, 2.0};

    float f32_actual[4] = {0};
    double f64_actual[2] = {0};
    int32_t i32_actual[4] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument;
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    argument = v128_value(TURBOWASM_V128_F64X2, demote_source);
    result = invoke_v128(&instance, 0u, &argument);
    assert(result.as.v128.shape == TURBOWASM_V128_F32X4);
    assert(turbowasm_v128_store(
               f32_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(f32_actual, demote_expected, sizeof(f32_actual)) == 0);

    argument = v128_value(TURBOWASM_V128_F32X4, promote_source);
    result = invoke_v128(&instance, 1u, &argument);
    assert(result.as.v128.shape == TURBOWASM_V128_F64X2);
    assert(turbowasm_v128_store(
               f64_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(f64_actual, promote_expected, sizeof(f64_actual)) == 0);

    argument = v128_value(TURBOWASM_V128_F32X4, trunc_source);
    result = invoke_v128(&instance, 2u, &argument);
    assert(result.as.v128.shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_v128_store(
               i32_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(i32_actual, trunc_expected, sizeof(i32_actual)) == 0);

    argument = v128_value(TURBOWASM_V128_U32X4, convert_source);
    result = invoke_v128(&instance, 3u, &argument);
    assert(result.as.v128.shape == TURBOWASM_V128_F64X2);
    assert(turbowasm_v128_store(
               f64_actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(f64_actual, convert_expected, sizeof(f64_actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_conversion_runtime();
    return 0;
}
