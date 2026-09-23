#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void invoke_void(
    turbowasm_instance *instance,
    uint32_t function_index) {
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void invoke_trap(
    turbowasm_instance *instance,
    uint32_t function_index,
    turbowasm_trap expected) {
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(trap == expected);
}

static void test_passive_data_bulk_runtime(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0 [] -> i32, type1 [] -> [] */
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x00,

        /* six functions */
        0x03, 0x07,
        0x06, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00,

        /* memory0 min=1 */
        0x05, 0x03,
        0x01, 0x00, 0x01,

        /* DataCount=1 */
        0x0c, 0x01, 0x01,

        0x0a, 0x56,
        0x06,

        /* func0: memory.init data0, return byte 0 */
        0x11, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x03,
              0xfc, 0x08, 0x00, 0x00,
              0x41, 0x00,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func1: data.drop 0 */
        0x05, 0x00, 0xfc, 0x09, 0x00, 0x0b,

        /* func2: zero-length memory.init after drop succeeds */
        0x0c, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0xfc, 0x08, 0x00, 0x00,
              0x0b,

        /* func3: length-one memory.init after drop traps */
        0x0c, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x01,
              0xfc, 0x08, 0x00, 0x00,
              0x0b,

        /* func4: memory.fill offset10 with 42, return it */
        0x10, 0x00,
              0x41, 0x0a,
              0x41, 0x2a,
              0x41, 0x01,
              0xfc, 0x0b, 0x00,
              0x41, 0x0a,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func5: memory.copy 10 -> 20, return byte20 */
        0x11, 0x00,
              0x41, 0x14,
              0x41, 0x0a,
              0x41, 0x01,
              0xfc, 0x0a, 0x00, 0x00,
              0x41, 0x14,
              0x2d, 0x00, 0x00,
              0x0b,

        /* passive data0 = "abc" */
        0x0b, 0x06,
        0x01, 0x01, 0x03, 0x61, 0x62, 0x63
    };
    turbowasm_module module = {0};
    turbowasm_instance a = {0};
    turbowasm_instance b = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&a, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&b, &module) == TURBOWASM_OK);

    assert(invoke_i32(&a, 0u) == 0x61);
    assert(invoke_i32(&a, 4u) == 42);
    assert(invoke_i32(&a, 5u) == 42);

    invoke_void(&a, 1u);
    invoke_void(&a, 2u);
    invoke_trap(&a, 3u, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    /* Drop lifecycle is per-instance. */
    assert(invoke_i32(&b, 0u) == 0x61);

    turbowasm_instance_destroy(&a);
    turbowasm_instance_destroy(&b);
    turbowasm_module_destroy(&module);
}

static void test_passive_element_bulk_runtime(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0 [] -> i32, type1 [] -> [] */
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x00,

        /* nine functions */
        0x03, 0x0a,
        0x09,
        0x00, 0x00, 0x01, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x00,

        /* table0 funcref min=1 max=2 */
        0x04, 0x05,
        0x01, 0x70, 0x01, 0x01, 0x02,

        /* passive elem0 = [func0] */
        0x09, 0x05,
        0x01, 0x01, 0x00, 0x01, 0x00,

        0x0a, 0x6b,
        0x09,

        /* func0 -> 7 */
        0x04, 0x00, 0x41, 0x07, 0x0b,

        /* func1: table.init then indirect call -> 7 */
        0x11, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x01,
              0xfc, 0x0c, 0x00, 0x00,
              0x41, 0x00,
              0x11, 0x00, 0x00,
              0x0b,

        /* func2: elem.drop 0 */
        0x05, 0x00, 0xfc, 0x0d, 0x00, 0x0b,

        /* func3: zero-length table.init after drop succeeds */
        0x0c, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0xfc, 0x0c, 0x00, 0x00,
              0x0b,

        /* func4: nonzero table.init after drop traps */
        0x0c, 0x00,
              0x41, 0x00,
              0x41, 0x00,
              0x41, 0x01,
              0xfc, 0x0c, 0x00, 0x00,
              0x0b,

        /* func5: table.size */
        0x05, 0x00, 0xfc, 0x10, 0x00, 0x0b,

        /* func6: table.grow null by 1 */
        0x09, 0x00,
              0xd0, 0x70,
              0x41, 0x01,
              0xfc, 0x0f, 0x00,
              0x0b,

        /* func7: table.fill table[0] with null, return is_null */
        0x10, 0x00,
              0x41, 0x00,
              0xd0, 0x70,
              0x41, 0x01,
              0xfc, 0x11, 0x00,
              0x41, 0x00,
              0x25, 0x00,
              0xd1,
              0x0b,

        /* func8: table.copy 0 -> 1, indirect call table[1] */
        0x11, 0x00,
              0x41, 0x01,
              0x41, 0x00,
              0x41, 0x01,
              0xfc, 0x0e, 0x00, 0x00,
              0x41, 0x01,
              0x11, 0x00, 0x00,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i32(&instance, 5u) == 1);
    assert(invoke_i32(&instance, 1u) == 7);

    assert(invoke_i32(&instance, 6u) == 1);
    assert(invoke_i32(&instance, 5u) == 2);
    assert(invoke_i32(&instance, 8u) == 7);

    assert(invoke_i32(&instance, 7u) == 1);

    invoke_void(&instance, 2u);
    invoke_void(&instance, 3u);
    invoke_trap(&instance, 4u, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);

    assert(invoke_i32(&instance, 6u) == -1);
    assert(invoke_i32(&instance, 5u) == 2);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_passive_data_bulk_runtime();
    test_passive_element_bulk_runtime();
    return 0;
}
