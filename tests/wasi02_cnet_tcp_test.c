#include <turbowasm/wasi02_cnet.h>
#include <turbowasm/wasi02.h>
#include "wasi02/fixtures/generated/wasi02_toolchain_fixtures.h"
#include <salts/error_codes.h>
#include <tinytest.h>
#include <string.h>

static turbowasm_wasi02_io io;
static turbowasm_wasi02_cnet adapter;
static native_io_backend backend;
static turbowasm_wasi02_socket_provider sockets;
static turbowasm_wasi02_stream_provider streams;
static turbowasm_wasi02_poll_provider poll;
static turbowasm_value network, listener, client, accepted;
static turbowasm_value inputs[2], outputs[2], subscriptions[2];
static turbowasm_wasi02 facade;
static turbowasm_component component;
static turbowasm_component_instance instance;
static turbowasm_component_call call;
static turbowasm_status stdin_create(void *context, turbowasm_value *out) {
    (void)context;
    if (inputs[1].kind != TURBOWASM_VALUE_I64) return TURBOWASM_INVALID_ARGUMENT;
    *out = inputs[1]; memset(&inputs[1], 0, sizeof(inputs[1])); return TURBOWASM_OK;
}

static void pump(void) {
    size_t events, count = 0; native_io_completion batch[8]; uint32_t timeout; int status;
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_next_timeout(&adapter, 10, &timeout), TURBOWASM_OK);
    status = native_io_backend_observe(&backend, batch, 8, timeout, &count);
    check_true(status == SALTS_OK || status == SALTS_ETIMEDOUT);
    for (size_t i = 0; i < count; ++i) {
        bool consumed = false;
        check_equal(turbowasm_wasi02_cnet_route_completion(&adapter, &batch[i], &consumed), TURBOWASM_OK);
        check_true(consumed);
    }
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_advance(&io), TURBOWASM_OK);
}
static bool is_ready(turbowasm_value subscription) {
    bool ready = false; check_equal(poll.ready(poll.context, subscription, &ready), TURBOWASM_OK); return ready;
}
static turbowasm_wasi02_ip_socket_address start_listener(turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_socket_error error;
    turbowasm_wasi02_ip_socket_address address = {0};
    address.family = family;
    if (family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) {
        address.as.ipv4.address[0] = 127; address.as.ipv4.address[3] = 1;
    } else address.as.ipv6.address[7] = 1;
    check_equal(sockets.tcp_create(sockets.context, address.family, &listener, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_set_keep_alive_enabled(sockets.context, listener, true, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_set_hop_limit(sockets.context, listener, 42, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_start_bind(sockets.context, listener, network, &address, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_finish_bind(sockets.context, listener, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_start_listen(sockets.context, listener, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_finish_listen(sockets.context, listener, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_local_address(sockets.context, listener, &address, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_not_equal(family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ? address.as.ipv4.port : address.as.ipv6.port, 0);
    return address;
}
static void connect_pair_family(turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_socket_error error;
    turbowasm_wasi02_ip_socket_address address = start_listener(family), remote = {0};
    check_equal(sockets.tcp_create(sockets.context, address.family, &client, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_start_connect(sockets.context, client, network, &address, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_subscribe(sockets.context, client, &subscriptions[0]), TURBOWASM_OK);
    for (unsigned i = 0; i < 200 && !is_ready(subscriptions[0]); ++i) pump();
    check_true(is_ready(subscriptions[0]));
    check_equal(sockets.tcp_finish_connect(sockets.context, client, &inputs[0], &outputs[0], &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_remote_address(sockets.context, client, &remote, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE); check_equal(remote.family, family);
    check_equal(family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ? remote.as.ipv4.port : remote.as.ipv6.port,
        family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ? address.as.ipv4.port : address.as.ipv6.port);
    for (unsigned i = 0; i < 200; ++i) {
        check_equal(sockets.tcp_accept(sockets.context, listener, &accepted, &inputs[1], &outputs[1], &error), TURBOWASM_OK);
        if (error != TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK) break;
        pump();
    }
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(streams.input_subscribe(streams.context, inputs[1], &subscriptions[1]), TURBOWASM_OK);
    {
        bool keep_alive = false; uint8_t hop = 0;
        check_equal(sockets.tcp_keep_alive_enabled(sockets.context, accepted, &keep_alive, &error), TURBOWASM_OK);
        check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE); check_true(keep_alive);
        check_equal(sockets.tcp_hop_limit(sockets.context, accepted, &hop, &error), TURBOWASM_OK);
        check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE); check_equal(hop, (uint8_t)42);
    }
}
static void connect_pair(void) { connect_pair_family(TURBOWASM_WASI02_IP_ADDRESS_IPV4); }
static void component_roundtrip(bool wide) {
    turbowasm_wasi02_ip_socket_address address = start_listener(TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    turbowasm_wasi02_socket_error socket_error;
    turbowasm_wasi02_stream_error error;
    turbowasm_wasi02_config config = {0}; turbowasm_component_host_value argument = {0}, result = {0};
    turbowasm_name run = {(const uint8_t *)"run", 3}; turbowasm_status status;
    uint64_t permit; const uint8_t *data; size_t size;
    config.pollable_capacity = 8; config.stream_resource_capacity = 8;
    config.socket_network_resource_capacity = 2; config.tcp_socket_resource_capacity = 4;
    check_equal(turbowasm_wasi02_cnet_wasi02_init(&adapter, &facade, &config, NULL), TURBOWASM_OK);
    check_equal(turbowasm_component_load_borrowed(&component, wide ? turbowasm_wasi02_fixture_socket_tcp_roundtrip64 :
        turbowasm_wasi02_fixture_socket_tcp_roundtrip, wide ? turbowasm_wasi02_fixture_socket_tcp_roundtrip64_size :
        turbowasm_wasi02_fixture_socket_tcp_roundtrip_size), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_component_instance_create_async(&instance, &component, &facade, NULL), TURBOWASM_OK);
    argument.kind = TURBOWASM_COMPONENT_HOST_U16; argument.as.u16 = address.as.ipv4.port;
    check_equal(turbowasm_component_call_create(&call, &instance, run, &argument, 1), TURBOWASM_OK);
    check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_YIELDED);
    for (unsigned i = 0; i < 200; ++i) {
        check_equal(sockets.tcp_accept(sockets.context, listener, &accepted, &inputs[1], &outputs[1], &socket_error), TURBOWASM_OK);
        if (socket_error != TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK) break;
        pump();
    }
    check_equal(socket_error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(streams.input_subscribe(streams.context, inputs[1], &subscriptions[1]), TURBOWASM_OK);
    for (unsigned i = 0; i < 200 && !is_ready(subscriptions[1]); ++i) {
        pump(); check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_YIELDED);
    }
    check_true(is_ready(subscriptions[1]));
    check_equal(streams.input_read(streams.context, inputs[1], 4, &data, &size, &error), TURBOWASM_OK);
    check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE); check_equal(size, (size_t)4); check_equal(data, "ping", 4);
    check_equal(streams.output_check_write(streams.context, outputs[1], &permit, &error), TURBOWASM_OK);
    check_equal(streams.output_write(streams.context, outputs[1], (const uint8_t *)"pong", 4, &error), TURBOWASM_OK);
    status = TURBOWASM_YIELDED;
    for (unsigned i = 0; i < 200 && status == TURBOWASM_YIELDED; ++i) {
        pump(); status = turbowasm_component_call_resume(&call, NULL);
    }
    check_equal(status, TURBOWASM_OK);
    check_equal(turbowasm_component_call_take_result(&call, &result), TURBOWASM_OK);
    check_equal(result.kind, TURBOWASM_COMPONENT_HOST_U32); check_equal(result.as.u32, UINT32_C(0x676e6f70));
    check_equal(turbowasm_component_host_value_destroy(&result), TURBOWASM_OK);
}
suite("native WASI TCP") {
    before_each() {
        turbowasm_wasi02_cnet_config config; native_io_backend_config native_config = {0};
        memset(&io, 0, sizeof(io)); memset(&adapter, 0, sizeof(adapter)); memset(&backend, 0, sizeof(backend));
        memset(&network, 0, sizeof(network)); memset(&listener, 0, sizeof(listener)); memset(&client, 0, sizeof(client));
        memset(&accepted, 0, sizeof(accepted)); memset(inputs, 0, sizeof(inputs)); memset(outputs, 0, sizeof(outputs));
        memset(subscriptions, 0, sizeof(subscriptions));
        turbowasm_wasi02_cnet_config_init(&config); config.socket_capacity = 4;
        config.receive_bytes = config.send_bytes = 1024; config.payload_bytes = 4096;
        config.allow_bind = config.allow_connect = config.allow_accept = true;
        native_config.kind = config.backend; native_config.endpoint_capacity = 8;
        native_config.request_capacity = 16; native_config.completion_batch_capacity = 8;
        check_equal(native_io_backend_init(&backend, &native_config), SALTS_OK);
        check_equal(turbowasm_wasi02_io_init(&io, NULL, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_cli_factory_set(&io, TURBOWASM_WASI02_IO_STDIN, stdin_create, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_providers(&io, &streams, &poll), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_init_external(&adapter, &io, &backend, &config, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_socket_provider(&adapter, &sockets), TURBOWASM_OK);
        check_equal(sockets.instance_network(sockets.context, &network), TURBOWASM_OK);
    }
    after_each() {
        bool complete = false;
        turbowasm_component_call_destroy(&call); turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        if (facade.impl) check_equal(turbowasm_wasi02_destroy(&facade), TURBOWASM_OK);
        for (unsigned i = 0; i < 2; ++i) {
            if (inputs[i].kind == TURBOWASM_VALUE_I64) streams.input_drop(streams.context, inputs[i]);
            if (outputs[i].kind == TURBOWASM_VALUE_I64) streams.output_drop(streams.context, outputs[i]);
            if (subscriptions[i].kind == TURBOWASM_VALUE_I64) check_equal(poll.drop(poll.context, subscriptions[i]), TURBOWASM_OK);
        }
        if (accepted.kind == TURBOWASM_VALUE_I64) check_equal(sockets.tcp_drop(sockets.context, accepted), TURBOWASM_OK);
        if (client.kind == TURBOWASM_VALUE_I64) check_equal(sockets.tcp_drop(sockets.context, client), TURBOWASM_OK);
        if (listener.kind == TURBOWASM_VALUE_I64) check_equal(sockets.tcp_drop(sockets.context, listener), TURBOWASM_OK);
        if (network.kind == TURBOWASM_VALUE_I64) check_equal(sockets.network_drop(sockets.context, network), TURBOWASM_OK);
        if (!adapter.impl) {
            if (io.impl) check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
            if (backend.impl) { check_equal(native_io_backend_close(&backend), SALTS_OK); check_equal(native_io_backend_destroy(&backend), SALTS_OK); }
            return;
        }
        check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
        for (unsigned i = 0; i < 200 && !complete; ++i) {
            pump(); check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK);
        }
        check_true(complete);
        check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
        check_equal(native_io_backend_close(&backend), SALTS_OK);
        check_equal(native_io_backend_destroy(&backend), SALTS_OK);
    }
    it("reuses an input subscription for multiple bounded TCP transfers") {
        connect_pair();
        for (unsigned round = 0; round < 3; ++round) {
            turbowasm_wasi02_stream_error error; uint64_t permit; const uint8_t *data; size_t size;
            check_false(is_ready(subscriptions[1]));
            check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
            check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE); check_equal(permit, 1024u);
            check_equal(streams.output_write(streams.context, outputs[0], (const uint8_t *)"hello", 5, &error), TURBOWASM_OK);
            check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE);
            check_equal(streams.output_flush(streams.context, outputs[0], &error), TURBOWASM_OK);
            check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
            check_equal(permit, 0u);
            for (unsigned i = 0; i < 200 && !is_ready(subscriptions[1]); ++i) pump();
            check_true(is_ready(subscriptions[1]));
            check_equal(streams.input_read(streams.context, inputs[1], 2, &data, &size, &error), TURBOWASM_OK);
            check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE); check_equal(size, (size_t)2); check_equal(data, "he", 2);
            check_true(is_ready(subscriptions[1]));
            check_equal(streams.input_read(streams.context, inputs[1], 3, &data, &size, &error), TURBOWASM_OK);
            check_equal(size, (size_t)3); check_equal(data, "llo", 3); check_false(is_ready(subscriptions[1]));
        }
    }
    it("round trips TCP through an async Component using memory32") { component_roundtrip(false); }
    it("round trips TCP through an async Component using memory64") { component_roundtrip(true); }
    it("connects portable IPv6 loopback endpoints") { connect_pair_family(TURBOWASM_WASI02_IP_ADDRESS_IPV6); }
    it("rejects exhausted socket capacity before native admission") {
        turbowasm_value owned[4] = {{0}}, extra = {0}; turbowasm_wasi02_socket_error error;
        for (unsigned i = 0; i < 4; ++i) {
            check_equal(sockets.tcp_create(sockets.context, TURBOWASM_WASI02_IP_ADDRESS_IPV4, &owned[i], &error), TURBOWASM_OK);
            check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
        }
        check_equal(sockets.tcp_create(sockets.context, TURBOWASM_WASI02_IP_ADDRESS_IPV4, &extra, &error), TURBOWASM_OK);
        check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT);
        for (unsigned i = 0; i < 4; ++i) check_equal(sockets.tcp_drop(sockets.context, owned[i]), TURBOWASM_OK);
    }
    it("keeps shutdown incomplete until live carriers and actual terminals retire") {
        turbowasm_wasi02_stream_error error; uint64_t permit; bool complete = true;
        connect_pair();
        check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
        check_equal(streams.output_write(streams.context, outputs[0], (const uint8_t *)"drain", 5, &error), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
        check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_CLOSED);
        check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK); check_false(complete);
        check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_INVALID_ARGUMENT);
    }
    it("keeps streams usable after dropping the parent socket") {
        turbowasm_wasi02_stream_error error; uint64_t permit; const uint8_t *data; size_t size;
        connect_pair(); check_equal(sockets.tcp_drop(sockets.context, client), TURBOWASM_OK); memset(&client, 0, sizeof(client));
        pump(); check_true(is_ready(subscriptions[0]));
        check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE);
        check_equal(streams.output_write(streams.context, outputs[0], (const uint8_t *)"kept", 4, &error), TURBOWASM_OK);
        for (unsigned i = 0; i < 200 && !is_ready(subscriptions[1]); ++i) pump();
        check_equal(streams.input_read(streams.context, inputs[1], 4, &data, &size, &error), TURBOWASM_OK);
        check_equal(size, (size_t)4); check_equal(data, "kept", 4);
    }
    it("receives peer EOF while preserving the opposite send direction") {
        turbowasm_wasi02_socket_error socket_error; turbowasm_wasi02_stream_error error;
        uint64_t permit; const uint8_t *data; size_t size; turbowasm_value reverse = {0};
        connect_pair();
        check_equal(sockets.tcp_shutdown(sockets.context, client, TURBOWASM_WASI02_TCP_SHUTDOWN_SEND, &socket_error), TURBOWASM_OK);
        check_equal(socket_error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
        for (unsigned i = 0; i < 200 && !is_ready(subscriptions[1]); ++i) pump();
        check_true(is_ready(subscriptions[1]));
        check_equal(streams.input_read(streams.context, inputs[1], 1, &data, &size, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_CLOSED);
        check_equal(streams.input_subscribe(streams.context, inputs[0], &reverse), TURBOWASM_OK);
        check_equal(streams.output_check_write(streams.context, outputs[1], &permit, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE); check_equal(permit, 1024u);
        check_equal(streams.output_write(streams.context, outputs[1], (const uint8_t *)"reply", 5, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE);
        for (unsigned i = 0; i < 200 && !is_ready(reverse); ++i) pump();
        check_equal(poll.drop(poll.context, reverse), TURBOWASM_OK);
        check_equal(streams.input_read(streams.context, inputs[0], 5, &data, &size, &error), TURBOWASM_OK);
        check_equal(error.kind, TURBOWASM_WASI02_STREAM_ERROR_NONE); check_equal(size, (size_t)5); check_equal(data, "reply", 5);
    }
    it("resumes a retained async WASI Component from real TCP readiness") {
        turbowasm_wasi02_config config = {0}; turbowasm_wasi02_stream_error error;
        uint64_t permit; turbowasm_host_wait wait; turbowasm_component_host_value result = {0};
        turbowasm_name run = {(const uint8_t *)"run", 3};
        connect_pair(); config.pollable_capacity = 8; config.stream_resource_capacity = 8;
        config.sockets = sockets; config.socket_network_resource_capacity = 2; config.tcp_socket_resource_capacity = 4;
        check_equal(turbowasm_wasi02_cnet_wasi02_init(&adapter, &facade, &config, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_load_borrowed(&component, turbowasm_wasi02_fixture_stream_poll_block,
            turbowasm_wasi02_fixture_stream_poll_block_size), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_component_instance_create_async(&instance, &component, &facade, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_create(&call, &instance, run, NULL, 0), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_YIELDED);
        check_true(turbowasm_component_call_pending_host_wait(&call, &wait));
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(turbowasm_wasi02_destroy(&facade), TURBOWASM_INVALID_ARGUMENT);
        check_equal(streams.output_check_write(streams.context, outputs[0], &permit, &error), TURBOWASM_OK);
        check_equal(streams.output_write(streams.context, outputs[0], (const uint8_t *)"wake", 4, &error), TURBOWASM_OK);
        for (unsigned i = 0; i < 200 && !is_ready(subscriptions[1]); ++i) pump();
        check_true(is_ready(subscriptions[1]));
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&call, &result), TURBOWASM_OK);
        check_equal(result.kind, TURBOWASM_COMPONENT_HOST_U32); check_equal(result.as.u32, 1u);
        check_equal(turbowasm_component_host_value_destroy(&result), TURBOWASM_OK);
        turbowasm_component_call_destroy(&call);
        check_equal(turbowasm_wasi02_destroy(&facade), TURBOWASM_OK);
    }
}
