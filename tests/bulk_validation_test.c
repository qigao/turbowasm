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

#define FUNCTION_TYPE0 \
    0x03, 0x02, 0x01, 0x00

#define MEMORY_ONE \
    0x05, 0x03, 0x01, 0x00, 0x01

#define DATA_COUNT_ONE \
    0x0c, 0x01, 0x01

#define PASSIVE_EMPTY_DATA \
    0x0b, 0x03, 0x01, 0x01, 0x00

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_i32_extend8_s(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x07, 0x01, 0x05,
        0x00,
        0x41, 0x7f,
        0xc0,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_i32_trunc_sat_f32_s(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x43, 0x00, 0x00, 0x00, 0x00,
        0xfc, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_init_with_data_count(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        DATA_COUNT_ONE,
        0x0a, 0x0e, 0x01, 0x0c,
        0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfc, 0x08, 0x00, 0x00,
        0x0b,
        PASSIVE_EMPTY_DATA
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_init_requires_data_count(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x0e, 0x01, 0x0c,
        0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfc, 0x08, 0x00, 0x00,
        0x0b,
        PASSIVE_EMPTY_DATA
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_memory_init_rejects_bad_data_index(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        DATA_COUNT_ONE,
        0x0a, 0x0e, 0x01, 0x0c,
        0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfc, 0x08, 0x01, 0x00,
        0x0b,
        PASSIVE_EMPTY_DATA
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_data_drop(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        DATA_COUNT_ONE,
        0x0a, 0x07, 0x01, 0x05,
        0x00,
        0xfc, 0x09, 0x00,
        0x0b,
        PASSIVE_EMPTY_DATA
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_copy(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x0e, 0x01, 0x0c,
        0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfc, 0x0a, 0x00, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_memory_fill(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        MEMORY_ONE,
        0x0a, 0x0d, 0x01, 0x0b,
        0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfc, 0x0b, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_unknown_fc_subopcode_fails_closed(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        0x0a, 0x06, 0x01, 0x04,
        0x00,
        0xfc, 0x12,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

int main(void) {
    test_i32_extend8_s();
    test_i32_trunc_sat_f32_s();
    test_memory_init_with_data_count();
    test_memory_init_requires_data_count();
    test_memory_init_rejects_bad_data_index();
    test_data_drop();
    test_memory_copy();
    test_memory_fill();
    test_unknown_fc_subopcode_fails_closed();
    return 0;
}
