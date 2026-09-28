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

static void invoke_memory_oob(
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
               &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
}

static void test_one_byte_pages_drive_bounds_and_grow(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        /* seven functions, all type0 */
        0x03, 0x08,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

        /*
         * memory0: custom page size, min=4, max=5, page exponent=0.
         * Flag 0x09 = maximum present + custom page size.
         */
        0x05, 0x05,
        0x01, 0x09, 0x04, 0x05, 0x00,

        0x0a, 0x3b,
        0x07,

        /* func0: memory.size -> 4 initially, then 5 */
        0x04, 0x00, 0x3f, 0x00, 0x0b,

        /* func1: load byte 3 */
        0x07, 0x00,
              0x41, 0x03,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func2: load byte 4 (OOB before grow) */
        0x07, 0x00,
              0x41, 0x04,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func3: grow by one one-byte page */
        0x06, 0x00,
              0x41, 0x01,
              0x40, 0x00,
              0x0b,

        /* func4: newly grown byte 4 is zero */
        0x07, 0x00,
              0x41, 0x04,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func5: store8 51 at byte4, then read it back */
        0x0e, 0x00,
              0x41, 0x04,
              0x41, 0x33,
              0x3a, 0x00, 0x00,
              0x41, 0x04,
              0x2d, 0x00, 0x00,
              0x0b,

        /* func6: max=5 rejects one more page */
        0x06, 0x00,
              0x41, 0x01,
              0x40, 0x00,
              0x0b,

        /* active data0 writes 42 to byte3 */
        0x0b, 0x07,
        0x01,
        0x00, 0x41, 0x03, 0x0b,
        0x01, 0x2a
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i32(&instance, 0u) == 4);
    assert(invoke_i32(&instance, 1u) == 42);
    invoke_memory_oob(&instance, 2u);

    assert(invoke_i32(&instance, 3u) == 4);
    assert(invoke_i32(&instance, 0u) == 5);
    assert(invoke_i32(&instance, 4u) == 0);
    assert(invoke_i32(&instance, 5u) == 51);

    assert(invoke_i32(&instance, 6u) == -1);
    assert(invoke_i32(&instance, 0u) == 5);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_page_size_validation_and_limit_range(void) {
    static const uint8_t invalid_exponent[] = {
        WASM_HEADER,
        /* custom min=1, page exponent=1 is not currently valid */
        0x05, 0x04,
        0x01, 0x08, 0x01, 0x01
    };
    static const uint8_t explicit_64k[] = {
        WASM_HEADER,
        /* custom min=1, explicit page exponent=16 */
        0x05, 0x04,
        0x01, 0x08, 0x01, 0x10
    };
    static const uint8_t one_byte_large_page_count[] = {
        WASM_HEADER,
        /* min=65537 pages is valid when each page is one byte */
        0x05, 0x06,
        0x01, 0x08, 0x81, 0x80, 0x04, 0x00
    };
    static const uint8_t default_large_page_count[] = {
        WASM_HEADER,
        /* min=65537 remains invalid for default 64-KiB pages */
        0x05, 0x05,
        0x01, 0x00, 0x81, 0x80, 0x04
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               invalid_exponent,
               sizeof(invalid_exponent)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);

    assert(turbowasm_module_load_borrowed(
               &module,
               explicit_64k,
               sizeof(explicit_64k)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    assert(turbowasm_module_load_borrowed(
               &module,
               one_byte_large_page_count,
               sizeof(one_byte_large_page_count)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    assert(turbowasm_module_load_borrowed(
               &module,
               default_large_page_count,
               sizeof(default_large_page_count)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static turbowasm_name name_span(
    const uint8_t *bytes,
    uint32_t size) {
    turbowasm_name name = {bytes, size};
    return name;
}

static void test_import_requires_exact_page_size(void) {
    static const uint8_t provider_bytes[] = {
        WASM_HEADER,
        /* custom memory min=1, one-byte pages */
        0x05, 0x04,
        0x01, 0x08, 0x01, 0x00,
        /* export memory as "mem" */
        0x07, 0x07,
        0x01, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
    };
    static const uint8_t default_consumer_bytes[] = {
        WASM_HEADER,
        /* import p.mem with default 64-KiB pages */
        0x02, 0x0a,
        0x01,
        0x01, 0x70,
        0x03, 0x6d, 0x65, 0x6d,
        0x02, 0x00, 0x01
    };
    static const uint8_t custom_consumer_bytes[] = {
        WASM_HEADER,
        /* import p.mem with matching one-byte pages */
        0x02, 0x0b,
        0x01,
        0x01, 0x70,
        0x03, 0x6d, 0x65, 0x6d,
        0x02, 0x08, 0x01, 0x00
    };
    static const uint8_t provider_name[] = {(uint8_t)'p'};
    turbowasm_module provider_module = {0};
    turbowasm_module default_consumer_module = {0};
    turbowasm_module custom_consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &default_consumer_module,
               default_consumer_bytes,
               sizeof(default_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &custom_consumer_module,
               custom_consumer_bytes,
               sizeof(custom_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(provider_name, 1u),
               &provider) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &consumer,
               &default_consumer_module,
               &linker) == TURBOWASM_TYPE_MISMATCH);
    assert(consumer.impl == NULL);

    assert(turbowasm_instance_create_linked(
               &consumer,
               &custom_consumer_module,
               &linker) == TURBOWASM_OK);

    turbowasm_instance_destroy(&consumer);
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&custom_consumer_module);
    turbowasm_module_destroy(&default_consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_one_byte_pages_drive_bounds_and_grow();
    test_page_size_validation_and_limit_range();
    test_import_requires_exact_page_size();
    return 0;
}
