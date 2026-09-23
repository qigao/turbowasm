#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>


static void test_raw_v128_roundtrip(void) {
    static const uint8_t expected[16] = {
        0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb,
        0xcc, 0xdd, 0xee, 0xff
    };
    uint8_t actual[16] = {0};
    turbowasm_v128 value = {0};

    assert(turbowasm_v128_load(
               &value, TURBOWASM_V128_RAW,
               expected) == TURBOWASM_OK);
    assert(value.shape == TURBOWASM_V128_RAW);
    assert(turbowasm_v128_descriptor(value.shape) == NULL);
    assert(turbowasm_v128_store(
               actual, &value) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_i32x4_add(void) {
    const int32_t left_lanes[4] = {1, 2, 3, 4};
    const int32_t right_lanes[4] = {4, 3, 2, 1};
    const int32_t expected[4] = {5, 5, 5, 5};
    int32_t actual[4] = {0};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 result = {0};

    assert(turbowasm_v128_load(
               &left, TURBOWASM_V128_I32X4, left_lanes) == TURBOWASM_OK);
    assert(turbowasm_v128_load(
               &right, TURBOWASM_V128_I32X4, right_lanes) == TURBOWASM_OK);

    assert(turbowasm_simd_i32x4_add(
               &result, &left, &right) == TURBOWASM_OK);
    assert(result.shape == TURBOWASM_V128_I32X4);
    assert(turbowasm_v128_descriptor(result.shape) == &cmeta_vector_i32x4);

    assert(turbowasm_v128_store(actual, &result) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_i32x4_eq_mask(void) {
    const int32_t left_lanes[4] = {1, 2, 3, 4};
    const int32_t right_lanes[4] = {1, 0, 3, 9};
    const uint32_t expected[4] = {UINT32_MAX, 0u, UINT32_MAX, 0u};
    uint32_t actual[4] = {0};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 mask = {0};

    assert(turbowasm_v128_load(
               &left, TURBOWASM_V128_I32X4, left_lanes) == TURBOWASM_OK);
    assert(turbowasm_v128_load(
               &right, TURBOWASM_V128_I32X4, right_lanes) == TURBOWASM_OK);
    assert(turbowasm_simd_i32x4_eq(
               &mask, &left, &right) == TURBOWASM_OK);
    assert(mask.shape == TURBOWASM_V128_B32X4);
    assert(turbowasm_v128_descriptor(mask.shape) == &cmeta_vector_b32x4);

    assert(turbowasm_v128_store(actual, &mask) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_f32x4_mul(void) {
    const float left_lanes[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float right_lanes[4] = {0.5f, 2.0f, 1.5f, 2.0f};
    const float expected[4] = {0.5f, 4.0f, 4.5f, 8.0f};
    float actual[4] = {0};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 result = {0};

    assert(turbowasm_v128_load(
               &left, TURBOWASM_V128_F32X4, left_lanes) == TURBOWASM_OK);
    assert(turbowasm_v128_load(
               &right, TURBOWASM_V128_F32X4, right_lanes) == TURBOWASM_OK);
    assert(turbowasm_simd_f32x4_mul(
               &result, &left, &right) == TURBOWASM_OK);
    assert(result.shape == TURBOWASM_V128_F32X4);

    assert(turbowasm_v128_store(actual, &result) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_shape_mismatch_rejected(void) {
    const int32_t ints[4] = {1, 2, 3, 4};
    const float floats[4] = {1, 2, 3, 4};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 result = {0};

    assert(turbowasm_v128_load(
               &left, TURBOWASM_V128_I32X4, ints) == TURBOWASM_OK);
    assert(turbowasm_v128_load(
               &right, TURBOWASM_V128_F32X4, floats) == TURBOWASM_OK);
    assert(turbowasm_simd_i32x4_add(
               &result, &left, &right) == TURBOWASM_TYPE_MISMATCH);
}

int main(void) {
    test_raw_v128_roundtrip();
    test_i32x4_add();
    test_i32x4_eq_mask();
    test_f32x4_mul();
    test_shape_mismatch_rejected();
    return 0;
}
