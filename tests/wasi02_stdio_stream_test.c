#include "../src/wasi02_streams.h"
#include "../src/component_type_graph.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct stdio_probe {
    uint32_t stdin_calls;
    uint32_t stdout_calls;
    uint32_t stderr_calls;
    uint32_t input_reads;
    uint32_t input_drops;
    uint32_t output_drops;
    uint32_t next_rep;
} stdio_probe;

static turbowasm_status make_stdin(
    void *context,
    turbowasm_value *out_rep) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL && out_rep != NULL);
    ++probe->stdin_calls;
    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)(++probe->next_rep);
    return TURBOWASM_OK;
}

static turbowasm_status make_stdout(
    void *context,
    turbowasm_value *out_rep) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL && out_rep != NULL);
    ++probe->stdout_calls;
    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)(++probe->next_rep);
    return TURBOWASM_OK;
}

static turbowasm_status make_stderr(
    void *context,
    turbowasm_value *out_rep) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL && out_rep != NULL);
    ++probe->stderr_calls;
    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)(++probe->next_rep);
    return TURBOWASM_OK;
}

static turbowasm_status read_stdin(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    static const uint8_t bytes[] = {'o','k'};
    stdio_probe *probe = (stdio_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(out_data != NULL && out_size != NULL && out_error != NULL);
    ++probe->input_reads;
    memset(out_error, 0, sizeof(*out_error));
    if (max_bytes == 0u) {
        *out_data = NULL;
        *out_size = 0u;
    } else {
        *out_data = bytes;
        *out_size = max_bytes < 2u ? (size_t)max_bytes : 2u;
    }
    return TURBOWASM_OK;
}

static void drop_input(
    void *context,
    turbowasm_value rep) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    ++probe->input_drops;
}

static void drop_output(
    void *context,
    turbowasm_value rep) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    ++probe->output_drops;
}

static turbowasm_component_name cname(const char *text) {
    turbowasm_component_name name;
    name.bytes = (const uint8_t *)text;
    name.size = (uint32_t)strlen(text);
    return name;
}

static void build_stdin_graph(
    turbowasm_component_type_graph *graph) {
    assert(turbowasm_component_type_graph_allocate(graph, 3u));
    assert(turbowasm_component_type_graph_define_resource(
               graph, 0u, UINT64_C(0x7111)));
    assert(turbowasm_component_type_graph_define_handle(
               graph, 1u,
               TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    assert(turbowasm_component_type_graph_define_function(
               graph, 2u,
               NULL, 0u,
               true,
               turbowasm_component_type_ref_indexed(1u)));
    assert(turbowasm_component_type_graph_validate(graph));
}

static uint32_t invoke_stdin_component_import(
    turbowasm_wasi02_streams *streams,
    turbowasm_component_exec_imports *imports,
    turbowasm_component_type_graph *graph) {
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    assert(imports->can_bind(
        imports->context,
        cname("wasi:cli/stdin@0.2.8"),
        cname("get-stdin"),
        graph,
        2u));

    status = imports->invoke(
        imports->context,
        NULL,
        cname("wasi:cli/stdin@0.2.8"),
        cname("get-stdin"),
        graph,
        2u,
        NULL,
        0u,
        &result,
        &trap);
    assert(status == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(result.as.resource_rep.kind == TURBOWASM_VALUE_I32);
    assert(streams->component_input_identity_bound);
    assert(streams->component_input_identity == UINT64_C(0x7111));

    return (uint32_t)result.as.resource_rep.as.i32;
}

static void test_stdio_component_producer_and_stream_use(void) {
    stdio_probe probe = {0};
    turbowasm_wasi02_stream_provider provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_component_exec_imports imports = {0};
    turbowasm_component_type_graph graph = {0};
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value read_result = {0};
    uint32_t resource;

    provider.context = &probe;
    provider.get_stdin = make_stdin;
    provider.get_stdout = make_stdout;
    provider.get_stderr = make_stderr;
    provider.input_read = read_stdin;
    provider.input_drop = drop_input;
    provider.output_drop = drop_output;

    assert(turbowasm_wasi02_streams_init(
               &streams, &provider, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_imports(
               &streams, &imports) == TURBOWASM_OK);

    build_stdin_graph(&graph);
    resource = invoke_stdin_component_import(
        &streams, &imports, &graph);
    assert(probe.stdin_calls == 1u);

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = resource;
    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 2u;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.read",
               args, 2u,
               &read_result) == TURBOWASM_OK);
    assert(read_result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!read_result.as.result.is_error);
    assert(read_result.as.result.value != NULL);
    assert(read_result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_LIST);
    assert(read_result.as.result.value->as.list.count == 2u);
    assert(read_result.as.result.value->as.list.items[0].as.u8 ==
           (uint8_t)'o');
    assert(read_result.as.result.value->as.list.items[1].as.u8 ==
           (uint8_t)'k');
    assert(probe.input_reads == 1u);
    turbowasm_wasi02_value_destroy(&read_result);

    assert(imports.resource_drop(
               imports.context,
               UINT64_C(0x7111),
               resource) == TURBOWASM_OK);
    assert(probe.input_drops == 1u);

    turbowasm_component_type_graph_destroy(&graph);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
}

static void test_factory_requires_drop_contract(void) {
    stdio_probe probe = {0};
    turbowasm_wasi02_stream_provider provider = {0};
    turbowasm_wasi02_streams streams = {0};

    provider.context = &probe;
    provider.get_stdin = make_stdin;
    assert(turbowasm_wasi02_streams_init(
               &streams, &provider, 2u) ==
           TURBOWASM_INVALID_ARGUMENT);

    memset(&provider, 0, sizeof(provider));
    provider.context = &probe;
    provider.get_stdout = make_stdout;
    assert(turbowasm_wasi02_streams_init(
               &streams, &provider, 2u) ==
           TURBOWASM_INVALID_ARGUMENT);
}

int main(void) {
    test_stdio_component_producer_and_stream_use();
    test_factory_requires_drop_contract();
    return 0;
}
