#include "../src/wasi02_poll_native_io.h"
#include "../src/wasi02_sockets.h"

#include <turbowasm/turbowasm.h>

#include <salts/error_codes.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER 0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef struct cancel_probe {
    uint32_t calls;
    native_io_request last_request;
    int status;
} cancel_probe;

typedef struct socket_probe {
    turbowasm_wasi02_native_io_poll *native_poll;
    native_io_request requests[2];
    uint32_t subscribe_calls;
    uint32_t tcp_drop_calls;
} socket_probe;

typedef struct host_probe {
    turbowasm_wasi02_poll *poll;
    uint32_t resource;
    uint32_t calls;
} host_probe;

static int fake_cancel(
    native_io_backend *backend,
    native_io_request request) {
    cancel_probe *probe;

    assert(backend != NULL);
    probe = (cancel_probe *)backend->impl;
    assert(probe != NULL);
    ++probe->calls;
    probe->last_request = request;
    return probe->status;
}

static turbowasm_value rep_i32(int32_t value) {
    turbowasm_value rep = {0};
    rep.kind = TURBOWASM_VALUE_I32;
    rep.as.i32 = value;
    return rep;
}

static turbowasm_status fake_instance_network(
    void *context,
    turbowasm_value *out_rep) {
    (void)context;
    assert(out_rep != NULL);
    *out_rep = rep_i32(1);
    return TURBOWASM_OK;
}

static turbowasm_status fake_network_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    return TURBOWASM_OK;
}

static turbowasm_status fake_tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    (void)context;

    assert(out_rep != NULL);
    assert(out_error != NULL);
    assert(family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ||
           family == TURBOWASM_WASI02_IP_ADDRESS_IPV6);
    *out_rep = rep_i32(101);
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_tcp_drop(
    void *context,
    turbowasm_value rep) {
    socket_probe *probe = (socket_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I32);
    assert(rep.as.i32 == 101);
    ++probe->tcp_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status fake_tcp_subscribe(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_pollable_rep) {
    socket_probe *probe = (socket_probe *)context;
    uint32_t index;

    assert(probe != NULL);
    assert(probe->native_poll != NULL);
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    assert(socket_rep.as.i32 == 101);
    assert(out_pollable_rep != NULL);

    index = probe->subscribe_calls;
    assert(index < 2u);
    ++probe->subscribe_calls;
    return turbowasm_wasi02_native_io_poll_register_request(
        probe->native_poll,
        probe->requests[index],
        out_pollable_rep);
}

static void fake_stream_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
}

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status host_poll_one(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_probe *probe = (host_probe *)context;
    turbowasm_component_value ready_indices = {0};
    turbowasm_status status;

    assert(probe != NULL);
    assert(call != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++probe->calls;
    status = turbowasm_wasi02_poll_many(
        probe->poll,
        &probe->resource,
        1u,
        call,
        &ready_indices,
        trap);
    if (status != TURBOWASM_OK)
        return status;

    assert(ready_indices.kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(ready_indices.as.list.count == 1u);
    assert(ready_indices.as.list.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_U32);
    assert(ready_indices.as.list.items[0].as.u32 == 0u);

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = 0;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;

    turbowasm_component_value_destroy(&ready_indices);
    return TURBOWASM_OK;
}

static const uint8_t module_bytes[] = {
    WASM_HEADER,

    /* type0: () -> i32 */
    0x01,0x05,
    0x01,0x60,0x00,0x01,0x7f,

    /* import host.poll type0 */
    0x02,0x0d,
    0x01,
    0x04,'h','o','s','t',
    0x04,'p','o','l','l',
    0x00,0x00,

    /* one local wrapper, type0 */
    0x03,0x02,
    0x01,0x00,

    /* wrapper body: call host.poll */
    0x0a,0x06,
    0x01,
    0x04,0x00,0x10,0x00,0x0b
};

static uint32_t create_tcp(
    turbowasm_wasi02_sockets *sockets) {
    turbowasm_wasi02_value argument = {0};
    turbowasm_wasi02_value result = {0};
    uint32_t resource;

    argument.kind = TURBOWASM_WASI02_VALUE_ENUM;
    argument.as.enum_index = 0u;
    assert(turbowasm_wasi02_sockets_call(
               sockets,
               "tcp-create-socket",
               "create-tcp-socket",
               &argument,
               1u,
               &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    resource = result.as.result.value->as.resource;
    turbowasm_wasi02_value_destroy(&result);
    return resource;
}

static uint32_t subscribe_tcp(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource) {
    turbowasm_wasi02_value argument = {0};
    turbowasm_wasi02_value result = {0};

    argument.kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    argument.as.resource = socket_resource;
    assert(turbowasm_wasi02_sockets_call(
               sockets,
               "tcp",
               "[method]tcp-socket.subscribe",
               &argument,
               1u,
               &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESOURCE);
    assert(result.as.resource != 0u);
    return result.as.resource;
}

static void test_socket_subscribe_routes_existing_native_io_wait(void) {
    cancel_probe cancel = {0};
    socket_probe socket_state = {0};
    native_io_backend backend = {0};
    turbowasm_wasi02_native_io_poll native_poll = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_socket_provider socket_provider = {0};
    turbowasm_wasi02_sockets sockets = {0};
    host_probe host = {0};
    turbowasm_module module = {0};
    turbowasm_linker linker = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    native_io_completion completion = {0};
    const turbowasm_value *execution_result;
    uint32_t socket_resource;
    uint32_t pollable_resource;
    uint32_t cancelled_pollable;
    bool ready = false;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };

    cancel.status = SALTS_OK;
    backend.impl = &cancel;

    assert(turbowasm_wasi02_native_io_poll_init(
               &native_poll,
               &backend,
               4u,
               2u,
               2u,
               fake_cancel) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_provider(
               &native_poll,
               &poll_provider) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll,
               &poll_provider,
               4u) == TURBOWASM_OK);

    stream_provider.input_drop = fake_stream_drop;
    stream_provider.output_drop = fake_stream_drop;
    assert(turbowasm_wasi02_streams_init(
               &streams,
               &stream_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams,
               &poll) == TURBOWASM_OK);

    socket_state.native_poll = &native_poll;
    socket_state.requests[0] = (native_io_request){7u, 41u};
    socket_state.requests[1] = (native_io_request){8u, 42u};

    socket_provider.context = &socket_state;
    socket_provider.instance_network = fake_instance_network;
    socket_provider.network_drop = fake_network_drop;
    socket_provider.tcp_create = fake_tcp_create;
    socket_provider.tcp_drop = fake_tcp_drop;
    socket_provider.tcp_subscribe = fake_tcp_subscribe;

    assert(turbowasm_wasi02_sockets_init(
               &sockets,
               &socket_provider,
               1u,
               2u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_sockets_attach_io(
               &sockets,
               &streams,
               &poll) == TURBOWASM_OK);

    socket_resource = create_tcp(&sockets);
    pollable_resource = subscribe_tcp(
        &sockets, socket_resource);
    assert(socket_state.subscribe_calls == 1u);
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               pollable_resource,
               &ready) == TURBOWASM_OK);
    assert(!ready);

    host.poll = &poll;
    host.resource = pollable_resource;
    assert(turbowasm_module_load_borrowed(
               &module,
               module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("poll", 4u),
               &host_type,
               host_poll_one,
               &host) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    assert(turbowasm_execution_create(
               &execution,
               &instance,
               1u,
               NULL,
               0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution,
               NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(host.calls == 1u);
    assert(turbowasm_execution_pending_host_wait(
               &execution,
               &wait));
    assert(wait.generation != 0u);
    assert(wait.operation_token != 0u);

    completion.request = socket_state.requests[0];
    completion.kind = NATIVE_IO_COMPLETION_OK;
    completion.status = SALTS_OK;
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution,
               NULL) == TURBOWASM_OK);
    assert(host.calls == 1u);
    execution_result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(execution_result != NULL);
    assert(execution_result->kind == TURBOWASM_VALUE_I32);
    assert(execution_result->as.i32 == 0);

    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               pollable_resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    /*
     * A terminal socket pollable drops without cancellation: completion was
     * already observed by the shared W4 NativeIO router.
     */
    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               pollable_resource) == TURBOWASM_OK);
    assert(cancel.calls == 0u);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    /*
     * A second subscribe registers another already-active NativeIO identity.
     * Dropping it requests cancellation but must not free/reuse the router slot
     * until the NativeIO owner publishes the terminal cancellation completion.
     */
    cancelled_pollable = subscribe_tcp(
        &sockets, socket_resource);
    assert(socket_state.subscribe_calls == 2u);
    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               cancelled_pollable) == TURBOWASM_OK);
    assert(cancel.calls == 1u);
    assert(cancel.last_request.slot ==
           socket_state.requests[1].slot);
    assert(cancel.last_request.generation ==
           socket_state.requests[1].generation);

    completion = (native_io_completion){0};
    completion.request = socket_state.requests[1];
    completion.kind = NATIVE_IO_COMPLETION_CANCELLED;
    completion.status = SALTS_ECANCELED;
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_OK);

    /*
     * The terminal completion retires the generation. Re-delivery is stale
     * and must fail deterministically instead of touching a recycled slot.
     */
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_tcp_drop(
               &sockets,
               socket_resource) == TURBOWASM_OK);
    assert(socket_state.tcp_drop_calls == 1u);

    assert(turbowasm_wasi02_sockets_destroy(
               &sockets) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_destroy(
               &native_poll) == TURBOWASM_OK);
    backend.impl = NULL;
}

int main(void) {
    test_socket_subscribe_routes_existing_native_io_wait();
    return 0;
}
