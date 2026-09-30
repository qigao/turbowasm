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

static void test_component_module_size_limit(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER
    };
    turbowasm_component_binary component = {0};
    turbowasm_runtime_config config;

    turbowasm_runtime_config_init(&config);
    config.limits.max_module_bytes = sizeof(bytes) - 1u;

    assert(turbowasm_component_binary_load_with_config(
               &component,
               bytes,
               sizeof(bytes),
               &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(component.bytes == NULL);
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

static void test_flat_instance_import_alias_and_lower(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,

        /*
         * type0 = instance {
         *   local type0 = func() -> u64;
         *   export "now" : func local-type0;
         * }
         */
        0x07,0x10,0x01,
          0x42,0x02,
            0x01,0x40,0x00,0x00,0x77,
            0x04,0x00,0x03,'n','o','w',0x01,0x00,

        /* import "clock" : instance type0 => component instance0 */
        0x0a,0x0a,0x01,
          0x00,0x05,'c','l','o','c','k',
          0x05,0x00,

        /* component func0 = alias export instance0 "now" */
        0x06,0x08,0x01,
          0x01,0x00,0x00,
          0x03,'n','o','w',

        /* core func0 = canon lower component func0, no options */
        0x08,0x05,0x01,
          0x01,0x00,0x00,0x00
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_type *instance_type;
    const turbowasm_component_import *import_desc;
    const turbowasm_component_function_alias *alias;
    const turbowasm_component_canon_lower *lower;

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);

    assert(component.type_graph.count == 1u);
    instance_type = turbowasm_component_type_graph_get(
        &component.type_graph, 0u);
    assert(instance_type != NULL);
    assert(instance_type->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE);
    assert(instance_type->as.instance != NULL);
    assert(instance_type->as.instance->type_graph.count == 1u);
    assert(instance_type->as.instance->export_count == 1u);
    assert(instance_type->as.instance->exports[0].name_size == 3u);
    assert(instance_type->as.instance->exports[0].kind ==
           TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION);
    assert(instance_type->as.instance->exports[0].type_index == 0u);

    assert(component.import_count == 1u);
    import_desc = turbowasm_component_binary_import_at(
        &component, 0u);
    assert(import_desc != NULL);
    assert(import_desc->kind == TURBOWASM_COMPONENT_EXTERN_INSTANCE);
    assert(import_desc->type_index == 0u);
    assert(import_desc->item_index == 0u);
    assert(component.component_instance_index_count == 1u);

    assert(component.component_function_alias_count == 1u);
    alias = turbowasm_component_binary_component_function_alias_at(
        &component, 0u);
    assert(alias != NULL);
    assert(alias->instance_index == 0u);
    assert(alias->component_function_index == 0u);

    assert(component.canon_lower_count == 1u);
    lower = turbowasm_component_binary_canon_lower_at(
        &component, 0u);
    assert(lower != NULL);
    assert(lower->component_function_index == 0u);
    assert(lower->core_function_index == 0u);
    assert(component.component_function_count == 1u);
    assert(component.core_function_count == 1u);

    turbowasm_component_binary_destroy(&component);
}

static void test_composite_type_binary_retention(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        /* type0 tuple<u64,u64>; type1 list<string>;
         * type2 option<string>; type3 result<(),()> */
        0x07,0x0c,0x04,
          0x6f,0x02,0x77,0x77,
          0x70,0x73,
          0x6b,0x73,
          0x6a,0x00,0x00
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_type *type;

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(component.type_graph.count == 4u);

    type = turbowasm_component_type_graph_get(
        &component.type_graph, 0u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_TUPLE);
    assert(type->as.tuple.count == 2u);
    assert(type->as.tuple.elements[0].kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.tuple.elements[0].as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_U64);

    type = turbowasm_component_type_graph_get(
        &component.type_graph, 1u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(type->as.list.element_type.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_STRING);

    type = turbowasm_component_type_graph_get(
        &component.type_graph, 2u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_OPTION);
    assert(type->as.option.payload.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_STRING);

    type = turbowasm_component_type_graph_get(
        &component.type_graph, 3u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_RESULT);
    assert(!type->as.result.has_ok);
    assert(!type->as.result.has_error);

    turbowasm_component_binary_destroy(&component);
}

static void test_named_record_inside_instance_type(void) {
    static const uint8_t bytes[] = {
        COMPONENT_HEADER,
        /*
         * type0 = instance {
         *   type0 = record { seconds: u64, nanoseconds: u32 };
         *   export "datetime" type eq0 -> local type1;
         *   type2 = func() -> type1;
         *   export "now" func type2;
         * }
         */
        0x07,0x37,0x01,
          0x42,0x04,
            0x01,0x72,0x02,
              0x07,'s','e','c','o','n','d','s',0x77,
              0x0b,'n','a','n','o','s','e','c','o','n','d','s',0x79,
            0x04,0x00,0x08,'d','a','t','e','t','i','m','e',
              0x03,0x00,0x00,
            0x01,0x40,0x00,0x00,0x01,
            0x04,0x00,0x03,'n','o','w',0x01,0x02
    };
    turbowasm_component_binary component = {0};
    const turbowasm_component_type *outer;
    const turbowasm_component_type *record;
    const turbowasm_component_type *clone;
    const turbowasm_component_type *function;

    assert(turbowasm_component_binary_load(
               &component, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(component.type_graph.count == 1u);

    outer = turbowasm_component_type_graph_get(
        &component.type_graph, 0u);
    assert(outer != NULL);
    assert(outer->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE);
    assert(outer->as.instance != NULL);
    assert(outer->as.instance->type_graph.count == 3u);
    assert(outer->as.instance->export_count == 1u);
    assert(outer->as.instance->exports[0].name_size == 3u);
    assert(outer->as.instance->exports[0].kind ==
           TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION);
    assert(outer->as.instance->exports[0].type_index == 2u);

    record = turbowasm_component_type_graph_get(
        &outer->as.instance->type_graph, 0u);
    clone = turbowasm_component_type_graph_get(
        &outer->as.instance->type_graph, 1u);
    function = turbowasm_component_type_graph_get(
        &outer->as.instance->type_graph, 2u);

    assert(record != NULL);
    assert(record->kind == TURBOWASM_COMPONENT_TYPE_RECORD);
    assert(record->as.record.count == 2u);
    assert(clone != NULL);
    assert(clone->kind == TURBOWASM_COMPONENT_TYPE_RECORD);
    assert(clone->as.record.count == 2u);
    assert(function != NULL);
    assert(function->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION);
    assert(function->as.function.has_result);
    assert(function->as.function.result.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(function->as.function.result.as.indexed == 1u);

    turbowasm_component_binary_destroy(&component);
}

static void test_semantic_unsupported_and_invalid_forms(void) {
    static const uint8_t alias[] = {
        COMPONENT_HEADER,
        /* one unsupported outer alias of a core func */
        0x06,0x06,0x01,
        0x00,0x00, /* core func sort */
        0x02,      /* outer alias */
        0x00,0x00
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
    static const uint8_t forward_resource_destructor[] = {
        COMPONENT_HEADER,
        /* resource(rep i32, dtor corefunc0) before any Core function exists */
        0x07,0x05,0x01,
        0x3f,0x7f,0x01,0x00
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
    assert(turbowasm_component_binary_load(
               &component,
               forward_resource_destructor,
               sizeof(forward_resource_destructor)) ==
           TURBOWASM_MALFORMED_MODULE);
}

int main(void) {
    test_empty_component();
    test_preamble_rejection();
    test_custom_section_framing();
    test_section_bounds();
    test_embedded_core_module_record();
    test_nested_component_boundary();
    test_runtime_allocation_limit();
    test_component_module_size_limit();
    test_type_import_export_semantics();
    test_repeated_type_sections_preserve_index_space();
    test_flat_instance_import_alias_and_lower();
    test_composite_type_binary_retention();
    test_named_record_inside_instance_type();
    test_semantic_unsupported_and_invalid_forms();
    return 0;
}
