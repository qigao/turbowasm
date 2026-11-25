#include "../src/wasi02_streams.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct stream_probe {
    uint8_t input_bytes[4];
    uint8_t written[8];
    size_t written_size;

    uint64_t check_permit;
    uint64_t skipped;
    uint64_t zeroes;

    uint32_t read_calls;
    uint32_t skip_calls;
    uint32_t check_calls;
    uint32_t write_calls;
    uint32_t flush_calls;
    uint32_t zero_calls;
    uint32_t input_drop_calls;
    uint32_t output_drop_calls;
    uint32_t error_drop_calls;
    uint32_t debug_calls;
    uint32_t input_subscribe_calls;
    uint32_t output_subscribe_calls;

    bool read_error;
    bool skip_closed;
} stream_probe;

static turbowasm_status probe_read(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;
    size_t size;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_data != NULL);
    assert(out_size != NULL);
    assert(out_error != NULL);
    ++probe->read_calls;

    if (probe->read_error) {
        out_error->kind =
            TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED;
        out_error->error_rep.kind = TURBOWASM_VALUE_I64;
        out_error->error_rep.as.i64 = 99;
        return TURBOWASM_OK;
    }

    size = max_bytes < 4u ? (size_t)max_bytes : 4u;
    *out_data = probe->input_bytes;
    *out_size = size;
    return TURBOWASM_OK;
}

static turbowasm_status probe_skip(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    uint64_t *out_skipped,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_skipped != NULL);
    assert(out_error != NULL);
    ++probe->skip_calls;

    if (probe->skip_closed) {
        out_error->kind = TURBOWASM_WASI02_STREAM_ERROR_CLOSED;
        return TURBOWASM_OK;
    }

    *out_skipped = max_bytes < probe->skipped
        ? max_bytes
        : probe->skipped;
    return TURBOWASM_OK;
}

static turbowasm_status probe_check_write(
    void *context,
    turbowasm_value stream_rep,
    uint64_t *out_permit,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_permit != NULL);
    assert(out_error != NULL);
    ++probe->check_calls;
    *out_permit = probe->check_permit;
    return TURBOWASM_OK;
}

static turbowasm_status probe_write(
    void *context,
    turbowasm_value stream_rep,
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;

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

static turbowasm_status probe_flush(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    ++probe->flush_calls;
    return TURBOWASM_OK;
}

static turbowasm_status probe_write_zeroes(
    void *context,
    turbowasm_value stream_rep,
    uint64_t size,
    turbowasm_wasi02_stream_error *out_error) {
    stream_probe *probe = (stream_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_error != NULL);
    ++probe->zero_calls;
    probe->zeroes = size;
    return TURBOWASM_OK;
}

static turbowasm_status probe_error_debug(
    void *context,
    turbowasm_value error_rep,
    turbowasm_wasi02_string_view *out_debug) {
    static const uint8_t text[] = "stream-failed";
    stream_probe *probe = (stream_probe *)context;

    assert(probe != NULL);
    assert(error_rep.kind == TURBOWASM_VALUE_I64);
    assert(error_rep.as.i64 == 99);
    assert(out_debug != NULL);
    ++probe->debug_calls;
    out_debug->data = text;
    out_debug->size = sizeof(text) - 1u;
    return TURBOWASM_OK;
}

static void probe_input_drop(
    void *context,
    turbowasm_value rep) {
    stream_probe *probe = (stream_probe *)context;
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static void probe_output_drop(
    void *context,
    turbowasm_value rep) {
    stream_probe *probe = (stream_probe *)context;
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 22);
    ++probe->output_drop_calls;
}

static void probe_error_drop(
    void *context,
    turbowasm_value rep) {
    stream_probe *probe = (stream_probe *)context;
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 99);
    ++probe->error_drop_calls;
}

static turbowasm_status probe_input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    stream_probe *probe = (stream_probe *)context;
    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_pollable_rep != NULL);
    ++probe->input_subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 101;
    return TURBOWASM_OK;
}

static turbowasm_status probe_output_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    stream_probe *probe = (stream_probe *)context;
    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 22);
    assert(out_pollable_rep != NULL);
    ++probe->output_subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 202;
    return TURBOWASM_OK;
}

static turbowasm_wasi02_stream_provider make_provider(
    stream_probe *probe) {
    turbowasm_wasi02_stream_provider provider = {0};
    provider.context = probe;
    provider.input_read = probe_read;
    provider.input_skip = probe_skip;
    provider.input_subscribe = probe_input_subscribe;
    provider.output_check_write = probe_check_write;
    provider.output_write = probe_write;
    provider.output_flush = probe_flush;
    provider.output_write_zeroes = probe_write_zeroes;
    provider.output_subscribe = probe_output_subscribe;
    provider.error_debug = probe_error_debug;
    provider.input_drop = probe_input_drop;
    provider.output_drop = probe_output_drop;
    provider.error_drop = probe_error_drop;
    return provider;
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

static void test_nonblocking_stream_contract(void) {
    stream_probe probe = {0};
    turbowasm_wasi02_stream_provider provider;
    turbowasm_wasi02_streams streams = {0};
    turbowasm_value rep = {0};
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value bytes[3] = {{0}};
    turbowasm_wasi02_value result = {0};
    uint32_t input = 0u;
    uint32_t output = 0u;
    uint32_t error_resource = 0u;

    memcpy(probe.input_bytes, "abcd", 4u);
    probe.skipped = 3u;
    probe.check_permit = 3u;
    provider = make_provider(&probe);

    assert(turbowasm_wasi02_streams_init(
               &streams, &provider, 8u) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &streams, rep, &input) == TURBOWASM_OK);
    rep.as.i64 = 22;
    assert(turbowasm_wasi02_output_stream_new(
               &streams, rep, &output) == TURBOWASM_OK);

    set_resource(&args[0], input);
    set_u64(&args[1], 3u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.read",
               args, 2u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.result.value->as.list.count == 3u);
    assert(result.as.result.value->as.list.items[0].as.u8 == 'a');
    assert(result.as.result.value->as.list.items[2].as.u8 == 'c');

    /* Provider memory is borrowed; returned WIT data is deep-copied. */
    probe.input_bytes[0] = 'X';
    assert(result.as.result.value->as.list.items[0].as.u8 == 'a');
    turbowasm_wasi02_value_destroy(&result);

    set_u64(&args[1], 2u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.skip",
               args, 2u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u64 == 2u);
    turbowasm_wasi02_value_destroy(&result);

    probe.skip_closed = true;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.skip",
               args, 2u, &result) == TURBOWASM_OK);
    assert(result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_VARIANT);
    assert(result.as.result.value->as.variant.case_index == 1u);
    assert(result.as.result.value->as.variant.value == NULL);
    turbowasm_wasi02_value_destroy(&result);
    probe.skip_closed = false;

    /* write without a preceding check-write traps. */
    set_resource(&args[0], output);
    bytes[0].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[0].as.u8 = 'x';
    args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
    args[1].as.list.items = bytes;
    args[1].as.list.count = 1u;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write",
               args, 2u, &result) == TURBOWASM_TRAPPED);
    assert(probe.write_calls == 0u);

    set_resource(&args[0], output);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.check-write",
               args, 1u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(result.as.result.value->as.u64 == 3u);
    turbowasm_wasi02_value_destroy(&result);

    bytes[0].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[0].as.u8 = 'x';
    bytes[1].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[1].as.u8 = 'y';
    bytes[2].kind = TURBOWASM_WASI02_VALUE_U8;
    bytes[2].as.u8 = 'z';
    args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
    args[1].as.list.items = bytes;
    args[1].as.list.count = 3u;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write",
               args, 2u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(probe.write_calls == 1u);
    assert(probe.written_size == 3u);
    assert(memcmp(probe.written, "xyz", 3u) == 0);
    turbowasm_wasi02_value_destroy(&result);

    /* permit was consumed by the write. */
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write",
               args, 2u, &result) == TURBOWASM_TRAPPED);

    /* check-write then flush invalidates that permit. */
    set_resource(&args[0], output);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.check-write",
               args, 1u, &result) == TURBOWASM_OK);
    turbowasm_wasi02_value_destroy(&result);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.flush",
               args, 1u, &result) == TURBOWASM_OK);
    assert(probe.flush_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    set_u64(&args[1], 1u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write-zeroes",
               args, 2u, &result) == TURBOWASM_TRAPPED);

    set_resource(&args[0], output);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.check-write",
               args, 1u, &result) == TURBOWASM_OK);
    turbowasm_wasi02_value_destroy(&result);
    set_u64(&args[1], 2u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write-zeroes",
               args, 2u, &result) == TURBOWASM_OK);
    assert(probe.zero_calls == 1u);
    assert(probe.zeroes == 2u);
    turbowasm_wasi02_value_destroy(&result);

    /* last-operation-failed creates a real owned wasi:io/error handle. */
    probe.read_error = true;
    set_resource(&args[0], input);
    set_u64(&args[1], 1u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.read",
               args, 2u, &result) == TURBOWASM_OK);
    assert(result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_VARIANT);
    assert(result.as.result.value->as.variant.case_index == 0u);
    assert(result.as.result.value->as.variant.value != NULL);
    assert(result.as.result.value->as.variant.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    error_resource =
        result.as.result.value->as.variant.value->as.resource;
    assert(error_resource != 0u);
    turbowasm_wasi02_value_destroy(&result);

    set_resource(&args[0], error_resource);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "error",
               "[method]error.to-debug-string",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_STRING);
    assert(result.as.string.size == 13u);
    assert(memcmp(
               result.as.string.data,
               "stream-failed", 13u) == 0);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, error_resource) == TURBOWASM_OK);
    assert(probe.error_drop_calls == 1u);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input) == TURBOWASM_OK);
    assert(probe.input_drop_calls == 1u);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output) == TURBOWASM_OK);
    assert(probe.output_drop_calls == 1u);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input) == TURBOWASM_TRAPPED);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
}

typedef struct subscribe_poll_probe {
    uint32_t ready_calls;
    uint32_t drop_calls;
    int64_t last_dropped;
} subscribe_poll_probe;

static turbowasm_status subscribe_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    subscribe_poll_probe *probe =
        (subscribe_poll_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(out_ready != NULL);
    ++probe->ready_calls;
    *out_ready = rep.as.i64 == 101;
    return TURBOWASM_OK;
}

static turbowasm_status subscribe_poll_drop(
    void *context,
    turbowasm_value rep) {
    subscribe_poll_probe *probe =
        (subscribe_poll_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    ++probe->drop_calls;
    probe->last_dropped = rep.as.i64;
    return TURBOWASM_OK;
}

static void test_stream_subscribe_pollable_bridge(void) {
    stream_probe stream = {0};
    subscribe_poll_probe poll_probe = {0};
    turbowasm_wasi02_stream_provider stream_provider =
        make_provider(&stream);
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_value rep = {0};
    turbowasm_wasi02_value argument = {0};
    turbowasm_wasi02_value result = {0};
    uint32_t input = 0u;
    uint32_t output = 0u;
    uint32_t input_pollable = 0u;
    uint32_t output_pollable = 0u;
    bool ready = false;

    poll_provider.context = &poll_probe;
    poll_provider.ready = subscribe_poll_ready;
    poll_provider.drop = subscribe_poll_drop;

    assert(turbowasm_wasi02_streams_init(
               &streams,
               &stream_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll,
               &poll_provider,
               1u) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &streams, rep, &input) == TURBOWASM_OK);
    rep.as.i64 = 22;
    assert(turbowasm_wasi02_output_stream_new(
               &streams, rep, &output) == TURBOWASM_OK);

    set_resource(&argument, input);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.subscribe",
               &argument, 1u, &result) == TURBOWASM_UNSUPPORTED);
    assert(stream.input_subscribe_calls == 0u);

    assert(turbowasm_wasi02_streams_attach_poll(
               &streams, &poll) == TURBOWASM_OK);

    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.subscribe",
               &argument, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESOURCE);
    input_pollable = result.as.resource;
    assert(input_pollable != 0u);
    assert(stream.input_subscribe_calls == 1u);
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               input_pollable,
               &ready) == TURBOWASM_OK);
    assert(ready);
    turbowasm_wasi02_value_destroy(&result);

    /*
     * Poll capacity is one. Provider subscription succeeds, but bridge
     * allocation fails and must release the returned provider rep.
     */
    set_resource(&argument, output);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.subscribe",
               &argument, 1u, &result) == TURBOWASM_OUT_OF_MEMORY);
    assert(stream.output_subscribe_calls == 1u);
    assert(poll_probe.drop_calls == 1u);
    assert(poll_probe.last_dropped == 202);

    assert(turbowasm_wasi02_pollable_drop(
               &poll, input_pollable) == TURBOWASM_OK);
    assert(poll_probe.drop_calls == 2u);
    assert(poll_probe.last_dropped == 101);

    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.subscribe",
               &argument, 1u, &result) == TURBOWASM_OK);
    output_pollable = result.as.resource;
    assert(output_pollable != 0u);
    assert(stream.output_subscribe_calls == 2u);
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               output_pollable,
               &ready) == TURBOWASM_OK);
    assert(!ready);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_pollable_drop(
               &poll, output_pollable) == TURBOWASM_OK);
    assert(poll_probe.drop_calls == 3u);
    assert(poll_probe.last_dropped == 202);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
}

int main(void) {
    test_nonblocking_stream_contract();
    test_stream_subscribe_pollable_bridge();
    return 0;
}
