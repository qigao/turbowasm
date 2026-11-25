#include "../src/wasi02_streams.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct bridge_probe {
    uint32_t input_subscribe_calls;
    uint32_t output_subscribe_calls;
    uint32_t poll_drop_calls;
    uint32_t input_drop_calls;
    uint32_t output_drop_calls;
} bridge_probe;

static turbowasm_status input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    bridge_probe *probe = (bridge_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_pollable_rep != NULL);
    ++probe->input_subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 77;
    return TURBOWASM_OK;
}

static turbowasm_status output_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    bridge_probe *probe = (bridge_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_pollable_rep != NULL);
    ++probe->output_subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 88;
    return TURBOWASM_OK;
}

static void input_drop(
    void *context,
    turbowasm_value rep) {
    bridge_probe *probe = (bridge_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static void output_drop(
    void *context,
    turbowasm_value rep) {
    bridge_probe *probe = (bridge_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 22);
    ++probe->output_drop_calls;
}

static turbowasm_status poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    (void)context;
    (void)rep;
    if (out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_ready = true;
    return TURBOWASM_OK;
}

static turbowasm_status poll_drop(
    void *context,
    turbowasm_value rep) {
    bridge_probe *probe = (bridge_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77 || rep.as.i64 == 88);
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_component_name cname(const char *text) {
    turbowasm_component_name name;
    name.bytes = (const uint8_t *)text;
    name.size = (uint32_t)strlen(text);
    return name;
}

static void build_graph(
    turbowasm_component_type_graph *graph) {
    turbowasm_component_type_ref params[1];

    assert(turbowasm_component_type_graph_allocate(
               graph, 11u));

    /* input-stream identity 0x101 -> borrow -> subscribe -> own pollable */
    assert(turbowasm_component_type_graph_define_resource(
               graph, 0u, UINT64_C(0x101)));
    assert(turbowasm_component_type_graph_define_handle(
               graph, 1u,
               TURBOWASM_COMPONENT_TYPE_BORROW,
               0u));

    /* pollable identity 0x202 -> own */
    assert(turbowasm_component_type_graph_define_resource(
               graph, 2u, UINT64_C(0x202)));
    assert(turbowasm_component_type_graph_define_handle(
               graph, 3u,
               TURBOWASM_COMPONENT_TYPE_OWN,
               2u));

    params[0] = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_function(
               graph, 4u,
               params, 1u,
               true,
               turbowasm_component_type_ref_indexed(3u)));

    /* output-stream identity 0x303 -> borrow -> subscribe -> same pollable */
    assert(turbowasm_component_type_graph_define_resource(
               graph, 5u, UINT64_C(0x303)));
    assert(turbowasm_component_type_graph_define_handle(
               graph, 6u,
               TURBOWASM_COMPONENT_TYPE_BORROW,
               5u));
    params[0] = turbowasm_component_type_ref_indexed(6u);
    assert(turbowasm_component_type_graph_define_function(
               graph, 7u,
               params, 1u,
               true,
               turbowasm_component_type_ref_indexed(3u)));

    /* error identity 0x404 -> borrow -> string */
    assert(turbowasm_component_type_graph_define_resource(
               graph, 8u, UINT64_C(0x404)));
    assert(turbowasm_component_type_graph_define_handle(
               graph, 9u,
               TURBOWASM_COMPONENT_TYPE_BORROW,
               8u));
    params[0] = turbowasm_component_type_ref_indexed(9u);
    assert(turbowasm_component_type_graph_define_function(
               graph, 10u,
               params, 1u,
               true,
               turbowasm_component_type_ref_inline(
                   TURBOWASM_COMPONENT_TYPE_STRING)));

    assert(turbowasm_component_type_graph_validate(graph));
}

static void test_stream_and_poll_nominal_routing(void) {
    bridge_probe probe = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_component_exec_imports stream_imports = {0};
    turbowasm_component_exec_imports poll_imports = {0};
    turbowasm_component_type_graph graph = {0};
    turbowasm_value rep = {0};
    turbowasm_wasi02_value args[1] = {{0}};
    turbowasm_wasi02_value subscribe_result = {0};
    turbowasm_component_value component_resource = {0};
    uint32_t input_resource = 0u;
    uint32_t output_resource = 0u;
    uint32_t lowered = 0u;
    uint32_t pollable_resource;

    poll_provider.context = &probe;
    poll_provider.ready = poll_ready;
    poll_provider.drop = poll_drop;

    stream_provider.context = &probe;
    stream_provider.input_subscribe = input_subscribe;
    stream_provider.output_subscribe = output_subscribe;
    stream_provider.input_drop = input_drop;
    stream_provider.output_drop = output_drop;

    assert(turbowasm_wasi02_poll_init(
               &poll,
               &poll_provider,
               8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_init(
               &streams,
               &stream_provider,
               8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams,
               &poll) == TURBOWASM_OK);

    assert(turbowasm_wasi02_streams_imports(
               &streams,
               &stream_imports) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_imports(
               &poll,
               &poll_imports) == TURBOWASM_OK);

    build_graph(&graph);

    assert(stream_imports.can_bind(
        stream_imports.context,
        cname("wasi:io/streams@0.2.8"),
        cname("[method]input-stream.subscribe"),
        &graph,
        4u));
    assert(stream_imports.can_bind(
        stream_imports.context,
        cname("wasi:io/streams@0.2.8"),
        cname("[method]output-stream.subscribe"),
        &graph,
        7u));
    assert(stream_imports.can_bind(
        stream_imports.context,
        cname("wasi:io/error@0.2.8"),
        cname("[method]error.to-debug-string"),
        &graph,
        10u));

    assert(streams.component_input_identity_bound);
    assert(streams.component_input_identity == UINT64_C(0x101));
    assert(streams.component_output_identity_bound);
    assert(streams.component_output_identity == UINT64_C(0x303));
    assert(streams.component_error_identity_bound);
    assert(streams.component_error_identity == UINT64_C(0x404));
    assert(poll.pollable_identity_bound);
    assert(poll.pollable_identity == UINT64_C(0x202));

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &streams,
               rep,
               &input_resource) == TURBOWASM_OK);
    rep.as.i64 = 22;
    assert(turbowasm_wasi02_output_stream_new(
               &streams,
               rep,
               &output_resource) == TURBOWASM_OK);

    component_resource.kind = TURBOWASM_COMPONENT_TYPE_BORROW;
    component_resource.as.resource_rep.kind = TURBOWASM_VALUE_I32;
    component_resource.as.resource_rep.as.i32 =
        (int32_t)input_resource;
    assert(stream_imports.resource_lower(
               stream_imports.context,
               &graph,
               turbowasm_component_type_ref_indexed(1u),
               &component_resource,
               &lowered) == TURBOWASM_OK);
    assert(lowered == input_resource);

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = input_resource;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.subscribe",
               args,
               1u,
               &subscribe_result) == TURBOWASM_OK);
    assert(subscribe_result.kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    pollable_resource = subscribe_result.as.resource;
    turbowasm_wasi02_value_destroy(&subscribe_result);

    /*
     * Pollable is nominally foreign to Streams and must fall through to the
     * Poll capability set.
     */
    assert(stream_imports.resource_lift(
               stream_imports.context,
               &graph,
               turbowasm_component_type_ref_indexed(3u),
               pollable_resource,
               &component_resource) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(poll_imports.resource_lift(
               poll_imports.context,
               &graph,
               turbowasm_component_type_ref_indexed(3u),
               pollable_resource,
               &component_resource) == TURBOWASM_OK);
    assert(component_resource.kind ==
           TURBOWASM_COMPONENT_TYPE_OWN);
    assert((uint32_t)component_resource.as.resource_rep.as.i32 ==
           pollable_resource);

    assert(stream_imports.resource_drop(
               stream_imports.context,
               UINT64_C(0x202),
               pollable_resource) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(poll_imports.resource_drop(
               poll_imports.context,
               UINT64_C(0x202),
               pollable_resource) == TURBOWASM_OK);
    assert(probe.poll_drop_calls == 1u);

    /*
     * A valid stream handle under the wrong nominal identity must not be
     * silently accepted by the internal multi-kind table.
     */
    assert(stream_imports.resource_drop(
               stream_imports.context,
               UINT64_C(0x303),
               input_resource) == TURBOWASM_TRAPPED);
    assert(probe.input_drop_calls == 0u);

    assert(stream_imports.resource_drop(
               stream_imports.context,
               UINT64_C(0x101),
               input_resource) == TURBOWASM_OK);
    assert(stream_imports.resource_drop(
               stream_imports.context,
               UINT64_C(0x303),
               output_resource) == TURBOWASM_OK);
    assert(probe.input_drop_calls == 1u);
    assert(probe.output_drop_calls == 1u);

    turbowasm_component_type_graph_destroy(&graph);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
}

int main(void) {
    test_stream_and_poll_nominal_routing();
    return 0;
}
