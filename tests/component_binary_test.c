#include "../src/component_binary.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define COMPONENT_HEADER     0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00

static void test_empty_component(void) {
    static const uint8_t bytes[] = { COMPONENT_HEADER };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(component.bytes == bytes);
    assert(component.size == sizeof(bytes));
    assert(component.section_count == 0u);
    assert(component.core_module_count == 0u);
    turbowasm_component_binary_destroy(&component);
}

static void test_preamble_rejection(void) {
    static const uint8_t core_module[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00
    };
    static const uint8_t bad_version[] = {
        0x00,0x61,0x73,0x6d,0x0c,0x00,0x01,0x00
    };
    static const uint8_t bad_layer[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x00,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,
               core_module,
               sizeof(core_module)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_version,
               sizeof(bad_version)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_layer,
               sizeof(bad_layer)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_custom_section_framing(void) {
    static const uint8_t valid[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x02,'h','i'
    };
    static const uint8_t bad_name_length[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x05,'a','b'
    };
    static const uint8_t bad_utf8[] = {
        COMPONENT_HEADER,
        0x00,0x03,0x02,0xff,0xfe
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,valid,sizeof(valid)) == TURBOWASM_OK);
    assert(component.section_count == 1u);
    assert(turbowasm_component_binary_section_at(
               &component,0u)->id == 0u);
    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               bad_name_length,
               sizeof(bad_name_length)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               bad_utf8,
               sizeof(bad_utf8)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_section_bounds(void) {
    static const uint8_t invalid_id[] = {
        COMPONENT_HEADER,
        0x0d,0x00
    };
    static const uint8_t truncated_payload[] = {
        COMPONENT_HEADER,
        0x07,0x03,0x00
    };
    static const uint8_t missing_size[] = {
        COMPONENT_HEADER,
        0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,
               invalid_id,
               sizeof(invalid_id)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               truncated_payload,
               sizeof(truncated_payload)) == TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,
               missing_size,
               sizeof(missing_size)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_embedded_core_module_record(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        0x01,0x08,
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
        0x07,0x01,0x00
    };
    static const uint8_t wrong_layer[] = {
        COMPONENT_HEADER,
        0x01,0x08,
        COMPONENT_HEADER
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_core_module *module;

    assert(turbowasm_component_binary_load(
               &component,bytes,sizeof(bytes)) == TURBOWASM_OK);
    assert(component.section_count == 2u);
    assert(component.core_module_count == 1u);

    module = turbowasm_component_binary_core_module_at(
        &component,0u);
    assert(module != NULL);
    assert(module->size == 8u);
    assert(module->bytes[0] == 0u);
    assert(module->bytes[1] == (uint8_t)'a');
    assert(turbowasm_component_binary_core_module_at(
               &component,1u) == NULL);

    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               wrong_layer,
               sizeof(wrong_layer)) != TURBOWASM_OK);
}

static void test_nested_component_boundary(void) {
    static const uint8_t valid[] = {
        COMPONENT_HEADER,
        0x04,0x08,
        COMPONENT_HEADER
    };
    static const uint8_t bad_version[] = {
        COMPONENT_HEADER,
        0x04,0x08,
        0x00,0x61,0x73,0x6d,0x0c,0x00,0x01,0x00
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,valid,sizeof(valid)) == TURBOWASM_OK);
    assert(component.section_count == 1u);
    turbowasm_component_binary_destroy(&component);

    assert(turbowasm_component_binary_load(
               &component,
               bad_version,
               sizeof(bad_version)) == TURBOWASM_MALFORMED_MODULE);
}

static void test_runtime_allocation_limit(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        0x07,0x01,0x00
    };
    turbowasm_component_binary component = {0};
    turbowasm_runtime_config config;

    turbowasm_runtime_config_init(&config);
    config.limits.max_allocation_bytes = 1u;

    assert(turbowasm_component_binary_load_with_config(
               &component,
               bytes,
               sizeof(bytes),
               &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(component.bytes == NULL);
}


static void test_type_import_export_semantics(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,

        /* 6 types */
        0x07,0x13,0x06,
        0x3f,0x7f,0x00,             /* resource(rep i32), no dtor */
        0x69,0x00,                  /* own type0 */
        0x68,0x00,                  /* borrow type0 */
        0x70,0x7d,                  /* list<u8> */
        0x40,0x01,0x01,'x',0x03,   /* func param x:type3 */
        0x00,0x79,                  /* result u32 */
        0x70,0x03,                  /* list<type3> */

        /* import "f" : func type4 */
        0x0a,0x06,0x01,
        0x00,0x01,'f',
        0x01,0x04,

        /* export "g" func0 ascribed func type4 */
        0x0b,0x09,0x01,
        0x00,0x01,'g',
        0x01,0x00,
        0x01,0x01,0x04
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_type *type;
    const turbowasm_component_import *import_desc;
    const turbowasm_component_export *export_desc;

    assert(turbowasm_component_binary_load(
               &component,bytes,sizeof(bytes)) == TURBOWASM_OK);

    assert(component.type_graph.count == 6u);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,0u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE);
    assert(type->as.resource.rep_type == 0x7fu);
    assert(!type->as.resource.has_destructor);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,1u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(type->as.handle.resource_type == 0u);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,2u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_BORROW);
    assert(type->as.handle.resource_type == 0u);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,3u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(type->as.list.element_type.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.list.element_type.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_U8);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,4u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION);
    assert(type->as.function.param_count == 1u);
    assert(type->as.function.params[0].kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(type->as.function.params[0].as.indexed == 3u);
    assert(type->as.function.has_result);
    assert(type->as.function.result.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.function.result.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_U32);

    type = turbowasm_component_type_graph_get(
        &component.type_graph,5u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(type->as.list.element_type.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(type->as.list.element_type.as.indexed == 3u);

    assert(component.import_count == 1u);
    import_desc = turbowasm_component_binary_import_at(
        &component,0u);
    assert(import_desc != NULL);
    assert(import_desc->kind ==
           TURBOWASM_COMPONENT_EXTERN_FUNCTION);
    assert(import_desc->type_index == 4u);
    assert(import_desc->name.size == 1u);
    assert(import_desc->name.bytes[0] == (uint8_t)'f');

    assert(component.export_count == 1u);
    export_desc = turbowasm_component_binary_export_at(
        &component,0u);
    assert(export_desc != NULL);
    assert(export_desc->kind ==
           TURBOWASM_COMPONENT_EXTERN_FUNCTION);
    assert(export_desc->item_index == 0u);
    assert(export_desc->has_ascribed_type);
    assert(export_desc->type_index == 4u);
    assert(export_desc->name.size == 1u);
    assert(export_desc->name.bytes[0] == (uint8_t)'g');

    turbowasm_component_binary_destroy(&component);
}

static void test_repeated_type_sections_preserve_index_space(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        0x07,0x02,0x01,0x73,       /* type0 = string */
        0x07,0x03,0x01,0x70,0x00   /* type1 = list<type0> */
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_type *type;

    assert(turbowasm_component_binary_load(
               &component,bytes,sizeof(bytes)) == TURBOWASM_OK);
    assert(component.type_graph.count == 2u);
    type = turbowasm_component_type_graph_get(
        &component.type_graph,1u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(type->as.list.element_type.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(type->as.list.element_type.as.indexed == 0u);
    turbowasm_component_binary_destroy(&component);
}

static void test_semantic_unsupported_and_invalid_forms(void) {
    static const uint8_t alias[] = {
        COMPONENT_HEADER,
        0x06,0x01,0x00
    };
    static const uint8_t async_func[] = {
        COMPONENT_HEADER,
        0x07,0x05,0x01,
        0x43,0x00,0x01,0x00
    };
    static const uint8_t own_non_resource[] = {
        COMPONENT_HEADER,
        0x07,0x04,0x02,
        0x73,
        0x69,0x00
    };
    static const uint8_t forward_list[] = {
        COMPONENT_HEADER,
        0x07,0x03,0x01,
        0x70,0x01
    };
    static const uint8_t type_import[] = {
        COMPONENT_HEADER,
        0x07,0x05,0x01,
        0x40,0x00,0x01,0x00,
        0x0a,0x06,0x01,
        0x00,0x01,'t',
        0x03,0x01
    };
    turbowasm_component_binary component = {0};

    assert(turbowasm_component_binary_load(
               &component,alias,sizeof(alias)) ==
           TURBOWASM_UNSUPPORTED);
    assert(turbowasm_component_binary_load(
               &component,async_func,sizeof(async_func)) ==
           TURBOWASM_UNSUPPORTED);
    assert(turbowasm_component_binary_load(
               &component,own_non_resource,sizeof(own_non_resource)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,forward_list,sizeof(forward_list)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(turbowasm_component_binary_load(
               &component,type_import,sizeof(type_import)) ==
           TURBOWASM_UNSUPPORTED);
}

int main(void) {
    test_empty_component();
    test_preamble_rejection();
    test_custom_section_framing();
    test_section_bounds();
    test_embedded_core_module_record();
    test_nested_component_boundary();
    test_runtime_allocation_limit();
    test_type_import_export_semantics();
    test_repeated_type_sections_preserve_index_space();
    test_semantic_unsupported_and_invalid_forms();
    return 0;
}
