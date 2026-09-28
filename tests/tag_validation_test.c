#include "../src/module_internal.h"
#include "../src/validation_context.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static turbowasm_status load(
    const uint8_t *bytes,
    size_t size,
    turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_defined_tag_retains_type_identity(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32, i64) -> () */
        0x01, 0x06,
        0x01, 0x60, 0x02, 0x7f, 0x7e, 0x00,

        /* tag0: attribute=0, type0 */
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /* export tag0 as "e" */
        0x07, 0x05,
        0x01, 0x01, 0x65, 0x04, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_tag *tag;
    const turbowasm_validation_func_type *type;
    const turbowasm_export_desc *export_desc;

    {
        turbowasm_status status = load(bytes, sizeof(bytes), &module);
        if (status != TURBOWASM_OK) {
            fprintf(stderr,
                    "defined tag status=%d (%s)\n",
                    (int)status,
                    turbowasm_status_string(status));
        }
        assert(status == TURBOWASM_OK);
    }
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.type_count == 1u);
    assert(summary.imported_tag_count == 0u);
    assert(summary.tag_count == 1u);
    assert(summary.export_count == 1u);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.tag_count == 1u);

    tag = turbowasm_validation_context_tag(
        &impl->validation, 0u);
    assert(tag != NULL);
    assert(tag->type_index == 0u);
    assert(!tag->imported);

    type = turbowasm_validation_context_type(
        &impl->validation, tag->type_index);
    assert(type != NULL);
    assert(type->param_count == 2u);
    assert(type->params[0] == 0x7fu);
    assert(type->params[1] == 0x7eu);
    assert(type->result_count == 0u);

    export_desc = turbowasm_module_export_at(&module, 0u);
    assert(export_desc != NULL);
    assert(export_desc->kind == TURBOWASM_EXTERN_TAG);
    assert(export_desc->item_index == 0u);

    turbowasm_module_destroy(&module);
}

static void test_imported_tag_precedes_defined_tag_index(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32) -> () */
        0x01, 0x05,
        0x01, 0x60, 0x01, 0x7f, 0x00,

        /* import m.t as tag type0 */
        0x02, 0x08,
        0x01,
        0x01, 0x6d,
        0x01, 0x74,
        0x04, 0x00, 0x00,

        /* one defined tag of the same type => tag index 1 */
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /* export imported tag0 and defined tag1 */
        0x07, 0x09,
        0x02,
        0x01, 0x69, 0x04, 0x00,
        0x01, 0x64, 0x04, 0x01
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_import_desc *import_desc;
    const turbowasm_validation_tag *imported_tag;
    const turbowasm_validation_tag *defined_tag;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.imported_tag_count == 1u);
    assert(summary.tag_count == 1u);
    assert(summary.export_count == 2u);

    assert(turbowasm_module_import_count(&module) == 1u);
    import_desc = turbowasm_module_import_at(&module, 0u);
    assert(import_desc != NULL);
    assert(import_desc->kind == TURBOWASM_EXTERN_TAG);
    assert(import_desc->item_index == 0u);
    assert(import_desc->type_index == 0u);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.tag_count == 2u);

    imported_tag = turbowasm_validation_context_tag(
        &impl->validation, 0u);
    defined_tag = turbowasm_validation_context_tag(
        &impl->validation, 1u);
    assert(imported_tag != NULL && imported_tag->imported);
    assert(defined_tag != NULL && !defined_tag->imported);
    assert(imported_tag->type_index == 0u);
    assert(defined_tag->type_index == 0u);

    assert(turbowasm_module_export_at(&module, 0u)->kind ==
           TURBOWASM_EXTERN_TAG);
    assert(turbowasm_module_export_at(&module, 0u)->item_index == 0u);
    assert(turbowasm_module_export_at(&module, 1u)->kind ==
           TURBOWASM_EXTERN_TAG);
    assert(turbowasm_module_export_at(&module, 1u)->item_index == 1u);

    turbowasm_module_destroy(&module);
}

static void test_tag_requires_zero_result_function_type(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32, invalid as a tag type */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x0d, 0x03,
        0x01, 0x00, 0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_tag_attribute_and_type_index_are_validated(void) {
    static const uint8_t invalid_attribute[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x0d, 0x03,
        0x01, 0x01, 0x00
    };
    static const uint8_t invalid_type_index[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x0d, 0x03,
        0x01, 0x00, 0x01
    };
    turbowasm_module module = {0};

    assert(load(
               invalid_attribute,
               sizeof(invalid_attribute),
               &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);

    assert(load(
               invalid_type_index,
               sizeof(invalid_type_index),
               &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_tag_section_order_is_between_memory_and_global(void) {
    static const uint8_t valid[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,

        /* memory section */
        0x05, 0x03,
        0x01, 0x00, 0x01,

        /* tag section */
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /* global section */
        0x06, 0x06,
        0x01, 0x7f, 0x00, 0x41, 0x00, 0x0b
    };
    static const uint8_t invalid[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,

        /* global before tag: tag is now out of semantic section order */
        0x06, 0x06,
        0x01, 0x7f, 0x00, 0x41, 0x00, 0x0b,

        0x0d, 0x03,
        0x01, 0x00, 0x00
    };
    turbowasm_module module = {0};

    assert(load(valid, sizeof(valid), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    assert(load(invalid, sizeof(invalid), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_tag_export_index_is_validated(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,

        /* one tag */
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /* export nonexistent tag1 */
        0x07, 0x05,
        0x01, 0x01, 0x78, 0x04, 0x01
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_defined_tag_retains_type_identity();
    test_imported_tag_precedes_defined_tag_index();
    test_tag_requires_zero_result_function_type();
    test_tag_attribute_and_type_index_are_validated();
    test_tag_section_order_is_between_memory_and_global();
    test_tag_export_index_is_validated();
    return 0;
}
