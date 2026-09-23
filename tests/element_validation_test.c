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

#define IMPORT_SECTION_FUNCTION \
    0x02, 0x07, \
    0x01, \
    0x01, 0x6d, \
    0x01, 0x66, \
    0x00, 0x00

#define TABLE_FUNCREF_ONE \
    0x04, 0x04, \
    0x01, 0x70, 0x00, 0x01

#define TABLE_EXTERNREF_ONE \
    0x04, 0x04, \
    0x01, 0x6f, 0x00, 0x01

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_active_legacy_funcref(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        TABLE_FUNCREF_ONE,
        0x09, 0x07,
        0x01,
        0x00,
        0x41, 0x00, 0x0b,
        0x01, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.element_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_passive_legacy_funcref(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x05,
        0x01,
        0x01, 0x00,
        0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_declarative_legacy_funcref(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x05,
        0x01,
        0x03, 0x00,
        0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_passive_ref_func_expression(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x07,
        0x01,
        0x05, 0x70,
        0x01, 0xd2, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_passive_ref_null_externref(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x09, 0x07,
        0x01,
        0x05, 0x6f,
        0x01, 0xd0, 0x6f, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_active_table_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        TABLE_EXTERNREF_ONE,
        0x09, 0x07,
        0x01,
        0x00,
        0x41, 0x00, 0x0b,
        0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_element_function_index_out_of_range(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x05,
        0x01,
        0x03, 0x00,
        0x01, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_element_expression_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x07,
        0x01,
        0x05, 0x6f,
        0x01, 0xd2, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_active_requires_table(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x09, 0x07,
        0x01,
        0x00,
        0x41, 0x00, 0x0b,
        0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_active_legacy_funcref();
    test_passive_legacy_funcref();
    test_declarative_legacy_funcref();
    test_passive_ref_func_expression();
    test_passive_ref_null_externref();
    test_active_table_type_mismatch();
    test_element_function_index_out_of_range();
    test_element_expression_type_mismatch();
    test_active_requires_table();
    return 0;
}
