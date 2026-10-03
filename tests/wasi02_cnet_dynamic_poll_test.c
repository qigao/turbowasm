#include "../src/wasi02_cnet_poll_native_io.h"
#include "../src/wasi02_poll.h"

#include <turbowasm/turbowasm.h>

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

#define WASM_HEADER 0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

enum {
    TEST_BATCH = 8u
};

typedef struct host_probe {
    turbowasm_wasi02_poll *poll;
    uint32_t resource;
    uint32_t entries;
} host_probe;

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
    config.connect_timeout_ms = 1000u;
    config.read_timeout_ms = 1000u;
    config.write_timeout_ms = 1000u;
    return config;
}

static turbowasm_wasi02_cnet_config adapter_config(void) {
    turbowasm_wasi02_cnet_config config = {0};
    config.backend = test_backend();
    config.socket_capacity = 1u;
    config.default_listen_backlog = 8u;
    return config;
}

static turbowasm_wasi02_ip_socket_address loopback_v4(void) {
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

    ++probe->entries;
    status = turbowasm_wasi02_poll_many(
        probe->poll,
        &probe->resource,
        1u,
        call,
        &ready_indices,
        trap);
    if (status != TURBOWASM_OK)
        return status;

    assert(ready_indices.kind ==
           TURBOWASM_COMPONENT_TYPE_LIST);
    assert(ready_indices.as.list.count == 1u);
    assert(ready_indices.as.list.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_U32);
    assert(ready_indices.as.list.items[0].as.u32 == 0u);

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = 1;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    turbowasm_component_value_destroy(&ready_indices);
    return TURBOWASM_OK;
}

static const uint8_t module_bytes[] = {
    WASM_HEADER,
    0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
    0x02,0x0d,0x01,
    0x04,'h','o','s','t',
    0x04,'p','o','l','l',
    0x00,0x00,
    0x03,0x02,0x01,0x00,
    0x0a,0x06,0x01,0x04,0x00,0x10,0x00,0x0b
};

static void route_until_accept_ready(
    native_io_backend *backend,
    turbowasm_wasi02_cnet *cnet,
    turbowasm_wasi02_native_io_poll *native_poll) {
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
            size_t events = SIZE_MAX;

            assert(turbowasm_wasi02_cnet_route_external_completion(
                       cnet,
                       &completions[i],
                       &consumed,
                       &events) == SALTS_OK);
            if (!consumed)
                continue;

            assert(events == 0u);
            assert(turbowasm_wasi02_native_io_poll_complete(
                       native_poll,
                       &completions[i]) == TURBOWASM_OK);
            return;
        }
    }

    assert(!"timed out waiting for external listener accept completion");
}

static void test_dynamic_socket_poll_reuses_one_w4_namespace(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config = backend_config();
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet cnet = {0};
    turbowasm_wasi02_native_io_poll native_poll = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_socket_provider socket_provider = {0};
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_wasi02_ip_socket_address bind = loopback_v4();
    turbowasm_wasi02_ip_socket_address local = {0};
    turbowasm_value network = {0};
    turbowasm_value socket = {0};
    turbowasm_value poll_rep = {0};
    host_probe host = {0};
    turbowasm_module module = {0};
    turbowasm_linker linker = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    const turbowasm_value *result;
    test_socket peer = TEST_INVALID_SOCKET;
    bool ready = false;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_native_io_poll_init(
               &native_poll,
               &backend,
               4u,
               2u,
               4u,
               NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_provider(
               &native_poll,
               &poll_provider) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll,
               &poll_provider,
               4u) == TURBOWASM_OK);

    assert(turbowasm_wasi02_cnet_init_external(
               &cnet,
               &config,
               &backend,
               &client) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_attach_native_io_poll(
               &cnet,
               &native_poll) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &cnet,
               &socket_provider) == TURBOWASM_OK);
    assert(socket_provider.tcp_subscribe != NULL);

    assert(socket_provider.instance_network(
               socket_provider.context,
               &network) == TURBOWASM_OK);
    assert(socket_provider.tcp_create(
               socket_provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(socket_provider.tcp_subscribe(
               socket_provider.context,
               socket,
               &poll_rep) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll,
               poll_rep,
               &host.resource) == TURBOWASM_OK);
    host.poll = &poll;

    /* The same owned pollable follows the socket state dynamically. */
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(socket_provider.tcp_start_bind(
               socket_provider.context,
               socket,
               network,
               &bind,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(socket_provider.tcp_finish_bind(
               socket_provider.context,
               socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(socket_provider.tcp_start_listen(
               socket_provider.context,
               socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    ready = false;
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(socket_provider.tcp_finish_listen(
               socket_provider.context,
               socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    ready = true;
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resource,
               &ready) == TURBOWASM_OK);
    assert(!ready);

    assert(socket_provider.tcp_local_address(
               socket_provider.context,
               socket,
               &local,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(local.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(local.as.ipv4.port != 0u);

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
    assert(host.entries == 1u);

    peer = connect_raw_peer(local.as.ipv4.port);
    route_until_accept_ready(&backend, &cnet, &native_poll);

    assert(turbowasm_execution_resume(
               &execution,
               NULL) == TURBOWASM_OK);
    assert(host.entries == 1u);
    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 1);

    ready = false;
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    /*
     * Parent socket drop consumes the guest socket but not the owned pollable.
     * The pollable observes the closed state as ready until its own drop
     * releases the final CNet slot lease.
     */
    assert(socket_provider.tcp_drop(
               socket_provider.context,
               socket) == TURBOWASM_OK);
    close_test_socket(peer);
    peer = TEST_INVALID_SOCKET;

    ready = false;
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resource,
               &ready) == TURBOWASM_OK);
    assert(ready);

    assert(socket_provider.network_drop(
               socket_provider.context,
               network) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &cnet) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               host.resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &cnet) == TURBOWASM_OK);

    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_destroy(
               &native_poll) == TURBOWASM_OK);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

int main(void) {
    test_dynamic_socket_poll_reuses_one_w4_namespace();
    return 0;
}
