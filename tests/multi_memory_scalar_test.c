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

static void test_scalar_memidx_execution(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        /* five functions, all type0 */
        0x03, 0x06,
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00,

        /* memory0 min=1 page, memory1 min=2 pages */
        0x05, 0x05,
        0x02,
        0x00, 0x01,
        0x00, 0x02,

        0x0a, 0x39,
        0x05,

        /*
         * func0:
         *   memory0[0] = 11
         *   memory1[0] = 22
         *   return memory0[0] + memory1[0]
         *
         * For load/store memargs, bit 6 in the alignment field announces
         * the following explicit memory index.
         */
        0x1d, 0x00,
        0x41, 0x00,
        0x41, 0x0b,
        0x36, 0x02, 0x00,
        0x41, 0x00,
        0x41, 0x16,
        0x36, 0x42, 0x01, 0x00,
        0x41, 0x00,
        0x28, 0x02, 0x00,
        0x41, 0x00,
        0x28, 0x42, 0x01, 0x00,
        0x6a,
        0x0b,

        /* func1: memory.size 0 */
        0x04, 0x00, 0x3f, 0x00, 0x0b,

        /* func2: memory.size 1 */
        0x04, 0x00, 0x3f, 0x01, 0x0b,

        /* func3: memory.grow 1 by one page */
        0x06, 0x00, 0x41, 0x01, 0x40, 0x01, 0x0b,

        /* func4: read memory1[0] again */
        0x08, 0x00,
        0x41, 0x00,
        0x28, 0x42, 0x01, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i32(&instance, 0u) == 33);
    assert(invoke_i32(&instance, 1u) == 1);
    assert(invoke_i32(&instance, 2u) == 2);
    assert(invoke_i32(&instance, 4u) == 22);

    assert(invoke_i32(&instance, 3u) == 2);
    assert(invoke_i32(&instance, 1u) == 1);
    assert(invoke_i32(&instance, 2u) == 3);
    assert(invoke_i32(&instance, 4u) == 22);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_out_of_range_memidx_rejected(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        /* only memory0 exists */
        0x05, 0x03,
        0x01, 0x00, 0x01,
        0x0a, 0x06,
        0x01,
        /* memory.size 1 */
        0x04, 0x00, 0x3f, 0x01, 0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_scalar_memidx_execution();
    test_out_of_range_memidx_rejected();
    return 0;
}
