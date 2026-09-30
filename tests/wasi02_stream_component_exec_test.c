#include "../src/wasi02_streams.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct bytebuf {
    uint8_t data[4096];
    size_t size;
} bytebuf;

static void put_u8(bytebuf *b, uint8_t v) {
    assert(b != NULL && b->size < sizeof(b->data));
    b->data[b->size++] = v;
}

static void put_bytes(bytebuf *b, const uint8_t *p, size_t n) {
    assert(b != NULL && (n == 0u || p != NULL));
    assert(n <= sizeof(b->data) - b->size);
    if (n != 0u) {
        memcpy(b->data + b->size, p, n);
        b->size += n;
    }
}

static void put_uleb(bytebuf *b, uint32_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7fu);
        v >>= 7u;
        if (v != 0u)
            byte |= 0x80u;
        put_u8(b, byte);
    } while (v != 0u);
}

static void put_name(bytebuf *b, const char *s) {
    size_t n = strlen(s);
    assert(n <= UINT32_MAX);
    put_uleb(b, (uint32_t)n);
    put_bytes(b, (const uint8_t *)s, n);
}

static void put_nameattr(bytebuf *b, const char *s) {
    put_u8(b, 0u);
    put_name(b, s);
}

static void put_section(
    bytebuf *module,
    uint8_t id,
    const bytebuf *payload) {
    assert(payload->size <= UINT32_MAX);
    put_u8(module, id);
    put_uleb(module, (uint32_t)payload->size);
    put_bytes(module, payload->data, payload->size);
}

static void put_core_header(bytebuf *b) {
    static const uint8_t header[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00
    };
    put_bytes(b, header, sizeof(header));
}

static void put_component_header(bytebuf *b) {
    static const uint8_t header[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00
    };
    put_bytes(b, header, sizeof(header));
}

/*
 * Core consumer:
 *   (import "p" "subscribe" (func (param i32) (result i32)))
 *   (export "run" (func 0))
 */
static void build_subscribe_consumer(bytebuf *out) {
    bytebuf s = {0};

    put_core_header(out);

    put_uleb(&s, 1u);
    put_u8(&s, 0x60u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x7fu);
    put_uleb(&s, 1u);
    put_u8(&s, 0x7fu);
    put_section(out, 1u, &s);

    s.size = 0u;
    put_uleb(&s, 1u);
    put_name(&s, "p");
    put_name(&s, "subscribe");
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_section(out, 2u, &s);

    s.size = 0u;
    put_uleb(&s, 1u);
    put_name(&s, "run");
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_section(out, 7u, &s);
}

static void append_streams_instance_type(bytebuf *component) {
    bytebuf s = {0};

    put_uleb(&s, 1u);
    put_u8(&s, 0x42u);
    put_uleb(&s, 6u);

    /* local type0 = exported input-stream resource */
    put_u8(&s, 0x04u);
    put_nameattr(&s, "input-stream");
    put_u8(&s, 0x03u);
    put_u8(&s, 0x01u);

    /* local type1 = exported pollable resource identity */
    put_u8(&s, 0x04u);
    put_nameattr(&s, "pollable");
    put_u8(&s, 0x03u);
    put_u8(&s, 0x01u);

    /* local type2 = borrow<input-stream> */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x68u);
    put_uleb(&s, 0u);

    /* local type3 = own<pollable> */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x69u);
    put_uleb(&s, 1u);

    /* local type4 = subscribe(self: borrow<input-stream>) -> own<pollable> */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x40u);
    put_uleb(&s, 1u);
    put_name(&s, "self");
    put_uleb(&s, 2u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 3u);

    put_u8(&s, 0x04u);
    put_nameattr(&s, "[method]input-stream.subscribe");
    put_u8(&s, 0x01u);
    put_uleb(&s, 4u);

    put_section(component, 7u, &s);
}

static void build_subscribe_component(bytebuf *out) {
    bytebuf consumer = {0};
    bytebuf s = {0};

    put_component_header(out);
    build_subscribe_consumer(&consumer);
    put_section(out, 1u, &consumer);

    append_streams_instance_type(out);

    /* component instance0 = imported wasi:io/streams@0.2.8 */
    put_uleb(&s, 1u);
    put_nameattr(&s, "wasi:io/streams@0.2.8");
    put_u8(&s, 0x05u);
    put_uleb(&s, 0u);
    put_section(out, 10u, &s);

    /*
     * Alias both nominal resource types into the outer graph, then alias the
     * subscribe function as component func0.
     */
    s.size = 0u;
    put_uleb(&s, 3u);

    put_u8(&s, 0x03u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "input-stream");

    put_u8(&s, 0x03u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "pollable");

    put_u8(&s, 0x01u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "[method]input-stream.subscribe");

    put_section(out, 6u, &s);

    /* canon lower component func0 -> core func0 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x01u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_uleb(&s, 0u);
    put_section(out, 8u, &s);

    /* core instance0 = inline { subscribe -> lowered core func0 } */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 1u);
    put_name(&s, "subscribe");
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_section(out, 2u, &s);

    /* core instance1 = instantiate consumer module0 with p=instance0 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_uleb(&s, 1u);
    put_name(&s, "p");
    put_u8(&s, 0x12u);
    put_uleb(&s, 0u);
    put_section(out, 2u, &s);

    /* core func1 = alias core instance1 export "run" */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 1u);
    put_name(&s, "run");
    put_section(out, 6u, &s);

    /*
     * outer type3 = borrow<input-stream(type1)>
     * outer type4 = own<pollable(type2)>
     * outer type5 = run(self: type3) -> type4
     */
    s.size = 0u;
    put_uleb(&s, 3u);
    put_u8(&s, 0x68u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x69u);
    put_uleb(&s, 2u);
    put_u8(&s, 0x40u);
    put_uleb(&s, 1u);
    put_name(&s, "self");
    put_uleb(&s, 3u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 4u);
    put_section(out, 7u, &s);

    /* canon lift core func1 as component func1 using type5 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 1u);
    put_uleb(&s, 0u);
    put_uleb(&s, 5u);
    put_section(out, 8u, &s);

    /* export component func1 as run */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_nameattr(&s, "run");
    put_u8(&s, 0x01u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_section(out, 11u, &s);
}

typedef struct subscribe_probe {
    uint32_t subscribe_calls;
    uint32_t input_drop_calls;
    uint32_t poll_drop_calls;
} subscribe_probe;

static turbowasm_status probe_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    subscribe_probe *probe = (subscribe_probe *)context;

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
    subscribe_probe *probe = (subscribe_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static turbowasm_status probe_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    (void)context;
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    assert(out_ready != NULL);
    *out_ready = true;
    return TURBOWASM_OK;
}

static turbowasm_status probe_poll_drop(
    void *context,
    turbowasm_value rep) {
    subscribe_probe *probe = (subscribe_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

static void test_subscribe_component_resource_bridge(void) {
    bytebuf component_bytes = {0};
    subscribe_probe probe = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_value stream_rep = {0};
    uint32_t input_resource = 0u;
    uint32_t pollable_resource;
    bool ready = false;

    build_subscribe_component(&component_bytes);

    stream_provider.context = &probe;
    stream_provider.input_subscribe = probe_subscribe;
    stream_provider.input_drop = probe_input_drop;

    poll_provider.context = &probe;
    poll_provider.ready = probe_ready;
    poll_provider.drop = probe_poll_drop;

    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll, &poll_provider, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams, &poll) == TURBOWASM_OK);

    stream_rep.kind = TURBOWASM_VALUE_I64;
    stream_rep.as.i64 = 11;
    assert(turbowasm_wasi02_input_stream_new(
               &streams,
               stream_rep,
               &input_resource) == TURBOWASM_OK);

    assert(turbowasm_component_binary_load(
               &binary,
               component_bytes.data,
               component_bytes.size) == TURBOWASM_OK);
    assert(binary.import_count == 1u);
    assert(binary.canon_lower_count == 1u);
    assert(binary.canon_lift_count == 1u);

    assert(turbowasm_wasi02_streams_component_exec_init(
               &exec,
               &binary,
               &streams) == TURBOWASM_OK);
    assert(streams.input_stream_identity_bound);
    assert(poll.pollable_identity_bound);
    assert(streams.input_stream_identity !=
           poll.pollable_identity);

    argument.kind = TURBOWASM_COMPONENT_TYPE_BORROW;
    argument.as.resource_rep.kind = TURBOWASM_VALUE_I32;
    argument.as.resource_rep.as.i32 =
        (int32_t)input_resource;

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run", 3u,
               &argument, 1u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(result.as.resource_rep.kind ==
           TURBOWASM_VALUE_I32);
    assert(result.as.resource_rep.as.i32 > 0);
    assert(probe.subscribe_calls == 1u);

    pollable_resource =
        (uint32_t)result.as.resource_rep.as.i32;
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               pollable_resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               pollable_resource) == TURBOWASM_OK);
    assert(probe.poll_drop_calls == 1u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams,
               input_resource) == TURBOWASM_OK);
    assert(probe.input_drop_calls == 1u);

    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
}

int main(void) {
    test_subscribe_component_resource_bridge();
    return 0;
}
