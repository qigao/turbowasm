#include <turbowasm/turbowasm.h>

#include "instance_internal.h"

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

static void synthetic_shared_instance_init(
    turbowasm_instance_impl *instance,
    turbowasm_instance_memory *memory,
    const turbowasm_module *module) {
    memset(instance, 0, sizeof(*instance));
    memset(memory, 0, sizeof(*memory));

    instance->module = module;
    instance->memories = memory;
    instance->memory_count = 1u;

    memory->pages = 1u;
    memory->maximum_pages = 1u;
    memory->page_size = TURBOWASM_WASM_PAGE_SIZE;
    memory->has_maximum = true;

    assert(turbowasm_instance_memory_storage_init(
               memory, true,
               TURBOWASM_WASM_PAGE_SIZE) == TURBOWASM_OK);
}

static turbowasm_value invoke_synthetic(
    turbowasm_instance_impl *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke_interpreter_internal(
               instance,
               function_index,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap,
               NULL) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    return result;
}

static void test_shared_scalar_load_store(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        /* shared memory32 min=1 max=1 */
        0x05, 0x04,
        0x01, 0x03, 0x01, 0x01,

        0x0a, 0x10,
        0x01,
        0x0e,
        0x00,
        0x41, 0x08,
        0x41, 0x2a,
        0x3a, 0x00, 0x00,
        0x41, 0x08,
        0x2d, 0x00, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance public_instance = {0};
    turbowasm_instance_impl instance = {0};
    turbowasm_instance_memory memory = {0};
    turbowasm_value result;
    uint8_t *raw = NULL;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);

    /* T2b still does not remove the public T1 gate. */
    assert(turbowasm_instance_create(
               &public_instance, &module) == TURBOWASM_UNSUPPORTED);
    assert(public_instance.impl == NULL);

    synthetic_shared_instance_init(
        &instance, &memory, &module);

    assert(turbowasm_instance_memory_bounds(
               &instance, 0u, 0u, 0u, 1u,
               &raw) == TURBOWASM_UNSUPPORTED);

    result = invoke_synthetic(&instance, 0u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    turbowasm_instance_memory_storage_destroy(&memory);
    turbowasm_module_destroy(&module);
}

static void test_shared_bulk_fill(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        /* shared memory32 min=1 max=1 */
        0x05, 0x04,
        0x01, 0x03, 0x01, 0x01,

        0x0a, 0x12,
        0x01,
        0x10,
        0x00,
        0x41, 0x10,
        0x41, 0x2a,
        0x41, 0x04,
        0xfc, 0x0b, 0x00,
        0x41, 0x10,
        0x2d, 0x00, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance_impl instance = {0};
    turbowasm_instance_memory memory = {0};
    turbowasm_value result;
    uint8_t payload[4] = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    synthetic_shared_instance_init(
        &instance, &memory, &module);

    result = invoke_synthetic(&instance, 0u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 0x2a);

    assert(turbowasm_instance_memory_read_bytes(
               &instance, 0u, 16u, 0u,
               payload, sizeof(payload)) == TURBOWASM_OK);
    assert(payload[0] == 0x2au);
    assert(payload[1] == 0x2au);
    assert(payload[2] == 0x2au);
    assert(payload[3] == 0x2au);

    turbowasm_instance_memory_storage_destroy(&memory);
    turbowasm_module_destroy(&module);
}

static void test_shared_v128_store_load(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> v128 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7b,

        0x03, 0x02,
        0x01, 0x00,

        /* shared memory32 min=1 max=1 */
        0x05, 0x04,
        0x01, 0x03, 0x01, 0x01,

        0x0a, 0x22,
        0x01,
        0x20,
        0x00,
        0x41, 0x20,
        0xfd, 0x0c,
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f,
        0xfd, 0x0b, 0x04, 0x00,
        0x41, 0x20,
        0xfd, 0x00, 0x04, 0x00,
        0x0b
    };
    static const uint8_t expected[16] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f
    };
    turbowasm_module module = {0};
    turbowasm_instance_impl instance = {0};
    turbowasm_instance_memory memory = {0};
    turbowasm_value result;
    uint8_t actual[16] = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    synthetic_shared_instance_init(
        &instance, &memory, &module);

    result = invoke_synthetic(&instance, 0u);
    assert(result.kind == TURBOWASM_VALUE_V128);
    assert(turbowasm_v128_store(
               actual, &result.as.v128) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    memset(actual, 0, sizeof(actual));
    assert(turbowasm_instance_memory_read_bytes(
               &instance, 0u, 32u, 0u,
               actual, sizeof(actual)) == TURBOWASM_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    turbowasm_instance_memory_storage_destroy(&memory);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_shared_scalar_load_store();
    test_shared_bulk_fill();
    test_shared_v128_store_load();
    return 0;
}
