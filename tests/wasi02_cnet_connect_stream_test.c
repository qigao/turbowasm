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
    config.endpoint_capacity = 8u;
    config.request_capacity = 16u;
    config.completion_batch_capacity = TEST_BATCH;
    return config;
}

static cnet_client_config client_config(void) {
    cnet_client_config config;
    memset(&config, 0, sizeof(config));
    config.backend = test_backend();
    config.connection_capacity = 1u;
    config.command_capacity = 8u;
    config.request_capacity = 8u;
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
    config.socket_capacity = 1u;
    config.default_listen_backlog = 8u;
    return config;
}

static test_socket open_loopback_server(uint16_t *out_port) {
    struct sockaddr_in address;
#if defined(_WIN32)
    int address_size = (int)sizeof(address);
#else
    socklen_t address_size = (socklen_t)sizeof(address);
#endif
    test_socket server;

    assert(out_port != NULL);
    *out_port = 0u;

    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(server != TEST_INVALID_SOCKET);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0u;
    assert(bind(
               server,
               (const struct sockaddr *)&address,
               (int)sizeof(address)) == 0);
    assert(listen(server, 8) == 0);
    assert(getsockname(
               server,
               (struct sockaddr *)&address,
               &address_size) == 0);
    *out_port = ntohs(address.sin_port);
    assert(*out_port != 0u);
    return server;
}

static turbowasm_wasi02_ip_socket_address loopback_v4(
    uint16_t port) {
    turbowasm_wasi02_ip_socket_address address;
    memset(&address, 0, sizeof(address));
    address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    address.as.ipv4.port = port;
    address.as.ipv4.address[0] = 127u;
    address.as.ipv4.address[3] = 1u;
    return address;
}

static void route_one_connect_completion(
    native_io_backend *backend,
    turbowasm_wasi02_cnet *adapter,
    native_io_request expected) {
    native_io_completion completions[TEST_BATCH];
    size_t attempt;

    for (attempt = 0u; attempt < 12u; ++attempt) {
        size_t count = 0u;
        size_t i;
        int status;

        memset(completions, 0, sizeof(completions));
        status = native_io_backend_observe(
            backend,
            completions,
            TEST_BATCH,
            250u,
            &count);
        if (status == SALTS_ETIMEDOUT)
            continue;
        assert(status == SALTS_OK);

        for (i = 0u; i < count; ++i) {
            bool consumed = false;
            size_t events = 0u;

            if (completions[i].request.slot != expected.slot ||
                completions[i].request.generation !=
                    expected.generation)
                continue;

            assert(turbowasm_wasi02_cnet_route_external_completion(
                       adapter,
                       &completions[i],
                       &consumed,
                       &events) == SALTS_OK);
            assert(consumed);
            return;
        }
    }

    assert(!"timed out waiting for CNet connect completion");
}

static void drain_connection_close(
    native_io_backend *backend,
    turbowasm_wasi02_cnet *adapter) {
    size_t attempt;

    for (attempt = 0u; attempt < 24u; ++attempt) {
        native_io_completion completions[TEST_BATCH];
        size_t events = 0u;
        size_t count = 0u;
        size_t i;
        int status;

        status = turbowasm_wasi02_cnet_advance_external(
            adapter, &events);
        if (status != SALTS_OK &&
            status != SALTS_ESHUTDOWN)
            assert(!"unexpected CNet external advance failure");

        memset(completions, 0, sizeof(completions));
        status = native_io_backend_observe(
            backend,
            completions,
            TEST_BATCH,
            100u,
            &count);
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

    assert(!"timed out draining CNet connection close");
}

static void test_connect_publishes_stream_leases(void) {
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
    turbowasm_wasi02_ip_socket_address remote;
    turbowasm_wasi02_ip_socket_address local = {0};
    turbowasm_wasi02_ip_socket_address observed_remote = {0};
    turbowasm_value network = {0};
    turbowasm_value socket_rep = {0};
    turbowasm_value input_rep = {0};
    turbowasm_value output_rep = {0};
    native_io_request request = {0};
    uint32_t input_resource = 0u;
    uint32_t output_resource = 0u;
    uint16_t server_port = 0u;
    test_socket server = TEST_INVALID_SOCKET;
    bool ready = true;

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter,
               &config,
               &backend,
               &client) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter,
               &sockets) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_stream_provider(
               &adapter,
               &stream_provider) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_init(
               &streams,
               &stream_provider,
               4u) == TURBOWASM_OK);

    server = open_loopback_server(&server_port);
    remote = loopback_v4(server_port);

    assert(sockets.instance_network(
               sockets.context, &network) == TURBOWASM_OK);
    assert(sockets.tcp_create(
               sockets.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &socket_rep,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(sockets.tcp_start_connect != NULL);
    assert(sockets.tcp_finish_connect != NULL);
    assert(sockets.tcp_remote_address != NULL);
    assert(sockets.tcp_shutdown != NULL);

    assert(sockets.tcp_start_connect(
               sockets.context,
               socket_rep,
               network,
               &remote,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter,
               socket_rep,
               &ready) == TURBOWASM_OK);
    assert(!ready);
    assert(turbowasm_wasi02_cnet_socket_poll_prepare(
               &adapter,
               socket_rep,
               &ready,
               &request) == TURBOWASM_OK);
    assert(!ready);
    assert(native_io_request_valid(request));

    route_one_connect_completion(&backend, &adapter, request);

    ready = false;
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter,
               socket_rep,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(sockets.tcp_finish_connect(
               sockets.context,
               socket_rep,
               &input_rep,
               &output_rep,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(input_rep.kind == TURBOWASM_VALUE_I64);
    assert(output_rep.kind == TURBOWASM_VALUE_I64);
    assert(input_rep.as.i64 == output_rep.as.i64);

    assert(turbowasm_wasi02_input_stream_new(
               &streams,
               input_rep,
               &input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_output_stream_new(
               &streams,
               output_rep,
               &output_resource) == TURBOWASM_OK);

    assert(sockets.tcp_local_address(
               sockets.context,
               socket_rep,
               &local,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(local.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(local.as.ipv4.port != 0u);

    assert(sockets.tcp_remote_address(
               sockets.context,
               socket_rep,
               &observed_remote,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(observed_remote.family ==
           TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(observed_remote.as.ipv4.port == server_port);

    assert(sockets.tcp_shutdown(
               sockets.context,
               socket_rep,
               TURBOWASM_WASI02_TCP_SHUTDOWN_SEND,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    /*
     * Socket drop starts connection close, but the connection slot remains
     * retained by the two already-returned stream resources.
     */
    assert(sockets.tcp_drop(
               sockets.context,
               socket_rep) == TURBOWASM_OK);
    assert(sockets.network_drop(
               sockets.context,
               network) == TURBOWASM_OK);

    close_test_socket(server);
    server = TEST_INVALID_SOCKET;
    drain_connection_close(&backend, &adapter);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams,
               input_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams,
               output_resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

int main(void) {
    test_connect_publishes_stream_leases();
    return 0;
}
