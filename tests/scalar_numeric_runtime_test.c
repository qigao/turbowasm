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

static const uint8_t module_bytes[] = {
    WASM_HEADER,

    /* type0: (f32, f32) -> i32; type1: (f64, f64) -> i32 */
    0x01, 0x0d,
    0x02,
    0x60, 0x02, 0x7d, 0x7d, 0x01, 0x7f,
    0x60, 0x02, 0x7c, 0x7c, 0x01, 0x7f,

    /* function0 type0; function1 type1 */
    0x03, 0x03,
    0x02, 0x00, 0x01,

    0x0a, 0x11,
    0x02,

    /* f32.lt */
    0x07,
    0x00,
    0x20, 0x00,
    0x20, 0x01,
    0x5d,
    0x0b,

    /* f64.ne */
    0x07,
    0x00,
    0x20, 0x00,
    0x20, 0x01,
    0x62,
    0x0b
};

static int32_t invoke_compare(
    turbowasm_instance *instance,
    uint32_t function_index,
    turbowasm_value left,
    turbowasm_value right) {
    turbowasm_value arguments[2] = {left, right};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance,
               function_index,
               arguments,
               2u,
               &result,
               1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

int main(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value left = {0};
    turbowasm_value right = {0};
    uint64_t quiet_nan_bits = UINT64_C(0x7ff8000000000000);

    assert(turbowasm_module_load_borrowed(
               &module,
               module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance,
               &module) == TURBOWASM_OK);

    left.kind = TURBOWASM_VALUE_F32;
    left.as.f32 = 1.0f;
    right.kind = TURBOWASM_VALUE_F32;
    right.as.f32 = 2.0f;
    assert(invoke_compare(&instance, 0u, left, right) == 1);

    left.kind = TURBOWASM_VALUE_F64;
    memcpy(&left.as.f64, &quiet_nan_bits, sizeof(quiet_nan_bits));
    right.kind = TURBOWASM_VALUE_F64;
    right.as.f64 = 1.0;
    assert(invoke_compare(&instance, 1u, left, right) == 1);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
