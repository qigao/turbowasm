#include "../src/component_canonical.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static turbowasm_component_type_ref inline_ref(
    turbowasm_component_type_kind kind) {
    return turbowasm_component_type_ref_inline(kind);
}

static turbowasm_component_type_ref indexed_ref(uint32_t id) {
    return turbowasm_component_type_ref_indexed(id);
}

static void build_graph(turbowasm_component_type_graph *graph) {
    turbowasm_component_type_ref params6[5];
    turbowasm_component_type_ref params7[9];
    turbowasm_component_type_ref params8[1];
    turbowasm_component_type_ref params9[16];
    uint32_t i;

    assert(turbowasm_component_type_graph_allocate(graph, 10u));

    assert(turbowasm_component_type_graph_define_scalar(
        graph, 0u, TURBOWASM_COMPONENT_TYPE_U64));
    assert(turbowasm_component_type_graph_define_string(
        graph, 1u));
    assert(turbowasm_component_type_graph_define_list_ref(
        graph, 2u, inline_ref(TURBOWASM_COMPONENT_TYPE_U8)));
    assert(turbowasm_component_type_graph_define_resource_full(
        graph, 3u, UINT64_C(1), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        graph, 4u, TURBOWASM_COMPONENT_TYPE_OWN, 3u));
    assert(turbowasm_component_type_graph_define_handle(
        graph, 5u, TURBOWASM_COMPONENT_TYPE_BORROW, 3u));

    params6[0] = inline_ref(TURBOWASM_COMPONENT_TYPE_BOOL);
    params6[1] = indexed_ref(0u);
    params6[2] = indexed_ref(1u);
    params6[3] = indexed_ref(2u);
    params6[4] = indexed_ref(4u);
    assert(turbowasm_component_type_graph_define_function(
        graph, 6u, params6, 5u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_F64)));

    for (i = 0u; i < 9u; ++i)
        params7[i] = inline_ref(TURBOWASM_COMPONENT_TYPE_STRING);
    assert(turbowasm_component_type_graph_define_function(
        graph, 7u, params7, 9u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U32)));

    params8[0] = inline_ref(TURBOWASM_COMPONENT_TYPE_U32);
    assert(turbowasm_component_type_graph_define_function(
        graph, 8u, params8, 1u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_STRING)));

    for (i = 0u; i < 16u; ++i)
        params9[i] = inline_ref(TURBOWASM_COMPONENT_TYPE_U32);
    assert(turbowasm_component_type_graph_define_function(
        graph, 9u, params9, 16u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_STRING)));

    assert(turbowasm_component_type_graph_validate(graph));
}

static void test_layouts(
    const turbowasm_component_type_graph *graph) {
    turbowasm_component_layout layout;

    assert(turbowasm_component_canonical_layout(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_BOOL),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 1u);
    assert(layout.size == 1u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(0u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 8u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(1u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 4u);
    assert(layout.size == 8u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(1u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 16u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(2u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 4u);
    assert(layout.size == 8u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(4u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 4u);
    assert(layout.size == 4u);

    assert(turbowasm_component_canonical_layout(
               graph, indexed_ref(3u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_UNSUPPORTED);
}

static void test_flat_types(
    const turbowasm_component_type_graph *graph) {
    turbowasm_component_flat_type_list flat;

    assert(turbowasm_component_canonical_flatten_type(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_S16),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 1u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_type(
               graph, indexed_ref(0u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 1u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I64);

    assert(turbowasm_component_canonical_flatten_type(
               graph, indexed_ref(1u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 2u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_type(
               graph, indexed_ref(2u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 2u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I64);

    assert(turbowasm_component_canonical_flatten_type(
               graph, indexed_ref(5u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 1u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);
}

static void test_function_flatten(
    const turbowasm_component_type_graph *graph) {
    turbowasm_component_flat_signature sig;

    assert(turbowasm_component_canonical_flatten_function(
               graph, 6u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(!sig.params_indirect);
    assert(!sig.results_indirect);
    assert(sig.param_count == 7u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[1] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(sig.params[2] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[3] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[4] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[5] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[6] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.result_count == 1u);
    assert(sig.results[0] == TURBOWASM_COMPONENT_FLAT_F64);

    assert(turbowasm_component_canonical_flatten_function(
               graph, 7u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(sig.params_indirect);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.result_count == 1u);
    assert(sig.results[0] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_function(
               graph, 7u,
               TURBOWASM_COMPONENT_POINTER_I64,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(sig.params_indirect);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I64);

    assert(turbowasm_component_canonical_flatten_function(
               graph, 8u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.results_indirect);
    assert(sig.result_count == 1u);
    assert(sig.results[0] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_function(
               graph, 8u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LOWER,
               &sig) == TURBOWASM_OK);
    assert(sig.param_count == 2u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[1] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.results_indirect);
    assert(sig.result_count == 0u);

    assert(turbowasm_component_canonical_flatten_function(
               graph, 9u,
               TURBOWASM_COMPONENT_POINTER_I64,
               TURBOWASM_COMPONENT_CANONICAL_LOWER,
               &sig) == TURBOWASM_OK);
    assert(sig.param_count == 17u);
    assert(sig.params[15] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.params[16] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(sig.result_count == 0u);
    assert(sig.results_indirect);
}

static void test_composite_metadata(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_record_field fields[2];
    turbowasm_component_type_ref tuple_elements[2];
    turbowasm_component_type_ref no_payload = {0};
    turbowasm_component_layout layout;
    turbowasm_component_flat_type_list flat;
    turbowasm_component_flat_signature sig;
    static const uint8_t seconds[] = "seconds";
    static const uint8_t nanoseconds[] = "nanoseconds";

    assert(turbowasm_component_type_graph_allocate(&graph, 10u));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 0u, TURBOWASM_COMPONENT_TYPE_U64));
    assert(turbowasm_component_type_graph_define_scalar(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));

    fields[0].name = seconds;
    fields[0].name_size = 7u;
    fields[0].type = indexed_ref(0u);
    fields[1].name = nanoseconds;
    fields[1].name_size = 11u;
    fields[1].type = indexed_ref(1u);
    assert(turbowasm_component_type_graph_define_record(
        &graph, 2u, fields, 2u));

    tuple_elements[0] = indexed_ref(0u);
    tuple_elements[1] = indexed_ref(0u);
    assert(turbowasm_component_type_graph_define_tuple(
        &graph, 3u, tuple_elements, 2u));

    assert(turbowasm_component_type_graph_define_string(
        &graph, 4u));
    assert(turbowasm_component_type_graph_define_option(
        &graph, 5u, indexed_ref(4u)));
    assert(turbowasm_component_type_graph_define_result(
        &graph, 6u, false, no_payload, false, no_payload));
    assert(turbowasm_component_type_graph_define_result(
        &graph, 7u,
        true, inline_ref(TURBOWASM_COMPONENT_TYPE_U32),
        true, inline_ref(TURBOWASM_COMPONENT_TYPE_F64)));

    assert(turbowasm_component_type_graph_define_function(
        &graph, 8u, NULL, 0u, true, indexed_ref(2u)));
    assert(turbowasm_component_type_graph_define_function(
        &graph, 9u, NULL, 0u, true, indexed_ref(5u)));
    assert(turbowasm_component_type_graph_validate(&graph));

    /* datetime = record { u64, u32 } => align 8, padded size 16. */
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(2u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 16u);
    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(2u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 2u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I32);

    /* insecure-seed tuple<u64,u64>. */
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(3u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 16u);
    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(3u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 2u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I64);

    /* option<string> is a 2-case variant with an aligned string payload. */
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(5u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 4u);
    assert(layout.size == 12u);
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(5u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 24u);
    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(5u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 3u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(flat.types[2] == TURBOWASM_COMPONENT_FLAT_I32);

    /* result<(),()> has only the u8 discriminant in memory / i32 flat. */
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(6u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 1u);
    assert(layout.size == 1u);
    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(6u),
               TURBOWASM_COMPONENT_POINTER_I64,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 1u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);

    /* Variant payload joining: u32 + f64 => joined i64 carrier. */
    assert(turbowasm_component_canonical_layout(
               &graph, indexed_ref(7u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &layout) == TURBOWASM_OK);
    assert(layout.alignment == 8u);
    assert(layout.size == 16u);
    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(7u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 2u);
    assert(flat.types[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(flat.types[1] == TURBOWASM_COMPONENT_FLAT_I64);

    /* Two-carrier records/results cross the 1-result indirect threshold. */
    assert(turbowasm_component_canonical_flatten_function(
               &graph, 8u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(sig.results_indirect);
    assert(sig.result_count == 1u);
    assert(sig.results[0] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_function(
               &graph, 8u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LOWER,
               &sig) == TURBOWASM_OK);
    assert(sig.results_indirect);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.result_count == 0u);

    assert(turbowasm_component_canonical_flatten_function(
               &graph, 9u,
               TURBOWASM_COMPONENT_POINTER_I64,
               TURBOWASM_COMPONENT_CANONICAL_LOWER,
               &sig) == TURBOWASM_OK);
    assert(sig.results_indirect);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I64);
    assert(sig.result_count == 0u);

    turbowasm_component_type_graph_destroy(&graph);
}

static void test_composite_flat_limit(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref elements[17];
    turbowasm_component_type_ref params[1];
    turbowasm_component_flat_type_list flat;
    turbowasm_component_flat_signature sig;
    uint32_t i;

    assert(turbowasm_component_type_graph_allocate(&graph, 2u));
    for (i = 0u; i < 17u; ++i)
        elements[i] = inline_ref(TURBOWASM_COMPONENT_TYPE_U32);
    assert(turbowasm_component_type_graph_define_tuple(
        &graph, 0u, elements, 17u));

    params[0] = indexed_ref(0u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 1u, params, 1u, false,
        inline_ref(TURBOWASM_COMPONENT_TYPE_BOOL)));
    assert(turbowasm_component_type_graph_validate(&graph));

    assert(turbowasm_component_canonical_flatten_type(
               &graph, indexed_ref(0u),
               TURBOWASM_COMPONENT_POINTER_I32,
               &flat) == TURBOWASM_OK);
    assert(flat.count == 17u);
    for (i = 0u; i < 17u; ++i)
        assert(flat.types[i] == TURBOWASM_COMPONENT_FLAT_I32);

    assert(turbowasm_component_canonical_flatten_function(
               &graph, 1u,
               TURBOWASM_COMPONENT_POINTER_I32,
               TURBOWASM_COMPONENT_CANONICAL_LIFT,
               &sig) == TURBOWASM_OK);
    assert(sig.params_indirect);
    assert(sig.param_count == 1u);
    assert(sig.params[0] == TURBOWASM_COMPONENT_FLAT_I32);
    assert(sig.result_count == 0u);

    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    turbowasm_component_type_graph graph = {0};

    build_graph(&graph);
    test_layouts(&graph);
    test_flat_types(&graph);
    test_function_flatten(&graph);
    turbowasm_component_type_graph_destroy(&graph);

    test_composite_metadata();
    test_composite_flat_limit();
    return 0;
}
