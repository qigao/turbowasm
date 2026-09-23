#include <turbowasm/turbowasm.h>

#include <stdint.h>
#include <string.h>

int main(void) {
    const int32_t left_data[4] = {1, 2, 3, 4};
    const int32_t right_data[4] = {4, 3, 2, 1};
    const int32_t expected[4] = {5, 5, 5, 5};
    int32_t actual[4] = {0};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 result = {0};

    if (turbowasm_v128_load(
            &left, TURBOWASM_V128_I32X4, left_data) != TURBOWASM_OK)
        return 1;
    if (turbowasm_v128_load(
            &right, TURBOWASM_V128_I32X4, right_data) != TURBOWASM_OK)
        return 2;
    if (turbowasm_simd_i32x4_add(
            &result, &left, &right) != TURBOWASM_OK)
        return 3;
    if (result.shape != TURBOWASM_V128_I32X4)
        return 4;
    if (turbowasm_v128_store(actual, &result) != TURBOWASM_OK)
        return 5;
    return memcmp(actual, expected, sizeof(actual)) == 0 ? 0 : 6;
}
