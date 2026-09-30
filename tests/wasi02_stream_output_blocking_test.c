#include "../src/wasi02_streams.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER 0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef enum output_operation {
    OUTPUT_OPERATION_WRITE = 0,
    OUTPUT_OPERATION_ZEROES,
    OUTPUT_OPERATION_FLUSH
} output_operation;

typedef struct output_probe {
    bool ready;
    output_operation operation;

    uint8_t written[8];
    size_t written_size;
    uint64_t zeroes;

    uint32_t check_calls;
    uint32_t write_calls;
    uint32_t zero_calls;
    uint32_t flush_calls;
    uint32_t subscribe_calls;
    uint32_t arm_calls;
    uint32_t poll_drop_calls;
    uint32_t output_drop_calls;
    uint32_t host_entries;

    uintptr_t operation_token_base;

    turbowasm_wasi02_streams streams;
    turbowasm_wasi02_poll poll;
    uint32_t output_resource;
} output_probe;

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status probe_check_write(
    void *context,
    turbowasm_value stream_rep,
    uint64_t *out_permit,
    turbowasm_wasi02_stream_error *out_error) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_permit != NULL);
    assert(out_error != NULL);
    ++probe->check_calls;

    *out_permit = probe->ready ? 4096u : 0u;
    return TURBOWASM_OK;
}

static turbowasm_status probe_write(
    void *context,
    turbowasm_value stream_rep,
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_stream_error *out_error) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    assert(size <= sizeof(probe->written));
    assert(size == 0u || data != NULL);
    assert(probe->ready);

    ++probe->write_calls;
    probe->written_size = size;
    if (size != 0u)
        memcpy(probe->written, data, size);
    return TURBOWASM_OK;
}

static turbowasm_status probe_write_zeroes(
    void *context,
    turbowasm_value stream_rep,
    uint64_t size,
    turbowasm_wasi02_stream_error *out_error) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    assert(probe->ready);

    ++probe->zero_calls;
    probe->zeroes += size;
    return TURBOWASM_OK;
}

static turbowasm_status probe_flush(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_wasi02_stream_error *out_error) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    ++probe->flush_calls;

    /*
     * Model an asynchronous flush: once requested, readiness drops until the
     * Runtime host-wait completion marks the pollable ready again.
     */
    probe->ready = false;
    return TURBOWASM_OK;
}

static turbowasm_status probe_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_pollable_rep != NULL);
    ++probe->subscribe_calls;

    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 88;
    return TURBOWASM_OK;
}

static void probe_output_drop(
    void *context,
    turbowasm_value rep) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 22);
    ++probe->output_drop_calls;
}

static turbowasm_status probe_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 88);
    assert(out_ready != NULL);
    *out_ready = probe->ready;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_arm(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 88);
    assert(out_operation_token != NULL);
    ++probe->arm_calls;
    *out_operation_token =
        probe->operation_token_base + probe->arm_calls;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_drop(
    void *context,
    turbowasm_value rep) {
    output_probe *probe = (output_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 88);
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status host_blocking_output(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    output_probe *probe = (output_probe *)context;
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value bytes[2] = {{0}};
    turbowasm_wasi02_value result = {0};
    const char *function_name;
    size_t wasi_argument_count;
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
    args[0].as.resource = probe->output_resource;

    if (probe->operation == OUTPUT_OPERATION_WRITE) {
        bytes[0].kind = TURBOWASM_WASI02_VALUE_U8;
        bytes[0].as.u8 = (uint8_t)'O';
        bytes[1].kind = TURBOWASM_WASI02_VALUE_U8;
        bytes[1].as.u8 = (uint8_t)'K';
        args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
        args[1].as.list.items = bytes;
        args[1].as.list.count = 2u;
        function_name =
            "[method]output-stream.blocking-write-and-flush";
        wasi_argument_count = 2u;
    } else if (probe->operation == OUTPUT_OPERATION_ZEROES) {
        args[1].kind = TURBOWASM_WASI02_VALUE_U64;
        args[1].as.u64 = 3u;
        function_name =
            "[method]output-stream.blocking-write-zeroes-and-flush";
        wasi_argument_count = 2u;
    } else {
        function_name =
            "[method]output-stream.blocking-flush";
        wasi_argument_count = 1u;
    }

    status = turbowasm_wasi02_streams_call_with_host(
        &probe->streams,
        call,
        "streams",
        function_name,
        args,
        wasi_argument_count,
        &result);
    if (status != TURBOWASM_OK)
        return status;

    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);

    results[0].kind = TURBOWASM_VALUE_I32;
    if (probe->operation == OUTPUT_OPERATION_WRITE) {
        results[0].as.i32 = (int32_t)probe->written_size;
    } else if (probe->operation == OUTPUT_OPERATION_ZEROES) {
        results[0].as.i32 = (int32_t)probe->zeroes;
    } else {
        results[0].as.i32 = (int32_t)probe->flush_calls;
    }
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

    /* import host.output type0 */
    0x02,0x0f,
    0x01,
    0x04,'h','o','s','t',
    0x06,'o','u','t','p','u','t',
    0x00,0x00
};

static void init_probe(
    output_probe *probe,
    output_operation operation) {
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_value rep = {0};

    memset(probe, 0, sizeof(*probe));
    probe->operation = operation;
    probe->operation_token_base = (uintptr_t)0x9900u;

    stream_provider.context = probe;
    stream_provider.output_check_write = probe_check_write;
    stream_provider.output_write = probe_write;
    stream_provider.output_flush = probe_flush;
    stream_provider.output_write_zeroes = probe_write_zeroes;
    stream_provider.output_subscribe = probe_subscribe;
    stream_provider.output_drop = probe_output_drop;

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
    rep.as.i64 = 22;
    assert(turbowasm_wasi02_output_stream_new(
               &probe->streams,
               rep,
               &probe->output_resource) == TURBOWASM_OK);
}

static void destroy_probe(output_probe *probe) {
    assert(turbowasm_wasi02_stream_resource_drop(
               &probe->streams,
               probe->output_resource) == TURBOWASM_OK);
    assert(probe->output_drop_calls == 1u);
    assert(turbowasm_wasi02_streams_destroy(
               &probe->streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &probe->poll) == TURBOWASM_OK);
}

static void test_one_shot_fail_fast_and_limit(void) {
    output_probe probe;
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value bytes[2] = {{0}};
    turbowasm_wasi02_value result = {0};

    init_probe(&probe, OUTPUT_OPERATION_WRITE);

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = probe.output_resource;
    bytes[0].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[0].as.u8 = (uint8_t)'O';
    bytes[1].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[1].as.u8 = (uint8_t)'K';
    args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
    args[1].as.list.items = bytes;
    args[1].as.list.count = 2u;

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.blocking-write-and-flush",
               args, 2u, &result) == TURBOWASM_UNSUPPORTED);
    assert(probe.check_calls == 0u);
    assert(probe.write_calls == 0u);
    assert(probe.flush_calls == 0u);
    assert(probe.subscribe_calls == 0u);

    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 4097u;
    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.blocking-write-zeroes-and-flush",
               args, 2u, &result) == TURBOWASM_TRAPPED);
    assert(probe.zero_calls == 0u);

    assert(turbowasm_wasi02_streams_call(
               &probe.streams,
               "streams",
               "[method]output-stream.blocking-flush",
               args, 1u, &result) == TURBOWASM_UNSUPPORTED);
    assert(probe.flush_calls == 0u);

    destroy_probe(&probe);
}

static void run_restartable_case(
    output_operation operation,
    uint32_t expected_waits,
    int32_t expected_result) {
    output_probe probe;
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    const turbowasm_value *result;
    uint32_t completed_waits = 0u;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };

    init_probe(&probe, operation);

    assert(turbowasm_module_load_borrowed(
               &module,
               host_module,
               sizeof(host_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(
               &linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("output", 6u),
               &host_type,
               host_blocking_output,
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

    for (;;) {
        turbowasm_status status =
            turbowasm_execution_resume(&execution, NULL);
        if (status == TURBOWASM_OK)
            break;

        assert(status == TURBOWASM_YIELDED);
        assert(turbowasm_execution_yield_reason_get(
                   &execution) == TURBOWASM_YIELD_HOST_WAIT);
        assert(probe.host_entries == 1u);
        assert(turbowasm_execution_pending_host_wait(
                   &execution, &wait));

        ++completed_waits;
        assert(completed_waits <= expected_waits);
        probe.ready = true;
        assert(turbowasm_execution_complete_host_wait(
                   &execution,
                   wait,
                   0) == TURBOWASM_OK);
    }

    assert(completed_waits == expected_waits);
    assert(probe.host_entries == 1u);
    assert(probe.subscribe_calls == expected_waits);
    assert(probe.arm_calls == expected_waits);
    assert(probe.poll_drop_calls == expected_waits);

    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == expected_result);

    if (operation == OUTPUT_OPERATION_WRITE) {
        assert(probe.write_calls == 1u);
        assert(probe.written_size == 2u);
        assert(probe.written[0] == (uint8_t)'O');
        assert(probe.written[1] == (uint8_t)'K');
        assert(probe.flush_calls == 1u);
        assert(probe.check_calls == 4u);
    } else if (operation == OUTPUT_OPERATION_ZEROES) {
        assert(probe.zero_calls == 1u);
        assert(probe.zeroes == 3u);
        assert(probe.flush_calls == 1u);
        assert(probe.check_calls == 4u);
    } else {
        assert(probe.write_calls == 0u);
        assert(probe.zero_calls == 0u);
        assert(probe.flush_calls == 1u);
        assert(probe.check_calls == 2u);
    }

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    destroy_probe(&probe);
}

int main(void) {
    test_one_shot_fail_fast_and_limit();
    run_restartable_case(
        OUTPUT_OPERATION_WRITE, 2u, 2);
    run_restartable_case(
        OUTPUT_OPERATION_ZEROES, 2u, 3);
    run_restartable_case(
        OUTPUT_OPERATION_FLUSH, 1u, 1);
    return 0;
}
