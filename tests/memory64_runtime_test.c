#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t memory64_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0e, 0x03,
    0x60, 0x00, 0x01, 0x7e,
    0x60, 0x01, 0x7e, 0x01, 0x7e,
    0x60, 0x00, 0x01, 0x7f,
    0x03, 0x07, 0x06, 0x00, 0x01, 0x02, 0x02, 0x02, 0x02,
    /* memory64 min=1 max=2 */
    0x05, 0x04, 0x01, 0x05, 0x01, 0x02,
    0x0a, 0x47, 0x06,

    /* f0: memory.size -> i64 */
    0x04, 0x00, 0x3f, 0x00, 0x0b,

    /* f1(delta:i64): memory.grow -> i64 */
    0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b,

    /* f2: read active-data byte at i64 address 5 */
    0x07, 0x00, 0x42, 0x05, 0x2d, 0x00, 0x00, 0x0b,

    /* f3: store8 0x33 at address 1 and read it */
    0x0e, 0x00,
    0x42, 0x01, 0x41, 0x33, 0x3a, 0x00, 0x00,
    0x42, 0x01, 0x2d, 0x00, 0x00, 0x0b,

    /* f4: memory.fill address 2 with 0x2a, len 1, then read */
    0x10, 0x00,
    0x42, 0x02, 0x41, 0x2a, 0x42, 0x01,
    0xfc, 0x0b, 0x00,
    0x42, 0x02, 0x2d, 0x00, 0x00, 0x0b,

    /* f5: memory.copy byte 2 -> 3, len 1, then read */
    0x11, 0x00,
    0x42, 0x03, 0x42, 0x02, 0x42, 0x01,
    0xfc, 0x0a, 0x00, 0x00,
    0x42, 0x03, 0x2d, 0x00, 0x00, 0x0b,

    /* active data0 at i64 offset 5 = 0x7a */
    0x0b, 0x07, 0x01,
    0x00, 0x42, 0x05, 0x0b,
    0x01, 0x7a
};

static int64_t invoke_i64(
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
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I64);
    return result.as.i64;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

int main(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value grow = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               memory64_module,
               sizeof(memory64_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i64(&instance, 0u, NULL, 0u) == 1);
    assert(invoke_i32(&instance, 2u) == 0x7a);
    assert(invoke_i32(&instance, 3u) == 0x33);
    assert(invoke_i32(&instance, 4u) == 0x2a);
    assert(invoke_i32(&instance, 5u) == 0x2a);

    grow.kind = TURBOWASM_VALUE_I64;
    grow.as.i64 = 1;
    assert(invoke_i64(&instance, 1u, &grow, 1u) == 1);
    assert(invoke_i64(&instance, 0u, NULL, 0u) == 2);
    assert(invoke_i64(&instance, 1u, &grow, 1u) == -1);
    assert(invoke_i64(&instance, 0u, NULL, 0u) == 2);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
