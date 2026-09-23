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

#define TYPE_EMPTY \
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00

#define TYPE_I32_RESULT \
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f

#define TYPE_V128_RESULT \
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b

#define FUNCTION_TYPE0 \
    0x03, 0x02, 0x01, 0x00

#define MEMORY_ONE \
    0x05, 0x03, 0x01, 0x00, 0x01

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_i32_load(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x41, 0x00,
        0x28, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_load_requires_memory(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x41, 0x00,
        0x28, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_load_rejects_over_alignment(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x41, 0x00,
        0x28, 0x03, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_i32_store(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x41, 0x00,
        0x41, 0x01,
        0x36, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_size(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x3f, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_grow(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x08, 0x01, 0x06,
        0x00,
        0x41, 0x01,
        0x40, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_index_extension_fails_closed(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x3f, 0x01, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

static void test_i32_wrap_i64(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x07, 0x01, 0x05,
        0x00,
        0x42, 0x00,
        0xa7,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_conversion_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0a, 0x01, 0x08,
        0x00,
        0x43, 0x00, 0x00, 0x00, 0x00,
        0xa7,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_v128_load(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_V128_RESULT,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x0a, 0x01, 0x08,
        0x00,
        0x41, 0x00,
        0xfd, 0x00, 0x04, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_v128_store(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x1c, 0x01, 0x1a,
        0x00,
        0x41, 0x00,
        0xfd, 0x0c,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0xfd, 0x0b, 0x04, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_br_table_same_label_types(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0f, 0x01, 0x0d,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x41, 0x00,
        0x0e, 0x01, 0x00, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_br_table_rejects_mixed_label_types(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x12, 0x01, 0x10,
        0x00,
        0x02, 0x7f,
        0x02, 0x40,
        0x41, 0x07,
        0x41, 0x00,
        0x0e, 0x01, 0x00, 0x01,
        0x0b,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_i32_load();
    test_load_requires_memory();
    test_load_rejects_over_alignment();
    test_i32_store();
    test_memory_size();
    test_memory_grow();
    test_memory_index_extension_fails_closed();
    test_i32_wrap_i64();
    test_conversion_type_mismatch();
    test_v128_load();
    test_v128_store();
    test_br_table_same_label_types();
    test_br_table_rejects_mixed_label_types();
    return 0;
}
