#include "../src/wasi02_cnet.h"
#include "../src/wasi02_streams.h"

#include <salts/error_codes.h>
#include <salts/native_io.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET test_socket;
#define TEST_INVALID_SOCKET INVALID_SOCKET
static void close_test_socket(test_socket socket_value) {
    if (socket_value != INVALID_SOCKET)
        assert(closesocket(socket_value) == 0);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int test_socket;
#define TEST_INVALID_SOCKET (-1)
static void close_test_socket(test_socket socket_value) {
    if (socket_value >= 0)
        assert(close(socket_value) == 0);
}
#endif

enum {
    TEST_BATCH = 8u
};

static native_io_backend_kind test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static native_io_backend_config backend_config(void) {
    native_io_backend_config config = {0};
    config.kind = test_backend();
    config.endpoint_capacity = 12u;
    config.request_capacity = 24u;
    config.completion_batch_capacity = TEST_BATCH;
    return config;
}

static cnet_client_config client_config(void) {
    cnet_client_config config;
    memset(&config, 0, sizeof(config));
    config.backend = test_backend();
    config.connection_capacity = 2u;
    config.command_capacity = 8u;
    config.request_capacity = 12u;
    config.completion_batch_capacity = TEST_BATCH;
    config.event_capacity = 8u;
    config.max_send_bytes = 4096u;
    config.receive_buffer_bytes = 4096u;
    config.connect_timeout_ms = 2000u;
    config.read_timeout_ms = 2000u;
    config.write_timeout_ms = 2000u;
    return config;
}

static turbowasm_wasi02_cnet_config adapter_config(void) {
    turbowasm_wasi02_cnet_config config = {0};
    config.backend = test_backend();
    config.socket_capacity = 2u;
    config.default_listen_backlog = 8u;
    return config;
}

static turbowasm_wasi02_ip_socket_address loopback_bind(void) {
    turbowasm_wasi02_ip_socket_address address;
    memset(&address, 0, sizeof(address));
    address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    address.as.ipv4.address[0] = 127u;
    address.as.ipv4.address[3] = 1u;
    return address;
}

static test_socket connect_raw_peer(uint16_t port) {
    struct sockaddr_in address;
    test_socket peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    assert(peer != TEST_INVALID_SOCKET);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    assert(connect(
               peer,
               (const struct sockaddr *)&address,
               (int)sizeof(address)) == 0);
    return peer;
}

static native_io_completion observe_request(
    native_io_backend *backend,
    native_io_request expected) {
    native_io_completion completions[TEST_BATCH];
    size_t attempt;

    for (attempt = 0u; attempt < 12u; ++attempt) {
        size_t count = 0u;
        size_t i;
        int status;

        memset(completions, 0, sizeof(completions));
        status = native_io_backend_observe(
            backend, completions, TEST_BATCH,
            250u, &count);
        if (status == SALTS_ETIMEDOUT)
            continue;
        assert(status == SALTS_OK);

        for (i = 0u; i < count; ++i) {
            if (completions[i].request.slot ==
                    expected.slot &&
                completions[i].request.generation ==
                    expected.generation)
                return completions[i];
        }
    }

    assert(!"timed out waiting for accept completion");
    return (native_io_completion){0};
}

static void drain_client(
    native_io_backend *backend,
    turbowasm_wasi02_cnet *adapter) {
    size_t attempt;

    for (attempt = 0u; attempt < 24u; ++attempt) {
        native_io_completion completions[TEST_BATCH];
        size_t count = 0u;
        size_t events = 0u;
        size_t i;
        int status;

        status = turbowasm_wasi02_cnet_advance_external(
            adapter, &events);
        if (status != SALTS_OK &&
            status != SALTS_ESHUTDOWN)
            assert(!"unexpected CNet advance failure");

        memset(completions, 0, sizeof(completions));
        status = native_io_backend_observe(
            backend, completions, TEST_BATCH,
            100u, &count);
        if (status != SALTS_OK &&
            status != SALTS_ETIMEDOUT)
            assert(!"unexpected NativeIO observe failure");

        if (status == SALTS_OK) {
            for (i = 0u; i < count; ++i) {
                bool consumed = false;
                size_t routed_events = 0u;
                assert(turbowasm_wasi02_cnet_route_external_completion(
                           adapter,
                           &completions[i],
                           &consumed,
                           &routed_events) == SALTS_OK);
            }
        }

        status = turbowasm_wasi02_cnet_stop_external(adapter);
        if (status == SALTS_OK)
            return;
        assert(status == SALTS_EBUSY);
    }

    assert(!"timed out draining accepted CNet child");
}

static void test_accept_publishes_child_stream_leases(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config = backend_config();
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_socket_provider sockets = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_wasi02_ip_socket_address bind = loopback_bind();
    turbowasm_wasi02_ip_socket_address local = {0};
    turbowasm_wasi02_ip_socket_address remote = {0};
    turbowasm_value network = {0};
    turbowasm_value listener = {0};
    turbowasm_value child = {0};
    turbowasm_value input_rep = {0};
    turbowasm_value output_rep = {0};
    native_io_request accept_request = {0};
    native_io_completion completion = {0};
    uint32_t input_resource = 0u;
    uint32_t output_resource = 0u;
    test_socket peer = TEST_INVALID_SOCKET;
    bool ready = false;
    bool consumed = false;
    size_t events = SIZE_MAX;

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter, &config,
               &backend, &client) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter, &sockets) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_stream_provider(
               &adapter, &stream_provider) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider,
               4u) == TURBOWASM_OK);

    assert(sockets.tcp_accept != NULL);
    assert(sockets.instance_network(
               sockets.context, &network) == TURBOWASM_OK);
    assert(sockets.tcp_create(
               sockets.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &listener, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(sockets.tcp_start_bind(
               sockets.context,
               listener, network,
               &bind, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(sockets.tcp_finish_bind(
               sockets.context,
               listener, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(sockets.tcp_start_listen(
               sockets.context,
               listener, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(sockets.tcp_finish_listen(
               sockets.context,
               listener, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(sockets.tcp_local_address(
               sockets.context,
               listener, &local,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(local.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(local.as.ipv4.port != 0u);

    assert(turbowasm_wasi02_cnet_socket_poll_prepare(
               &adapter,
               listener,
               &ready,
               &accept_request) == TURBOWASM_OK);
    assert(!ready);
    assert(native_io_request_valid(accept_request));

    peer = connect_raw_peer(local.as.ipv4.port);
    completion = observe_request(&backend, accept_request);

    assert(turbowasm_wasi02_cnet_route_external_completion(
               &adapter,
               &completion,
               &consumed,
               &events) == SALTS_OK);
    assert(consumed);
    assert(events == 0u);

    ready = false;
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter,
               listener,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(sockets.tcp_accept(
               sockets.context,
               listener,
               &child,
               &input_rep,
               &output_rep,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(child.kind == TURBOWASM_VALUE_I64);
    assert(input_rep.kind == TURBOWASM_VALUE_I64);
    assert(output_rep.kind == TURBOWASM_VALUE_I64);
    assert(input_rep.as.i64 == child.as.i64);
    assert(output_rep.as.i64 == child.as.i64);

    assert(turbowasm_wasi02_input_stream_new(
               &streams,
               input_rep,
               &input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_output_stream_new(
               &streams,
               output_rep,
               &output_resource) == TURBOWASM_OK);

    /*
     * Consuming one accepted child resets the listener readiness. A subsequent
     * poll must arm a fresh accept instead of seeing sticky readiness.
     */
    ready = true;
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter,
               listener,
               &ready) == TURBOWASM_OK);
    assert(!ready);

    assert(sockets.tcp_remote_address(
               sockets.context,
               child,
               &remote,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(remote.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(remote.as.ipv4.port != 0u);

    assert(sockets.tcp_drop(
               sockets.context, child) == TURBOWASM_OK);
    close_test_socket(peer);
    peer = TEST_INVALID_SOCKET;

    /*
     * Listener has no active accept at this point, so it can detach from the
     * shared NativeIO owner synchronously.
     */
    assert(sockets.tcp_drop(
               sockets.context, listener) == TURBOWASM_OK);
    assert(sockets.network_drop(
               sockets.context, network) == TURBOWASM_OK);

    drain_client(&backend, &adapter);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output_resource) == TURBOWASM_OK);

    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

int main(void) {
    test_accept_publishes_child_stream_leases();
    return 0;
}
