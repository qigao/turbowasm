#include "../src/wasi02_streams.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER 0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef struct splice_probe {
    bool input_ready;
    bool output_ready;
    bool input_closed;

    uint8_t input[4];
    size_t input_size;
    uint8_t written[8];
    size_t written_size;
    uint64_t output_permit;
    uint64_t last_read_limit;

    uint32_t check_calls;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t input_subscribe_calls;
    uint32_t output_subscribe_calls;
    uint32_t arm_calls;
    uint32_t input_poll_drop_calls;
    uint32_t output_poll_drop_calls;
    uint32_t input_drop_calls;
    uint32_t output_drop_calls;
    uint32_t host_entries;

    uintptr_t token_base;

    turbowasm_wasi02_streams streams;
    turbowasm_wasi02_poll poll;
    uint32_t input_resource;
    uint32_t output_resource;
} splice_probe;

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status probe_read(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    splice_probe *probe = (splice_probe *)context;
    size_t size;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_data != NULL);
    assert(out_size != NULL);
    assert(out_error != NULL);

    ++probe->read_calls;
    probe->last_read_limit = max_bytes;

    if (probe->input_closed) {
        out_error->kind = TURBOWASM_WASI02_STREAM_ERROR_CLOSED;
        return TURBOWASM_OK;
    }

    if (max_bytes == 0u || !probe->input_ready) {
        *out_data = NULL;
        *out_size = 0u;
        return TURBOWASM_OK;
    }

    size = (uint64_t)probe->input_size < max_bytes
        ? probe->input_size
        : (size_t)max_bytes;
    *out_data = probe->input;
    *out_size = size;
    return TURBOWASM_OK;
}

static turbowasm_status probe_check_write(
    void *context,
    turbowasm_value stream_rep,
    uint64_t *out_permit,
    turbowasm_wasi02_stream_error *out_error) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_permit != NULL);
    assert(out_error != NULL);

    ++probe->check_calls;
    *out_permit = probe->output_ready
        ? probe->output_permit
        : 0u;
    return TURBOWASM_OK;
}

static turbowasm_status probe_write(
    void *context,
    turbowasm_value stream_rep,
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_stream_error *out_error) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    assert(size <= sizeof(probe->written));
    assert(size == 0u || data != NULL);

    ++probe->write_calls;
    probe->written_size = size;
    if (size != 0u)
        memcpy(probe->written, data, size);
    return TURBOWASM_OK;
}

static turbowasm_status probe_input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_pollable_rep != NULL);
    ++probe->input_subscribe_calls;

    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 77;
    return TURBOWASM_OK;
}

static turbowasm_status probe_output_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_pollable_rep != NULL);
    ++probe->output_subscribe_calls;

    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 88;
    return TURBOWASM_OK;
}

static void probe_input_drop(
    void *context,
    turbowasm_value rep) {
    splice_probe *probe = (splice_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static void probe_output_drop(
    void *context,
    turbowasm_value rep) {
    splice_probe *probe = (splice_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 22);
    ++probe->output_drop_calls;
}

static turbowasm_status probe_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(out_ready != NULL);

    if (rep.as.i64 == 77) {
        *out_ready = probe->input_ready;
    } else {
        assert(rep.as.i64 == 88);
        *out_ready = probe->output_ready;
    }
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_arm(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77 || rep.as.i64 == 88);
    assert(out_operation_token != NULL);

    ++probe->arm_calls;
    *out_operation_token =
        probe->token_base + probe->arm_calls;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_drop(
    void *context,
    turbowasm_value rep) {
    splice_probe *probe = (splice_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    if (rep.as.i64 == 77) {
        ++probe->input_poll_drop_calls;
    } else {
        assert(rep.as.i64 == 88);
        ++probe->output_poll_drop_calls;
    }
    return TURBOWASM_OK;
}

static void set_resource(
    turbowasm_wasi02_value *value,
    uint32_t resource) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    value->as.resource = resource;
}

static void set_u64(
    turbowasm_wasi02_value *value,
    uint64_t number) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_U64;
    value->as.u64 = number;
}

static uint64_t result_u64(
    const turbowasm_wasi02_value *result) {
    assert(result != NULL);
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_U64);
    return result->as.result.value->as.u64;
}

static void init_probe(splice_probe *probe) {
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_value rep = {0};

    memset(probe, 0, sizeof(*probe));
    memcpy(probe->input, "ABC", 3u);
    probe->input_size = 3u;
    probe->output_permit = 2u;
    probe->token_base = (uintptr_t)0xa100u;

    stream_provider.context = probe;
    stream_provider.input_read = probe_read;
    stream_provider.input_subscribe = probe_input_subscribe;
    stream_provider.output_check_write = probe_check_write;
    stream_provider.output_write = probe_write;
    stream_provider.output_subscribe = probe_output_subscribe;
    stream_provider.input_drop = probe_input_drop;
    stream_provider.output_drop = probe_output_drop;

    poll_provider.context = probe;
    poll_provider.ready = probe_poll_ready;
    poll_provider.arm = probe_poll_arm;
    poll_provider.drop = probe_poll_drop;

    assert(turbowasm_wasi02_streams_init(
               &probe->streams,
               &stream_provider,
               8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &probe->poll,
               &poll_provider,
               8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &probe->streams,
               &probe->poll) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &probe->streams,
               rep,
               &probe->input_resource) == TURBOWASM_OK);

    rep.as.i64 = 22;
    assert(turbowasm_wasi02_output_stream_new(
               &probe->streams,
               rep,
               &probe->output_resource) == TURBOWASM_OK);
}

static void destroy_probe(splice_probe *probe) {
    assert(turbowasm_wasi02_stream_resource_drop(
               &probe->streams,
               probe->input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &probe->streams,
               probe->output_resource) == TURBOWASM_OK);
    assert(probe->input_drop_calls == 1u);
    assert(probe->output_drop_calls == 1u);
    assert(turbowasm_wasi02_streams_destroy(
               &probe->streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &probe->poll) == TURBOWASM_OK);
}

static void test_nonblocking_splice_contract(void) {
    splice_probe probe;
    turbowasm_wasi02_value args[3] = {{0}};
    turbowasm_wasi02_value result = {0};

    init_probe(&probe);
    probe.input_ready = true;
    probe.output_ready = true;

    set_resource(&args[0], probe.output_resource);
    set_resource(&args[1], probe.input_resource);
    set_u64(&args[2], 4u);

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.splice",
               args, 3u, &result) == TURBOWASM_OK);
    assert(result_u64(&result) == 2u);
    assert(probe.check_calls == 1u);
    assert(probe.read_calls == 1u);
    assert(probe.last_read_limit == 2u);
    assert(probe.write_calls == 1u);
    assert(probe.written_size == 2u);
    assert(probe.written[0] == (uint8_t)'A');
    assert(probe.written[1] == (uint8_t)'B');
    turbowasm_wasi02_value_destroy(&result);

    /*
     * With no output permit, no transfer is possible: return zero without
     * touching the input or calling write, and consume the hidden permit.
     */
    probe.output_ready = false;
    probe.written_size = 99u;
    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.splice",
               args, 3u, &result) == TURBOWASM_OK);
    assert(result_u64(&result) == 0u);
    assert(probe.check_calls == 2u);
    assert(probe.read_calls == 1u);
    assert(probe.write_calls == 1u);
    assert(probe.written_size == 99u);
    turbowasm_wasi02_value_destroy(&result);

    destroy_probe(&probe);
}

static void test_input_error_consumes_hidden_permit(void) {
    splice_probe probe;
    turbowasm_wasi02_value splice_args[3] = {{0}};
    turbowasm_wasi02_value write_args[2] = {{0}};
    turbowasm_wasi02_value byte = {0};
    turbowasm_wasi02_value result = {0};

    init_probe(&probe);
    probe.input_ready = true;
    probe.output_ready = true;
    probe.output_permit = 4u;
    probe.input_closed = true;

    set_resource(&splice_args[0], probe.output_resource);
    set_resource(&splice_args[1], probe.input_resource);
    set_u64(&splice_args[2], 4u);

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.splice",
               splice_args, 3u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(result.as.result.is_error);
    assert(probe.check_calls == 1u);
    assert(probe.read_calls == 1u);
    assert(probe.write_calls == 0u);
    turbowasm_wasi02_value_destroy(&result);

    /*
     * The check-write permit obtained before the input error is internal to
     * splice and must not be usable by the next external write.
     */
    set_resource(&write_args[0], probe.output_resource);
    byte.kind = TURBOWASM_WASI02_VALUE_U8;
    byte.as.u8 = (uint8_t)'X';
    write_args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
    write_args[1].as.list.items = &byte;
    write_args[1].as.list.count = 1u;
    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.write",
               write_args, 2u, &result) == TURBOWASM_TRAPPED);
    assert(probe.write_calls == 0u);

    destroy_probe(&probe);
}

static turbowasm_status host_blocking_splice(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    splice_probe *probe = (splice_probe *)context;
    turbowasm_wasi02_value args[3] = {{0}};
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

    set_resource(&args[0], probe->output_resource);
    set_resource(&args[1], probe->input_resource);
    set_u64(&args[2], 4u);

    status = turbowasm_wasi02_streams_call_with_host(
        &probe->streams,
        call,
        "streams",
        "[method]output-stream.blocking-splice",
        args,
        3u,
        &result);
    if (status != TURBOWASM_OK)
        return status;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = (int32_t)result_u64(&result);
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

    /* import host.splice type0 */
    0x02,0x0f,
    0x01,
    0x04,'h','o','s','t',
    0x06,'s','p','l','i','c','e',
    0x00,0x00
};

static void test_blocking_splice_fail_fast(void) {
    splice_probe probe;
    turbowasm_wasi02_value args[3] = {{0}};
    turbowasm_wasi02_value result = {0};

    init_probe(&probe);
    set_resource(&args[0], probe.output_resource);
    set_resource(&args[1], probe.input_resource);
    set_u64(&args[2], 4u);

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.blocking-splice",
               args, 3u, &result) == TURBOWASM_UNSUPPORTED);
    assert(probe.output_subscribe_calls == 0u);
    assert(probe.input_subscribe_calls == 0u);
    assert(probe.check_calls == 0u);
    assert(probe.read_calls == 0u);
    assert(probe.write_calls == 0u);

    destroy_probe(&probe);
}

static void test_restartable_blocking_splice(void) {
    splice_probe probe;
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
    probe.output_permit = 4u;

    assert(turbowasm_module_load_borrowed(
               &module,
               host_module,
               sizeof(host_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(
               &linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("splice", 6u),
               &host_type,
               host_blocking_splice,
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

    /* First wait: output readiness. */
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.host_entries == 1u);
    assert(probe.check_calls == 1u);
    assert(probe.output_subscribe_calls == 1u);
    assert(probe.input_subscribe_calls == 0u);
    assert(probe.arm_calls == 1u);
    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));

    probe.output_ready = true;
    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 0) == TURBOWASM_OK);

    /* Second wait: input readiness, still inside the same host callback. */
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.host_entries == 1u);
    assert(probe.output_poll_drop_calls == 1u);
    assert(probe.check_calls == 2u);
    assert(probe.read_calls == 1u);
    assert(probe.input_subscribe_calls == 1u);
    assert(probe.arm_calls == 2u);
    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));

    probe.input_ready = true;
    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 0) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);

    assert(probe.host_entries == 1u);
    assert(probe.input_poll_drop_calls == 1u);
    assert(probe.output_poll_drop_calls == 1u);
    assert(probe.check_calls == 2u);
    assert(probe.read_calls == 2u);
    assert(probe.write_calls == 1u);
    assert(probe.written_size == 3u);
    assert(memcmp(probe.written, "ABC", 3u) == 0);

    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 3);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    destroy_probe(&probe);
}

int main(void) {
    test_nonblocking_splice_contract();
    test_input_error_consumes_hidden_permit();
    test_blocking_splice_fail_fast();
    test_restartable_blocking_splice();
    return 0;
}
