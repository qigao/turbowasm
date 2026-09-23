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

#define TYPE_SECTION_EMPTY_FN \
    0x01, 0x04, \
    0x01, 0x60, 0x00, 0x00

#define FUNCTION_SECTION_TYPE0 \
    0x03, 0x02, \
    0x01, 0x00

#define CODE_SECTION_EMPTY_FN \
    0x0a, 0x04, \
    0x01, 0x02, 0x00, 0x0b

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_valid_single_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        FUNCTION_SECTION_TYPE0,
        CODE_SECTION_EMPTY_FN
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.type_count == 1u);
    assert(summary.function_count == 1u);
    assert(summary.code_count == 1u);
    assert(summary.custom_section_count == 0u);
    assert((summary.standard_section_mask & (UINT32_C(1) << 1u)) != 0u);
    assert((summary.standard_section_mask & (UINT32_C(1) << 3u)) != 0u);
    assert((summary.standard_section_mask & (UINT32_C(1) << 10u)) != 0u);
    turbowasm_module_destroy(&module);
}

static void test_repeated_custom_sections(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x00, 0x01, 0xaa,
        0x00, 0x02, 0xbb, 0xcc
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.custom_section_count == 2u);
    assert(summary.standard_section_mask == 0u);
    turbowasm_module_destroy(&module);
}

static void test_duplicate_standard_section(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        TYPE_SECTION_EMPTY_FN
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_out_of_order_section(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x03, 0x01, 0x00,
        TYPE_SECTION_EMPTY_FN
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_bad_function_type_index(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        0x03, 0x02, 0x01, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_function_without_code(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        FUNCTION_SECTION_TYPE0
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_bad_code_count(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        FUNCTION_SECTION_TYPE0,
        0x0a, 0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_code_body_requires_end(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        FUNCTION_SECTION_TYPE0,
        0x0a, 0x04, 0x01, 0x02, 0x00, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_truncated_section_payload(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}


static void test_unvalidated_standard_section_is_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

static void test_unknown_section_is_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x0d, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

static void test_unknown_value_type_is_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x01, 0x7a, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

int main(void) {
    test_valid_single_function();
    test_repeated_custom_sections();
    test_duplicate_standard_section();
    test_out_of_order_section();
    test_bad_function_type_index();
    test_function_without_code();
    test_bad_code_count();
    test_code_body_requires_end();
    test_truncated_section_payload();
    test_unvalidated_standard_section_is_unsupported();
    test_unknown_section_is_unsupported();
    test_unknown_value_type_is_unsupported();
    return 0;
}
