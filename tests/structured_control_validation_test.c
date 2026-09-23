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

#define TYPE_I32_RESULT \
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f

#define TYPE_EMPTY \
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00

#define TYPE_I32_I32 \
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f

#define FUNCTION_TYPE0 \
    0x03, 0x02, 0x01, 0x00

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_block_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_block_missing_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x07, 0x01, 0x05,
        0x00,
        0x02, 0x7f,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_if_else_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0e, 0x01, 0x0c,
        0x00,
        0x41, 0x01,
        0x04, 0x7f,
        0x41, 0x02,
        0x05,
        0x41, 0x03,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_if_without_else_cannot_produce_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x41, 0x01,
        0x04, 0x7f,
        0x41, 0x02,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_br_supplies_block_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x0c, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_br_if_preserves_fallthrough_value(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x0d, 0x01, 0x0b,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x41, 0x01,
        0x0d, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_loop_branch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x03, 0x40,
        0x0c, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_typeidx_block_params_and_results(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_I32,
        FUNCTION_TYPE0,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x20, 0x00,
        0x02, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_block_floor_prevents_underflow(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_I32,
        FUNCTION_TYPE0,
        0x0a, 0x0b, 0x01, 0x09,
        0x00,
        0x20, 0x00,
        0x02, 0x00,
        0x1a,
        0x1a,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_nested_unreachable_is_polymorphic(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_I32_RESULT,
        FUNCTION_TYPE0,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x02, 0x7f,
        0x00,
        0x6a,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}


static void test_block_typeidx_s33_overflow_rejected(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        0x0a, 0x0a, 0x01, 0x08,
        0x00,
        0x02, 0x80, 0x80, 0x80, 0x80, 0x10,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_invalid_branch_depth(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_EMPTY,
        FUNCTION_TYPE0,
        0x0a, 0x06, 0x01, 0x04,
        0x00,
        0x0c, 0x01,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_block_result();
    test_block_missing_result();
    test_if_else_result();
    test_if_without_else_cannot_produce_result();
    test_br_supplies_block_result();
    test_br_if_preserves_fallthrough_value();
    test_loop_branch();
    test_typeidx_block_params_and_results();
    test_block_floor_prevents_underflow();
    test_nested_unreachable_is_polymorphic();
    test_block_typeidx_s33_overflow_rejected();
    test_invalid_branch_depth();
    return 0;
}
