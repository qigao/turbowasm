#include "../src/wasi02_streams.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER     0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef struct blocking_probe {
    bool ready;
    uint8_t bytes[2];

    uint32_t read_calls;
    uint32_t subscribe_calls;
    uint32_t arm_calls;
    uint32_t poll_drop_calls;
    uint32_t input_drop_calls;
    uint32_t host_entries;

    uintptr_t operation_token;

    turbowasm_wasi02_streams streams;
    turbowasm_wasi02_poll poll;
    uint32_t input_resource;
} blocking_probe;

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status probe_input_read(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    blocking_probe *probe = (blocking_probe *)context;
    size_t size;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_data != NULL);
    assert(out_size != NULL);
    assert(out_error != NULL);
    ++probe->read_calls;

    if (!probe->ready) {
        *out_data = NULL;
        *out_size = 0u;
        return TURBOWASM_OK;
    }

    size = max_bytes < 2u ? (size_t)max_bytes : 2u;
    *out_data = probe->bytes;
    *out_size = size;
    return TURBOWASM_OK;
}

static turbowasm_status probe_input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    blocking_probe *probe = (blocking_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_pollable_rep != NULL);
    ++probe->subscribe_calls;

    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 77;
    return TURBOWASM_OK;
}

static void probe_input_drop(
    void *context,
    turbowasm_value rep) {
    blocking_probe *probe = (blocking_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static turbowasm_status probe_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    blocking_probe *probe = (blocking_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    assert(out_ready != NULL);
    *out_ready = probe->ready;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_arm(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token) {
    blocking_probe *probe = (blocking_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    assert(out_operation_token != NULL);
    ++probe->arm_calls;
    *out_operation_token = probe->operation_token;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_drop(
    void *context,
    turbowasm_value rep) {
    blocking_probe *probe = (blocking_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status host_blocking_read(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    blocking_probe *probe = (blocking_probe *)context;
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value result = {0};
    turbowasm_status status;

    assert(probe != NULL);
    assert(call != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++probe->host_entries;

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = probe->input_resource;
    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 2u;

    status = turbowasm_wasi02_streams_call_with_host(
        &probe->streams,
        call,
        "streams",
        "[method]input-stream.blocking-read",
        args,
        2u,
        &result);
    if (status != TURBOWASM_OK)
        return status;

    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.result.value->as.list.count == 2u);
    assert(result.as.result.value->as.list.items[0].as.u8 ==
           (uint8_t)'O');
    assert(result.as.result.value->as.list.items[1].as.u8 ==
           (uint8_t)'K');

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 =
        (int32_t)result.as.result.value->as.list.count;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;

    turbowasm_wasi02_value_destroy(&result);
    return TURBOWASM_OK;
}

static const uint8_t host_module[] = {
    WASM_HEADER,

    /* type0: () -> i32 */
    0x01,0x05,
    0x01,0x60,0x00,0x01,0x7f,

    /* import host.read type0 */
    0x02,0x0d,
    0x01,
    0x04,'h','o','s','t',
    0x04,'r','e','a','d',
    0x00,0x00
};

static void init_probe(
    blocking_probe *probe) {
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_value rep = {0};

    memset(probe, 0, sizeof(*probe));
    memcpy(probe->bytes, "OK", 2u);
    probe->operation_token = (uintptr_t)0x9876u;

    stream_provider.context = probe;
    stream_provider.input_read = probe_input_read;
    stream_provider.input_subscribe = probe_input_subscribe;
    stream_provider.input_drop = probe_input_drop;

    poll_provider.context = probe;
    poll_provider.ready = probe_poll_ready;
    poll_provider.arm = probe_poll_arm;
    poll_provider.drop = probe_poll_drop;

    assert(turbowasm_wasi02_streams_init(
               &probe->streams,
               &stream_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &probe->poll,
               &poll_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &probe->streams,
               &probe->poll) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &probe->streams,
               rep,
               &probe->input_resource) == TURBOWASM_OK);
}

static void destroy_probe(
    blocking_probe *probe) {
    assert(turbowasm_wasi02_stream_resource_drop(
               &probe->streams,
               probe->input_resource) == TURBOWASM_OK);
    assert(probe->input_drop_calls == 1u);
    assert(turbowasm_wasi02_streams_destroy(
               &probe->streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &probe->poll) == TURBOWASM_OK);
}

static void test_one_shot_fail_fast_and_zero_len(void) {
    blocking_probe probe;
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value result = {0};

    init_probe(&probe);

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = probe.input_resource;
    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 2u;

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]input-stream.blocking-read",
               args, 2u, &result) == TURBOWASM_UNSUPPORTED);
    assert(probe.read_calls == 1u);
    assert(probe.subscribe_calls == 0u);
    assert(probe.arm_calls == 0u);

    args[1].as.u64 = 0u;
    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]input-stream.blocking-read",
               args, 2u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.result.value->as.list.count == 0u);
    assert(probe.subscribe_calls == 0u);
    turbowasm_wasi02_value_destroy(&result);

    destroy_probe(&probe);
}

static void test_restartable_blocking_read(void) {
    blocking_probe probe;
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    const turbowasm_value *result;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };

    init_probe(&probe);

    assert(turbowasm_module_load_borrowed(
               &module,
               host_module,
               sizeof(host_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(
               &linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("read", 4u),
               &host_type,
               host_blocking_read,
               &probe) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    assert(turbowasm_execution_create(
               &execution,
               &instance,
               0u,
               NULL, 0u) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.host_entries == 1u);
    assert(probe.read_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 0u);

    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.operation_token == probe.operation_token);

    probe.ready = true;
    assert(turbowasm_execution_complete_host_wait(
               &execution,
               wait,
               0) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);

    /* Retained callback frame resumes without replay. */
    assert(probe.host_entries == 1u);
    assert(probe.read_calls == 2u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 1u);

    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 2);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    destroy_probe(&probe);
}

int main(void) {
    test_one_shot_fail_fast_and_zero_len();
    test_restartable_blocking_read();
    return 0;
}
