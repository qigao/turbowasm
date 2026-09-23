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

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_imported_function_and_export(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,
        0x07, 0x05,
        0x01,
        0x01, 0x66,
        0x00, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.type_count == 1u);
    assert(summary.imported_function_count == 1u);
    assert(summary.function_count == 0u);
    assert(summary.export_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_memory_export(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x03,
        0x01, 0x00, 0x01,
        0x07, 0x07,
        0x01,
        0x03, 0x6d, 0x65, 0x6d,
        0x02, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.memory_count == 1u);
    assert(summary.export_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_table_section(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x04, 0x04,
        0x01, 0x70, 0x00, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.table_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_duplicate_export_name(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x03,
        0x01, 0x00, 0x01,
        0x07, 0x09,
        0x02,
        0x01, 0x78, 0x02, 0x00,
        0x01, 0x78, 0x02, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_invalid_utf8_import_name(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x02, 0x09,
        0x01,
        0x02, 0xc0, 0x80,
        0x01, 0x6d,
        0x02, 0x00, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_memory_max_less_than_min(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x04,
        0x01, 0x01, 0x02, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_shared_memory_flags_are_explicitly_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x02,
        0x01, 0x03
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_UNSUPPORTED);
    assert(module.impl == NULL);
}

static void test_export_index_out_of_range(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x05, 0x03,
        0x01, 0x00, 0x01,
        0x07, 0x07,
        0x01,
        0x03, 0x6d, 0x65, 0x6d,
        0x02, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_data_count_zero_without_data(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x0c, 0x01, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.has_data_count);
    assert(summary.data_count == 0u);
    turbowasm_module_destroy(&module);
}

static void test_data_count_nonzero_requires_data(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x0c, 0x01, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_start_imported_empty_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,
        0x08, 0x01, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.has_start);
    assert(summary.start_function_index == 0u);
    turbowasm_module_destroy(&module);
}

static void test_start_rejects_parameter_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x01, 0x7f, 0x00,
        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,
        0x08, 0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_start_rejects_result_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,
        0x08, 0x01, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_start_local_empty_function(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        0x03, 0x02, 0x01, 0x00,
        0x08, 0x01, 0x00,
        0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_imported_function_and_export();
    test_memory_export();
    test_table_section();
    test_duplicate_export_name();
    test_invalid_utf8_import_name();
    test_memory_max_less_than_min();
    test_shared_memory_flags_are_explicitly_unsupported();
    test_export_index_out_of_range();
    test_data_count_zero_without_data();
    test_data_count_nonzero_requires_data();
    test_start_imported_empty_function();
    test_start_rejects_parameter_function();
    test_start_rejects_result_function();
    test_start_local_empty_function();
    return 0;
}
