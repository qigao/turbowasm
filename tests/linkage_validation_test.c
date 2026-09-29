#include <turbowasm/turbowasm.h>

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

#define TYPE_SECTION_EMPTY_FN \
    0x01, 0x04, \
    0x01, 0x60, 0x00, 0x00

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static bool name_is(
    turbowasm_name name,
    const char *expected) {
    size_t size = strlen(expected);
    return name.size == size &&
           memcmp(name.bytes, expected, size) == 0;
}

static bool name_borrows_module(
    turbowasm_name name,
    const uint8_t *bytes,
    size_t size) {
    return name.bytes >= bytes &&
           name.bytes <= bytes + size &&
           (size_t)(bytes + size - name.bytes) >= name.size;
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

static void test_retained_linkage_metadata(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,

        /* imports: env.f, env.t, env.m, env.g */
        0x02, 0x25,
        0x04,
        0x03, 0x65, 0x6e, 0x76,
        0x01, 0x66,
        0x00, 0x00,
        0x03, 0x65, 0x6e, 0x76,
        0x01, 0x74,
        0x01, 0x70, 0x00, 0x01,
        0x03, 0x65, 0x6e, 0x76,
        0x01, 0x6d,
        0x02, 0x00, 0x01,
        0x03, 0x65, 0x6e, 0x76,
        0x01, 0x67,
        0x03, 0x7f, 0x00,

        /* export the four imported index-space entries */
        0x07, 0x11,
        0x04,
        0x01, 0x66, 0x00, 0x00,
        0x01, 0x74, 0x01, 0x00,
        0x01, 0x6d, 0x02, 0x00,
        0x01, 0x67, 0x03, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};
    turbowasm_function_signature signature = {0};
    const turbowasm_import_desc *import_desc;
    const turbowasm_export_desc *export_desc;
    size_t index;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.imported_function_count == 1u);
    assert(summary.imported_table_count == 1u);
    assert(summary.imported_memory_count == 1u);
    assert(summary.imported_global_count == 1u);
    assert(summary.export_count == 4u);

    assert(turbowasm_module_import_count(&module) == 4u);
    assert(turbowasm_module_export_count(&module) == 4u);
    assert(turbowasm_module_import_at(&module, 4u) == NULL);
    assert(turbowasm_module_export_at(&module, 4u) == NULL);

    for (index = 0u; index < 4u; ++index) {
        import_desc = turbowasm_module_import_at(&module, index);
        export_desc = turbowasm_module_export_at(&module, index);
        assert(import_desc != NULL);
        assert(export_desc != NULL);
        assert(name_is(import_desc->module_name, "env"));
        assert(name_borrows_module(
            import_desc->module_name, bytes, sizeof(bytes)));
        assert(name_borrows_module(
            import_desc->name, bytes, sizeof(bytes)));
        assert(name_borrows_module(
            export_desc->name, bytes, sizeof(bytes)));
        assert(import_desc->item_index == 0u);
        assert(export_desc->item_index == 0u);
        assert(import_desc->kind == (turbowasm_external_kind)index);
        assert(export_desc->kind == (turbowasm_external_kind)index);
    }

    import_desc = turbowasm_module_import_at(&module, 0u);
    assert(import_desc != NULL);
    assert(name_is(import_desc->name, "f"));
    assert(import_desc->type_index == 0u);
    assert(turbowasm_module_function_signature_get(
        &module, import_desc->item_index, &signature));
    assert(signature.param_count == 0u);
    assert(signature.result_count == 0u);

    import_desc = turbowasm_module_import_at(&module, 1u);
    assert(import_desc != NULL);
    assert(name_is(import_desc->name, "t"));
    assert(import_desc->type_index == UINT32_MAX);

    import_desc = turbowasm_module_import_at(&module, 2u);
    assert(import_desc != NULL);
    assert(name_is(import_desc->name, "m"));
    assert(import_desc->type_index == UINT32_MAX);

    import_desc = turbowasm_module_import_at(&module, 3u);
    assert(import_desc != NULL);
    assert(name_is(import_desc->name, "g"));
    assert(import_desc->type_index == UINT32_MAX);

    assert(name_is(
        turbowasm_module_export_at(&module, 0u)->name, "f"));
    assert(name_is(
        turbowasm_module_export_at(&module, 1u)->name, "t"));
    assert(name_is(
        turbowasm_module_export_at(&module, 2u)->name, "m"));
    assert(name_is(
        turbowasm_module_export_at(&module, 3u)->name, "g"));

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

static void test_shared_memory_metadata_is_retained_and_executable(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* memory0: shared memory32, min=1, max=2 */
        0x05, 0x04,
        0x01, 0x03, 0x01, 0x02
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_memory_desc memory = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_memory_count(&module) == 1u);
    assert(turbowasm_module_memory_at(&module, 0u, &memory));
    assert(memory.minimum == 1u);
    assert(memory.maximum == 2u);
    assert(memory.page_size == 65536u);
    assert(memory.has_maximum);
    assert(memory.shared);
    assert(!memory.imported);
    assert(!turbowasm_module_memory_at(&module, 1u, &memory));

    /*
     * T2c enables ordinary shared memory through the guarded backing.
     */
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(instance.impl != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_shared_memory_requires_maximum(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* shared bit without maximum-present bit */
        0x05, 0x03,
        0x01, 0x02, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_memory64_metadata_is_retained(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* memory64 + maximum, min=1, max=2 */
        0x05, 0x04,
        0x01, 0x05, 0x01, 0x02
    };
    turbowasm_module module = {0};
    turbowasm_memory_desc desc = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_memory_at(&module, 0u, &desc));
    assert(desc.memory64);
    assert(desc.minimum64 == 1u);
    assert(desc.maximum64 == 2u);
    assert(desc.has_maximum);
    turbowasm_module_destroy(&module);
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
    test_retained_linkage_metadata();
    test_memory_export();
    test_table_section();
    test_duplicate_export_name();
    test_invalid_utf8_import_name();
    test_memory_max_less_than_min();
    test_shared_memory_metadata_is_retained_and_executable();
    test_shared_memory_requires_maximum();
    test_memory64_metadata_is_retained();
    test_export_index_out_of_range();
    test_data_count_zero_without_data();
    test_data_count_nonzero_requires_data();
    test_start_imported_empty_function();
    test_start_rejects_parameter_function();
    test_start_rejects_result_function();
    test_start_local_empty_function();
    return 0;
}
