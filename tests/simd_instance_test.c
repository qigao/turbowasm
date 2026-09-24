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

static turbowasm_value v128_i32x4(
    int32_t a, int32_t b, int32_t c, int32_t d) {
    const int32_t lanes[4] = {a, b, c, d};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_I32X4,
               lanes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value v128_f32x4(
    float a, float b, float c, float d) {
    const float lanes[4] = {a, b, c, d};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_F32X4,
               lanes) == TURBOWASM_OK);
    return value;
}


static turbowasm_value v128_u32x4(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t lanes[4] = {a, b, c, d};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_U32X4,
               lanes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value v128_i16x8(
    int16_t a, int16_t b, int16_t c, int16_t d,
    int16_t e, int16_t f, int16_t g, int16_t h) {
    const int16_t lanes[8] = {a, b, c, d, e, f, g, h};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    assert(turbowasm_v128_load(
               &value.as.v128,
               TURBOWASM_V128_I16X8,
               lanes) == TURBOWASM_OK);
    return value;
}

static turbowasm_value v128_f64x2(double a, double b) {
    const double lanes[2] = {a, b};
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

static void test_v128_const(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x16,
        0x01, 0x14,
        0x00,
        0xfd, 0x0c,
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f,
        0x0b
    };
    static const uint8_t expected[16] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f
    };
    uint8_t actual[16] = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, NULL, 0u);
    assert(result.as.v128.shape == TURBOWASM_V128_RAW);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_i32x4_splat(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00, 0x20, 0x00, 0xfd, 0x11, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value result;
    int32_t lanes[4] = {0};

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = 9;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, &argument, 1u);
    assert(result.as.v128.shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 9 && lanes[1] == 9 &&
           lanes[2] == 9 && lanes[3] == 9);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_i32x4_add_and_eq(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x03, 0x02, 0x00, 0x00,
        0x0a, 0x14,
        0x02,
        0x09, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0xae, 0x01,
              0x0b,
        0x08, 0x00,
              0x20, 0x00,
              0x20, 0x01,
              0xfd, 0x37,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    int32_t lanes[4] = {0};
    uint32_t mask[4] = {0};

    args[0] = v128_i32x4(1, 2, 3, 4);
    args[1] = v128_i32x4(10, 20, 30, 40);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 11 && lanes[1] == 22 &&
           lanes[2] == 33 && lanes[3] == 44);

    args[1] = v128_i32x4(1, 99, 3, 88);
    result = invoke_v128(&instance, 1u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_B32X4);
    assert(turbowasm_v128_store(
               mask, &result.as.v128) == TURBOWASM_OK);
    assert(mask[0] == UINT32_MAX);
    assert(mask[1] == 0u);
    assert(mask[2] == UINT32_MAX);
    assert(mask[3] == 0u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_f32x4_mul(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0xfd, 0xe6, 0x01,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    float lanes[4] = {0};

    args[0] = v128_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
    args[1] = v128_f32x4(2.0f, 3.0f, 4.0f, 5.0f);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_F32X4);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 2.0f && lanes[1] == 6.0f &&
           lanes[2] == 12.0f && lanes[3] == 20.0f);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_bitselect(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x01, 0x60, 0x03, 0x7b, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0c, 0x01, 0x0a,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0x20, 0x02,
        0xfd, 0x52,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[3];
    turbowasm_value result;
    uint32_t lanes[4] = {0};

    args[0] = v128_i32x4(1, 2, 3, 4);
    args[1] = v128_i32x4(10, 20, 30, 40);
    args[2] = v128_i32x4(
        -1, 0, -1, 0);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 3u);
    assert(result.as.v128.shape == TURBOWASM_V128_RAW);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 1u && lanes[1] == 20u &&
           lanes[2] == 3u && lanes[3] == 40u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_v128_memory_roundtrip(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x12, 0x01, 0x10,
        0x00,
        0x41, 0x00,
        0x20, 0x00,
        0xfd, 0x0b, 0x04, 0x00,
        0x41, 0x00,
        0xfd, 0x00, 0x04, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = v128_i32x4(5, 6, 7, 8);
    turbowasm_value result;
    int32_t lanes[4] = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, &argument, 1u);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 5 && lanes[1] == 6 &&
           lanes[2] == 7 && lanes[3] == 8);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_generic_unsigned_compare(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0a, 0x01, 0x08,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0xfd, 0x3a,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    uint32_t mask[4] = {0};

    args[0] = v128_u32x4(
        UINT32_MAX, 1u, UINT32_C(0x80000000), 3u);
    args[1] = v128_u32x4(
        1u, 2u, UINT32_MAX, 3u);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_B32X4);
    assert(turbowasm_v128_store(
               mask, &result.as.v128) == TURBOWASM_OK);
    assert(mask[0] == 0u);
    assert(mask[1] == UINT32_MAX);
    assert(mask[2] == UINT32_MAX);
    assert(mask[3] == 0u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_generic_i16x8_mul(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0xfd, 0x95, 0x01,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    int16_t lanes[8] = {0};

    args[0] = v128_i16x8(1, 2, 3, 4, 5, 6, 7, 8);
    args[1] = v128_i16x8(8, 7, 6, 5, 4, 3, 2, 1);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_I16X8);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 8 && lanes[1] == 14 &&
           lanes[2] == 18 && lanes[3] == 20 &&
           lanes[4] == 20 && lanes[5] == 18 &&
           lanes[6] == 14 && lanes[7] == 8);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_generic_unsigned_shift(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7f, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0xfd, 0xad, 0x01,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    uint32_t lanes[4] = {0};

    args[0] = v128_u32x4(
        UINT32_C(0xfffffff8), UINT32_C(0x80000000), 8u, 4u);
    args[1].kind = TURBOWASM_VALUE_I32;
    args[1].as.i32 = 1;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_U32X4);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == UINT32_C(0x7ffffffc));
    assert(lanes[1] == UINT32_C(0x40000000));
    assert(lanes[2] == 4u);
    assert(lanes[3] == 2u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_generic_f64x2_div(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7b, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0xfd, 0xf3, 0x01,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2];
    turbowasm_value result;
    double lanes[2] = {0.0, 0.0};

    args[0] = v128_f64x2(12.0, 9.0);
    args[1] = v128_f64x2(3.0, 1.5);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    result = invoke_v128(&instance, 0u, args, 2u);
    assert(result.as.v128.shape == TURBOWASM_V128_F64X2);
    assert(turbowasm_v128_store(
               lanes, &result.as.v128) == TURBOWASM_OK);
    assert(lanes[0] == 4.0);
    assert(lanes[1] == 6.0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_validated_but_unimplemented_simd_fails_closed(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00,
        0x20, 0x00,
        /* i16x8.extend_low_i8x16_s remains deferred under #48. */
        0xfd, 0x67,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = v128_i32x4(1, 2, 3, 4);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_UNSUPPORTED);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_v128_const();
    test_i32x4_splat();
    test_i32x4_add_and_eq();
    test_f32x4_mul();
    test_bitselect();
    test_v128_memory_roundtrip();
    test_generic_unsigned_compare();
    test_generic_i16x8_mul();
    test_generic_unsigned_shift();
    test_generic_f64x2_div();
    test_validated_but_unimplemented_simd_fails_closed();
    return 0;
}
