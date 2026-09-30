#include "../src/component_type_graph.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void test_scalar_projection(void) {
    assert(cmeta_type_equal(
        turbowasm_component_scalar_cmeta_type(
            TURBOWASM_COMPONENT_TYPE_BOOL),
        &cmeta_type_bool));
    assert(cmeta_type_equal(
        turbowasm_component_scalar_cmeta_type(
            TURBOWASM_COMPONENT_TYPE_S32),
        &cmeta_type_int32));
    assert(cmeta_type_equal(
        turbowasm_component_scalar_cmeta_type(
            TURBOWASM_COMPONENT_TYPE_U64),
        &cmeta_type_uint64));
    assert(cmeta_type_equal(
        turbowasm_component_scalar_cmeta_type(
            TURBOWASM_COMPONENT_TYPE_F32),
        &cmeta_type_float));
    assert(cmeta_type_equal(
        turbowasm_component_scalar_cmeta_type(
            TURBOWASM_COMPONENT_TYPE_CHAR),
        &cmeta_type_uint32));
    assert(turbowasm_component_scalar_cmeta_type(
               TURBOWASM_COMPONENT_TYPE_STRING) == NULL);
}

static void test_indexed_type_graph(void) {
    turbowasm_component_type_graph graph = {0};
    const turbowasm_component_type *type;

    assert(turbowasm_component_type_graph_allocate(&graph, 7u));

    /* Forward reference: list<type1> before type1 is defined. */
    assert(turbowasm_component_type_graph_define_list(
        &graph, 0u, 1u));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_U8));
    assert(turbowasm_component_type_graph_define_string(
        &graph, 2u));
    assert(turbowasm_component_type_graph_define_resource(
        &graph, 3u, UINT64_C(0x1001)));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 4u, TURBOWASM_COMPONENT_TYPE_OWN, 3u));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 5u, TURBOWASM_COMPONENT_TYPE_BORROW, 3u));
    assert(turbowasm_component_type_graph_define_list(
        &graph, 6u, 0u));

    assert(turbowasm_component_type_graph_validate(&graph));

    type = turbowasm_component_type_graph_get(&graph, 0u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(type->as.list.element_type.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(type->as.list.element_type.as.indexed == 1u);

    type = turbowasm_component_type_graph_get(&graph, 4u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(type->as.handle.resource_type == 3u);

    assert(turbowasm_component_type_graph_get(&graph, 7u) == NULL);
    turbowasm_component_type_graph_destroy(&graph);
    assert(graph.types == NULL);
    assert(graph.count == 0u);
}

static void test_inline_and_function_refs(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref params[2];
    turbowasm_component_type_ref result;
    const turbowasm_component_type *type;

    assert(turbowasm_component_type_graph_allocate(&graph, 3u));
    assert(turbowasm_component_type_graph_define_list_ref(
        &graph, 0u,
        turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_U8)));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));

    params[0] = turbowasm_component_type_ref_indexed(0u);
    params[1] = turbowasm_component_type_ref_inline(
        TURBOWASM_COMPONENT_TYPE_STRING);
    result = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 2u, params, 2u, true, result));
    assert(turbowasm_component_type_graph_validate(&graph));

    type = turbowasm_component_type_graph_get(&graph, 0u);
    assert(type != NULL);
    assert(type->as.list.element_type.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.list.element_type.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_U8);

    type = turbowasm_component_type_graph_get(&graph, 2u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION);
    assert(type->as.function.param_count == 2u);
    assert(type->as.function.params[0].kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
    assert(type->as.function.params[0].as.indexed == 0u);
    assert(type->as.function.params[1].kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.function.params[1].as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_STRING);
    assert(type->as.function.has_result);
    assert(type->as.function.result.as.indexed == 1u);

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_invalid_resource_links_fail_closed(void) {
    turbowasm_component_type_graph graph = {0};

    assert(turbowasm_component_type_graph_allocate(&graph, 3u));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 0u, TURBOWASM_COMPONENT_TYPE_U32));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    assert(turbowasm_component_type_graph_define_resource(
        &graph, 2u, UINT64_C(9)));
    assert(!turbowasm_component_type_graph_validate(&graph));

    turbowasm_component_type_graph_destroy(&graph);

    assert(turbowasm_component_type_graph_allocate(&graph, 2u));
    assert(turbowasm_component_type_graph_define_resource(
        &graph, 0u, UINT64_C(7)));
    assert(turbowasm_component_type_graph_define_resource(
        &graph, 1u, UINT64_C(7)));
    assert(!turbowasm_component_type_graph_validate(&graph));
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_incomplete_graph_fails_closed(void) {
    turbowasm_component_type_graph graph = {0};

    assert(turbowasm_component_type_graph_allocate(&graph, 2u));
    assert(turbowasm_component_type_graph_define_string(&graph, 0u));
    assert(!turbowasm_component_type_graph_validate(&graph));
    assert(!turbowasm_component_type_graph_define_string(&graph, 0u));
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_borrow_result_is_rejected(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref result;

    assert(turbowasm_component_type_graph_allocate(&graph, 3u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(0x7001), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_BORROW, 0u));
    result = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 2u, NULL, 0u, true, result));
    assert(!turbowasm_component_type_graph_validate(&graph));

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_composite_type_graph(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_record_field fields[2];
    turbowasm_component_type_ref tuple_elements[2];
    turbowasm_component_type_ref unit = {0};
    const turbowasm_component_type *type;
    static const uint8_t first_name[] = "seconds";
    static const uint8_t second_name[] = "nanoseconds";

    assert(turbowasm_component_type_graph_allocate(&graph, 8u));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 0u, TURBOWASM_COMPONENT_TYPE_U64));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));

    fields[0].name = first_name;
    fields[0].name_size = 7u;
    fields[0].type = turbowasm_component_type_ref_indexed(0u);
    fields[1].name = second_name;
    fields[1].name_size = 11u;
    fields[1].type = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_record(
        &graph, 2u, fields, 2u));

    tuple_elements[0] =
        turbowasm_component_type_ref_indexed(0u);
    tuple_elements[1] =
        turbowasm_component_type_ref_indexed(0u);
    assert(turbowasm_component_type_graph_define_tuple(
        &graph, 3u, tuple_elements, 2u));

    assert(turbowasm_component_type_graph_define_string(
        &graph, 4u));
    assert(turbowasm_component_type_graph_define_option(
        &graph, 5u,
        turbowasm_component_type_ref_indexed(4u)));
    assert(turbowasm_component_type_graph_define_result(
        &graph, 6u, false, unit, false, unit));
    assert(turbowasm_component_type_graph_define_list_ref(
        &graph, 7u,
        turbowasm_component_type_ref_indexed(3u)));

    assert(turbowasm_component_type_graph_validate(&graph));

    type = turbowasm_component_type_graph_get(&graph, 2u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_RECORD);
    assert(type->as.record.count == 2u);
    assert(type->as.record.fields[0].type.as.indexed == 0u);
    assert(type->as.record.fields[1].type.as.indexed == 1u);

    type = turbowasm_component_type_graph_get(&graph, 3u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_TUPLE);
    assert(type->as.tuple.count == 2u);

    type = turbowasm_component_type_graph_get(&graph, 5u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_OPTION);
    assert(type->as.option.payload.as.indexed == 4u);

    type = turbowasm_component_type_graph_get(&graph, 6u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_RESULT);
    assert(!type->as.result.has_ok);
    assert(!type->as.result.has_error);

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_nested_borrow_result_is_rejected(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref result;

    assert(turbowasm_component_type_graph_allocate(&graph, 4u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(0x8001), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_BORROW, 0u));
    assert(turbowasm_component_type_graph_define_option(
        &graph, 2u,
        turbowasm_component_type_ref_indexed(1u)));
    result = turbowasm_component_type_ref_indexed(2u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 3u, NULL, 0u, true, result));
    assert(!turbowasm_component_type_graph_validate(&graph));

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_enum_and_flags_types(void) {
    turbowasm_component_type_graph graph = {0};
    static const uint8_t a[] = "a";
    static const uint8_t b[] = "b";
    static const uint8_t c[] = "c";
    turbowasm_component_label enum_labels[3] = {
        {a, 1u}, {b, 1u}, {c, 1u}
    };
    turbowasm_component_label flag_labels[2] = {
        {a, 1u}, {b, 1u}
    };
    turbowasm_component_label duplicate[2] = {
        {a, 1u}, {a, 1u}
    };
    const turbowasm_component_type *type;

    assert(turbowasm_component_type_graph_allocate(&graph, 2u));
    assert(turbowasm_component_type_graph_define_enum(
        &graph, 0u, enum_labels, 3u));
    assert(turbowasm_component_type_graph_define_flags(
        &graph, 1u, flag_labels, 2u));
    assert(turbowasm_component_type_graph_validate(&graph));

    type = turbowasm_component_type_graph_get(&graph, 0u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_ENUM);
    assert(type->as.enumeration.count == 3u);

    type = turbowasm_component_type_graph_get(&graph, 1u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_FLAGS);
    assert(type->as.flags.count == 2u);

    turbowasm_component_type_graph_destroy(&graph);

    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(!turbowasm_component_type_graph_define_enum(
        &graph, 0u, duplicate, 2u));
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_variant_type_graph(void) {
    turbowasm_component_type_graph graph = {0};
    static const uint8_t ok_name[] = "ok";
    static const uint8_t value_name[] = "value";
    static const uint8_t closed_name[] = "closed";
    turbowasm_component_variant_case cases[3] = {{0}};
    const turbowasm_component_type *type;

    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    cases[0].name = ok_name;
    cases[0].name_size = 2u;
    cases[1].name = value_name;
    cases[1].name_size = 5u;
    cases[1].has_payload = true;
    cases[1].payload =
        turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_U32);
    cases[2].name = closed_name;
    cases[2].name_size = 6u;

    assert(turbowasm_component_type_graph_define_variant(
        &graph, 0u, cases, 3u));
    assert(turbowasm_component_type_graph_validate(&graph));

    type = turbowasm_component_type_graph_get(&graph, 0u);
    assert(type != NULL);
    assert(type->kind == TURBOWASM_COMPONENT_TYPE_VARIANT);
    assert(type->as.variant.count == 3u);
    assert(!type->as.variant.cases[0].has_payload);
    assert(type->as.variant.cases[1].has_payload);
    assert(type->as.variant.cases[1].payload.kind ==
           TURBOWASM_COMPONENT_TYPE_REF_INLINE);
    assert(type->as.variant.cases[1].payload.as.inline_type ==
           TURBOWASM_COMPONENT_TYPE_U32);
    assert(type->as.variant.cases[2].name_size == 6u);

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_variant_borrow_result_is_rejected(void) {
    turbowasm_component_type_graph graph = {0};
    static const uint8_t borrowed_name[] = "borrowed";
    turbowasm_component_variant_case cases[1] = {{0}};
    turbowasm_component_type_ref result;

    assert(turbowasm_component_type_graph_allocate(&graph, 4u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(0x9001), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_BORROW, 0u));

    cases[0].name = borrowed_name;
    cases[0].name_size = 8u;
    cases[0].has_payload = true;
    cases[0].payload =
        turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_variant(
        &graph, 2u, cases, 1u));

    result = turbowasm_component_type_ref_indexed(2u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 3u, NULL, 0u, true, result));
    assert(!turbowasm_component_type_graph_validate(&graph));

    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    test_scalar_projection();
    test_indexed_type_graph();
    test_inline_and_function_refs();
    test_invalid_resource_links_fail_closed();
    test_incomplete_graph_fails_closed();
    test_borrow_result_is_rejected();
    test_composite_type_graph();
    test_nested_borrow_result_is_rejected();
    test_enum_and_flags_types();
    test_variant_type_graph();
    test_variant_borrow_result_is_rejected();
    return 0;
}
