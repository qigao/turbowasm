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

int main(void) {
    test_scalar_projection();
    test_indexed_type_graph();
    test_inline_and_function_refs();
    test_invalid_resource_links_fail_closed();
    test_incomplete_graph_fails_closed();
    return 0;
}
