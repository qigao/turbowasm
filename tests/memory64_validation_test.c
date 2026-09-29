#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void test_memory64_size_and_grow_typing(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x0a, 0x02,
        0x60, 0x00, 0x01, 0x7e,
        0x60, 0x01, 0x7e, 0x01, 0x7e,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x05, 0x04, 0x01, 0x05, 0x01, 0x02,
        0x0a, 0x0d, 0x02,
        0x04, 0x00, 0x3f, 0x00, 0x0b,
        0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_memory_desc desc = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_memory_at(&module, 0u, &desc));
    assert(desc.memory64);
    assert(desc.minimum64 == 1u);
    assert(desc.maximum64 == 2u);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_memory64_load_requires_i64_address(void) {
    static const uint8_t valid[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x04, 0x01,
        0x0a, 0x09, 0x01,
        0x07, 0x00, 0x42, 0x00, 0x28, 0x02, 0x00, 0x0b
    };
    static const uint8_t invalid[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x04, 0x01,
        0x0a, 0x09, 0x01,
        0x07, 0x00, 0x41, 0x00, 0x28, 0x02, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, valid, sizeof(valid)) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
    assert(turbowasm_module_load_borrowed(
               &module, invalid, sizeof(invalid)) ==
           TURBOWASM_MALFORMED_MODULE);
}

static void test_shared_memory64_is_explicitly_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x04, 0x01, 0x07, 0x01, 0x02
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_UNSUPPORTED);
}

int main(void) {
    test_memory64_size_and_grow_typing();
    test_memory64_load_requires_i64_address();
    test_shared_memory64_is_explicitly_unsupported();
    return 0;
}
