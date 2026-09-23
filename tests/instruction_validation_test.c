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

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_i32_arithmetic_result(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x09, 0x01, 0x07,
        0x00, 0x41, 0x02, 0x41, 0x03, 0x6a, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_result_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x09, 0x01, 0x07,
        0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_parameter_is_local_zero(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x20, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_local_set_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x01, 0x7f, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b, 0x01, 0x09,
        0x00, 0x43, 0x00, 0x00, 0x00, 0x00,
        0x21, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_direct_call_uses_retained_signature(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7f,
        0x02, 0x07,
        0x01, 0x01, 0x6d, 0x01, 0x66, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x01,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x10, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_ref_func_declared_by_export(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x70,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
        0x0a, 0x0a,
        0x02,
        0x02, 0x00, 0x0b,
        0x04, 0x00, 0xd2, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_ref_func_requires_declaration(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x70,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x0a, 0x0a,
        0x02,
        0x02, 0x00, 0x0b,
        0x04, 0x00, 0xd2, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_global_get_in_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x02, 0x08,
        0x01, 0x01, 0x6d, 0x01, 0x67, 0x03, 0x7f, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_simd_i32x4_add_typing(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x2b, 0x01, 0x29,
        0x00,
        0xfd, 0x0c,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0xfd, 0x0c,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0xfd, 0xae, 0x01,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_simd_binary_underflow(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x19, 0x01, 0x17,
        0x00,
        0xfd, 0x0c,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0xfd, 0xae, 0x01,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_nested_control_fails_closed_for_now(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00, 0x02, 0x40, 0x0b, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

int main(void) {
    test_i32_arithmetic_result();
    test_result_type_mismatch();
    test_parameter_is_local_zero();
    test_local_set_type_mismatch();
    test_direct_call_uses_retained_signature();
    test_ref_func_declared_by_export();
    test_ref_func_requires_declaration();
    test_global_get_in_function();
    test_simd_i32x4_add_typing();
    test_simd_binary_underflow();
    test_nested_control_fails_closed_for_now();
    return 0;
}
