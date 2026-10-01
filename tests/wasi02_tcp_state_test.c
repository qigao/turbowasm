#include "../src/wasi02_sockets.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_state {
    int next_rep;

    turbowasm_wasi02_socket_error start_bind_error;
    turbowasm_wasi02_socket_error finish_bind_error;
    turbowasm_wasi02_socket_error start_connect_error;
    turbowasm_wasi02_socket_error finish_connect_error;
    turbowasm_wasi02_socket_error start_listen_error;
    turbowasm_wasi02_socket_error finish_listen_error;
    turbowasm_wasi02_socket_error accept_error;
    turbowasm_wasi02_socket_error shutdown_error;

    unsigned network_drop_calls;
    unsigned tcp_drop_calls;
    unsigned start_bind_calls;
    unsigned finish_bind_calls;
    unsigned start_connect_calls;
    unsigned finish_connect_calls;
    unsigned start_listen_calls;
    unsigned finish_listen_calls;
    unsigned accept_calls;
    unsigned subscribe_calls;
    unsigned shutdown_calls;
    unsigned input_drop_calls;
    unsigned output_drop_calls;
    unsigned poll_drop_calls;

    bool keepalive;
    uint64_t u64_value;
    uint32_t u32_value;
    uint8_t u8_value;
} fake_state;

static turbowasm_value rep_i32(int value) {
    turbowasm_value rep = {0};
    rep.kind = TURBOWASM_VALUE_I32;
    rep.as.i32 = value;
    return rep;
}

static turbowasm_status fake_instance_network(
    void *context,
    turbowasm_value *out_rep) {
    fake_state *state = (fake_state *)context;
    *out_rep = rep_i32(++state->next_rep);
    return TURBOWASM_OK;
}

static turbowasm_status fake_network_drop(
    void *context,
    turbowasm_value rep) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    ++state->network_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status fake_tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ||
           family == TURBOWASM_WASI02_IP_ADDRESS_IPV6);
    *out_rep = rep_i32(++state->next_rep);
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_tcp_drop(
    void *context,
    turbowasm_value rep) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    ++state->tcp_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status fake_start_bind(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *address,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    assert(network_rep.kind == TURBOWASM_VALUE_I32);
    assert(address != NULL);
    ++state->start_bind_calls;
    *out_error = state->start_bind_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_finish_bind(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    ++state->finish_bind_calls;
    *out_error = state->finish_bind_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_start_connect(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *address,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    assert(network_rep.kind == TURBOWASM_VALUE_I32);
    assert(address != NULL);
    ++state->start_connect_calls;
    *out_error = state->start_connect_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_finish_connect(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_input,
    turbowasm_value *out_output,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    ++state->finish_connect_calls;
    *out_error = state->finish_connect_error;
    if (*out_error == TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        *out_input = rep_i32(++state->next_rep);
        *out_output = rep_i32(++state->next_rep);
    }
    return TURBOWASM_OK;
}

static turbowasm_status fake_start_listen(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    ++state->start_listen_calls;
    *out_error = state->start_listen_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_finish_listen(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    ++state->finish_listen_calls;
    *out_error = state->finish_listen_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_accept(
    void *context,
    turbowasm_value listener_rep,
    turbowasm_value *out_socket,
    turbowasm_value *out_input,
    turbowasm_value *out_output,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(listener_rep.kind == TURBOWASM_VALUE_I32);
    ++state->accept_calls;
    *out_error = state->accept_error;
    if (*out_error == TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        *out_socket = rep_i32(++state->next_rep);
        *out_input = rep_i32(++state->next_rep);
        *out_output = rep_i32(++state->next_rep);
    }
    return TURBOWASM_OK;
}

static turbowasm_status fake_get_address(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_ip_socket_address *out_address,
    turbowasm_wasi02_socket_error *out_error) {
    (void)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    memset(out_address, 0, sizeof(*out_address));
    out_address->family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    out_address->as.ipv4.port = UINT16_C(8080);
    out_address->as.ipv4.address[0] = 127u;
    out_address->as.ipv4.address[3] = 1u;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_get_bool(
    void *context,
    turbowasm_value rep,
    bool *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    *out_value = state->keepalive;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_set_bool(
    void *context,
    turbowasm_value rep,
    bool value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    state->keepalive = value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_get_u64(
    void *context,
    turbowasm_value rep,
    uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    *out_value = state->u64_value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_set_u64(
    void *context,
    turbowasm_value rep,
    uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    state->u64_value = value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_get_u32(
    void *context,
    turbowasm_value rep,
    uint32_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    *out_value = state->u32_value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_set_u32(
    void *context,
    turbowasm_value rep,
    uint32_t value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    state->u32_value = value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_get_u8(
    void *context,
    turbowasm_value rep,
    uint8_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    *out_value = state->u8_value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_set_u8(
    void *context,
    turbowasm_value rep,
    uint8_t value,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    state->u8_value = value;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_subscribe(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_rep) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    ++state->subscribe_calls;
    *out_rep = rep_i32(++state->next_rep);
    return TURBOWASM_OK;
}

static turbowasm_status fake_shutdown(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_tcp_shutdown_type how,
    turbowasm_wasi02_socket_error *out_error) {
    fake_state *state = (fake_state *)context;
    assert(socket_rep.kind == TURBOWASM_VALUE_I32);
    assert(how <= TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH);
    ++state->shutdown_calls;
    *out_error = state->shutdown_error;
    return TURBOWASM_OK;
}

static turbowasm_status fake_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    (void)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    *out_ready = true;
    return TURBOWASM_OK;
}

static turbowasm_status fake_poll_drop(
    void *context,
    turbowasm_value rep) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    ++state->poll_drop_calls;
    return TURBOWASM_OK;
}

static void fake_input_drop(
    void *context,
    turbowasm_value rep) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    ++state->input_drop_calls;
}

static void fake_output_drop(
    void *context,
    turbowasm_value rep) {
    fake_state *state = (fake_state *)context;
    assert(rep.kind == TURBOWASM_VALUE_I32);
    ++state->output_drop_calls;
}

static turbowasm_wasi02_socket_provider provider_config(
    fake_state *state) {
    turbowasm_wasi02_socket_provider provider;
    memset(&provider, 0, sizeof(provider));
    provider.context = state;
    provider.instance_network = fake_instance_network;
    provider.network_drop = fake_network_drop;
    provider.tcp_create = fake_tcp_create;
    provider.tcp_drop = fake_tcp_drop;
    provider.tcp_start_bind = fake_start_bind;
    provider.tcp_finish_bind = fake_finish_bind;
    provider.tcp_start_connect = fake_start_connect;
    provider.tcp_finish_connect = fake_finish_connect;
    provider.tcp_start_listen = fake_start_listen;
    provider.tcp_finish_listen = fake_finish_listen;
    provider.tcp_accept = fake_accept;
    provider.tcp_local_address = fake_get_address;
    provider.tcp_remote_address = fake_get_address;
    provider.tcp_set_listen_backlog_size = fake_set_u64;
    provider.tcp_keep_alive_enabled = fake_get_bool;
    provider.tcp_set_keep_alive_enabled = fake_set_bool;
    provider.tcp_keep_alive_idle_time = fake_get_u64;
    provider.tcp_set_keep_alive_idle_time = fake_set_u64;
    provider.tcp_keep_alive_interval = fake_get_u64;
    provider.tcp_set_keep_alive_interval = fake_set_u64;
    provider.tcp_keep_alive_count = fake_get_u32;
    provider.tcp_set_keep_alive_count = fake_set_u32;
    provider.tcp_hop_limit = fake_get_u8;
    provider.tcp_set_hop_limit = fake_set_u8;
    provider.tcp_receive_buffer_size = fake_get_u64;
    provider.tcp_set_receive_buffer_size = fake_set_u64;
    provider.tcp_send_buffer_size = fake_get_u64;
    provider.tcp_set_send_buffer_size = fake_set_u64;
    provider.tcp_subscribe = fake_subscribe;
    provider.tcp_shutdown = fake_shutdown;
    return provider;
}

static turbowasm_wasi02_value resource_value(uint32_t resource) {
    turbowasm_wasi02_value value = {0};
    value.kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    value.as.resource = resource;
    return value;
}

static turbowasm_wasi02_value enum_value(uint32_t index) {
    turbowasm_wasi02_value value = {0};
    value.kind = TURBOWASM_WASI02_VALUE_ENUM;
    value.as.enum_index = index;
    return value;
}

static void expect_error(
    turbowasm_wasi02_value *result,
    turbowasm_wasi02_socket_error error) {
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_ENUM);
    assert(result->as.result.value->as.enum_index ==
           (uint32_t)error - 1u);
    turbowasm_wasi02_value_destroy(result);
}

static void expect_unit_ok(turbowasm_wasi02_value *result) {
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value == NULL);
    turbowasm_wasi02_value_destroy(result);
}

static uint32_t expect_resource_result(
    turbowasm_wasi02_value *result) {
    uint32_t resource;
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    resource = result->as.result.value->as.resource;
    turbowasm_wasi02_value_destroy(result);
    return resource;
}

static uint32_t create_tcp(
    turbowasm_wasi02_sockets *sockets,
    uint32_t family) {
    turbowasm_wasi02_value argument = enum_value(family);
    turbowasm_wasi02_value result = {0};
    assert(turbowasm_wasi02_sockets_call(
               sockets,
               "tcp-create-socket",
               "create-tcp-socket",
               &argument, 1u, &result) == TURBOWASM_OK);
    return expect_resource_result(&result);
}

static uint32_t instance_network(
    turbowasm_wasi02_sockets *sockets) {
    turbowasm_wasi02_value result = {0};
    assert(turbowasm_wasi02_sockets_call(
               sockets,
               "instance-network",
               "instance-network",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESOURCE);
    return result.as.resource;
}

static void make_ipv4_address(
    uint16_t port,
    turbowasm_wasi02_value *variant,
    turbowasm_wasi02_value *record,
    turbowasm_wasi02_value record_items[2],
    turbowasm_wasi02_value *tuple,
    turbowasm_wasi02_value tuple_items[4]) {
    size_t i;
    memset(variant, 0, sizeof(*variant));
    memset(record, 0, sizeof(*record));
    memset(record_items, 0, 2u * sizeof(*record_items));
    memset(tuple, 0, sizeof(*tuple));
    memset(tuple_items, 0, 4u * sizeof(*tuple_items));

    record_items[0].kind = TURBOWASM_WASI02_VALUE_U16;
    record_items[0].as.u16 = port;
    for (i = 0u; i < 4u; ++i) {
        tuple_items[i].kind = TURBOWASM_WASI02_VALUE_U8;
        tuple_items[i].as.u8 = i == 0u ? 127u : (i == 3u ? 1u : 0u);
    }
    tuple->kind = TURBOWASM_WASI02_VALUE_TUPLE;
    tuple->as.tuple.items = tuple_items;
    tuple->as.tuple.count = 4u;
    record_items[1] = *tuple;
    record->kind = TURBOWASM_WASI02_VALUE_RECORD;
    record->as.record.items = record_items;
    record->as.record.count = 2u;
    variant->kind = TURBOWASM_WASI02_VALUE_VARIANT;
    variant->as.variant.case_index = 0u;
    variant->as.variant.value = record;
}

static void assert_state(
    turbowasm_wasi02_sockets *sockets,
    uint32_t resource,
    turbowasm_wasi02_tcp_state expected) {
    turbowasm_wasi02_tcp_state state;
    assert(turbowasm_wasi02_tcp_state_get(
               sockets, resource, &state) == TURBOWASM_OK);
    assert(state == expected);
}

int main(void) {
    fake_state state;
    turbowasm_wasi02_socket_provider socket_provider;
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_sockets sockets = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_value result = {0};
    turbowasm_wasi02_value address = {0};
    turbowasm_wasi02_value address_record = {0};
    turbowasm_wasi02_value address_record_items[2];
    turbowasm_wasi02_value address_tuple = {0};
    turbowasm_wasi02_value address_tuple_items[4];
    turbowasm_wasi02_value args[3];
    uint32_t network;
    uint32_t listener;
    uint32_t client;
    uint32_t child;
    uint32_t input_resource;
    uint32_t output_resource;
    uint32_t pollable_resource;
    unsigned before;
    turbowasm_wasi02_tcp_state ignored_state;

    memset(&state, 0, sizeof(state));
    state.next_rep = 100;
    state.keepalive = true;
    state.u64_value = UINT64_C(30);
    state.u32_value = UINT32_C(5);
    state.u8_value = UINT8_C(64);
    socket_provider = provider_config(&state);

    poll_provider.context = &state;
    poll_provider.ready = fake_poll_ready;
    poll_provider.drop = fake_poll_drop;
    assert(turbowasm_wasi02_poll_init(
               &poll, &poll_provider, 16u) == TURBOWASM_OK);

    stream_provider.context = &state;
    stream_provider.input_drop = fake_input_drop;
    stream_provider.output_drop = fake_output_drop;
    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider, 32u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams, &poll) == TURBOWASM_OK);

    assert(turbowasm_wasi02_sockets_init(
               &sockets, &socket_provider, 4u, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_sockets_attach_io(
               &sockets, &streams, &poll) == TURBOWASM_OK);

    network = instance_network(&sockets);
    listener = create_tcp(&sockets, 0u);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_UNBOUND);

    args[0] = resource_value(listener);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.address-family",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_ENUM);
    assert(result.as.enum_index == 0u);
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = UINT64_C(64);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-listen-backlog-size",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(state.u64_value == UINT64_C(64));

    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.keep-alive-enabled",
               args, 1u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_BOOL);
    assert(result.as.result.value->as.boolean);
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_BOOL;
    args[1].as.boolean = false;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-keep-alive-enabled",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(!state.keepalive);

    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = UINT64_C(11);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-keep-alive-idle-time",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.keep-alive-idle-time",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u64 == UINT64_C(11));
    turbowasm_wasi02_value_destroy(&result);

    args[1].as.u64 = UINT64_C(12);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-keep-alive-interval",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.keep-alive-interval",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u64 == UINT64_C(12));
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_U32;
    args[1].as.u32 = UINT32_C(7);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-keep-alive-count",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.keep-alive-count",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u32 == UINT32_C(7));
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_U8;
    args[1].as.u8 = UINT8_C(55);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.set-hop-limit",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.hop-limit",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u8 == UINT8_C(55));
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = UINT64_C(4096);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-receive-buffer-size",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.receive-buffer-size",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u64 == UINT64_C(4096));
    turbowasm_wasi02_value_destroy(&result);

    args[1].as.u64 = UINT64_C(8192);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-send-buffer-size",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.send-buffer-size",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.u64 == UINT64_C(8192));
    turbowasm_wasi02_value_destroy(&result);

    make_ipv4_address(
        UINT16_C(8080),
        &address, &address_record, address_record_items,
        &address_tuple, address_tuple_items);
    args[0] = resource_value(listener);
    args[1] = resource_value(network);
    args[2] = address;

    state.start_bind_error =
        TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-bind",
               args, 3u, &result) == TURBOWASM_OK);
    expect_error(
        &result,
        TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_UNBOUND);

    state.start_bind_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-bind",
               args, 3u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(
        &sockets, listener, TURBOWASM_WASI02_TCP_BIND_IN_PROGRESS);

    before = state.start_connect_calls;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-connect",
               args, 3u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    assert(state.start_connect_calls == before);

    state.finish_bind_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-bind",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK);
    assert_state(
        &sockets, listener, TURBOWASM_WASI02_TCP_BIND_IN_PROGRESS);

    state.finish_bind_error = TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_IN_USE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-bind",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_IN_USE);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_UNBOUND);

    state.finish_bind_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-bind",
               args, 3u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-bind",
               args, 1u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_BOUND);

    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-bind",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS);

    state.start_listen_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    state.finish_listen_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-listen",
               args, 1u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(
        &sockets, listener, TURBOWASM_WASI02_TCP_LISTEN_IN_PROGRESS);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-listen",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK);

    state.finish_listen_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-listen",
               args, 1u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_LISTENING);

    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.is-listening",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_BOOL);
    assert(result.as.boolean);
    turbowasm_wasi02_value_destroy(&result);

    state.accept_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.accept",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK);
    assert_state(&sockets, listener, TURBOWASM_WASI02_TCP_LISTENING);

    state.accept_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.accept",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind == TURBOWASM_WASI02_VALUE_TUPLE);
    assert(result.as.result.value->as.tuple.count == 3u);
    child = result.as.result.value->as.tuple.items[0].as.resource;
    input_resource = result.as.result.value->as.tuple.items[1].as.resource;
    output_resource = result.as.result.value->as.tuple.items[2].as.resource;
    assert_state(&sockets, child, TURBOWASM_WASI02_TCP_CONNECTED);
    turbowasm_wasi02_value_destroy(&result);

    args[0] = resource_value(listener);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.subscribe",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESOURCE);
    pollable_resource = result.as.resource;
    assert(turbowasm_wasi02_pollable_drop(
               &poll, pollable_resource) == TURBOWASM_OK);

    args[0] = resource_value(child);
    args[1] = enum_value(2u);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.shutdown",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(&sockets, child, TURBOWASM_WASI02_TCP_CONNECTED);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_tcp_drop(&sockets, child) == TURBOWASM_OK);

    client = create_tcp(&sockets, 1u);
    args[0] = resource_value(client);
    args[1] = resource_value(network);
    args[2] = address;
    before = state.start_connect_calls;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-connect",
               args, 3u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    assert(state.start_connect_calls == before);
    assert_state(&sockets, client, TURBOWASM_WASI02_TCP_CLOSED);
    assert(turbowasm_wasi02_tcp_drop(&sockets, client) == TURBOWASM_OK);

    client = create_tcp(&sockets, 0u);
    args[0] = resource_value(client);
    args[1] = resource_value(network);
    args[2] = address;
    state.start_connect_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    state.finish_connect_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-connect",
               args, 3u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert_state(
        &sockets, client, TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-connect",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK);

    state.finish_connect_error =
        TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_REFUSED;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-connect",
               args, 1u, &result) == TURBOWASM_OK);
    expect_error(
        &result, TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_REFUSED);
    assert_state(&sockets, client, TURBOWASM_WASI02_TCP_CLOSED);
    assert(turbowasm_wasi02_tcp_drop(&sockets, client) == TURBOWASM_OK);

    client = create_tcp(&sockets, 0u);
    args[0] = resource_value(client);
    args[1] = resource_value(network);
    args[2] = address;
    state.finish_connect_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.start-connect",
               args, 3u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.finish-connect",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    input_resource = result.as.result.value->as.tuple.items[0].as.resource;
    output_resource = result.as.result.value->as.tuple.items[1].as.resource;
    turbowasm_wasi02_value_destroy(&result);
    assert_state(&sockets, client, TURBOWASM_WASI02_TCP_CONNECTED);

    args[0] = resource_value(client);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.local-address",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_VARIANT);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.keep-alive-enabled",
               args, 1u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(!result.as.result.value->as.boolean);
    turbowasm_wasi02_value_destroy(&result);

    args[1].kind = TURBOWASM_WASI02_VALUE_U8;
    args[1].as.u8 = 0u;
    before = state.u8_value;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.set-hop-limit",
               args, 2u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    assert(state.u8_value == before);

    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 128u;
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp",
               "[method]tcp-socket.set-listen-backlog-size",
               args, 2u, &result) == TURBOWASM_OK);
    expect_error(&result, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);

    args[1] = enum_value(0u);
    assert(turbowasm_wasi02_sockets_call(
               &sockets, "tcp", "[method]tcp-socket.shutdown",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output_resource) == TURBOWASM_OK);

    assert(turbowasm_wasi02_tcp_drop(&sockets, client) == TURBOWASM_OK);
    assert(turbowasm_wasi02_tcp_state_get(
               &sockets, client, &ignored_state) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_tcp_drop(&sockets, listener) == TURBOWASM_OK);
    assert(turbowasm_wasi02_network_drop(
               &sockets, network) == TURBOWASM_OK);

    assert(state.network_drop_calls == 1u);
    assert(state.tcp_drop_calls == 5u);
    assert(state.input_drop_calls == 2u);
    assert(state.output_drop_calls == 2u);
    assert(state.poll_drop_calls == 1u);

    assert(turbowasm_wasi02_sockets_destroy(
               &sockets) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    return 0;
}
