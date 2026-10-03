#include "../src/wasi02_cnet.h"

#include <salts/error_codes.h>
#include <salts/native_io.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
    address.as.ipv4.port = 0u;
    address.as.ipv4.address[0] = 127u;
    address.as.ipv4.address[3] = 1u;
    return address;
}

static native_io_completion observe_request(
    native_io_backend *backend,
    native_io_request expected) {
    native_io_completion completions[TEST_BATCH];
    size_t count = 0u;
    size_t attempt;

    memset(completions, 0, sizeof(completions));
    for (attempt = 0u; attempt < 8u; ++attempt) {
        int status = native_io_backend_observe(
            backend, completions, TEST_BATCH, 250u, &count);
        if (status == SALTS_ETIMEDOUT)
            continue;
        assert(status == SALTS_OK);
        if (count == 0u)
            continue;

        for (size_t i = 0u; i < count; ++i) {
            if (completions[i].request.slot == expected.slot &&
                completions[i].request.generation ==
                    expected.generation)
                return completions[i];
        }
    }

    assert(!"timed out waiting for listener accept terminal completion");
    return (native_io_completion){0};
}

static void test_listener_accept_cancel_drains_before_slot_reuse(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config = backend_config();
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_socket_provider provider = {0};
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_wasi02_ip_socket_address bind = loopback_v4();
    turbowasm_value network = {0};
    turbowasm_value socket = {0};
    turbowasm_value replacement = {0};
    native_io_request first_request = {0};
    native_io_request repeated_request = {0};
    native_io_completion completion = {0};
    bool ready = false;
    bool consumed = false;
    size_t events = SIZE_MAX;

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter, &config, &backend,
               &client) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter, &provider) == TURBOWASM_OK);

    assert(provider.instance_network(
               provider.context, &network) == TURBOWASM_OK);
    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &socket, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    /* Unbound and synchronously completed bind/listen transitions are ready. */
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter, socket, &ready) == TURBOWASM_OK);
    assert(ready);

    assert(provider.tcp_start_bind(
               provider.context, socket, network,
               &bind, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_finish_bind(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(provider.tcp_start_listen(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter, socket, &ready) == TURBOWASM_OK);
    assert(ready);

    assert(provider.tcp_finish_listen(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(turbowasm_wasi02_cnet_socket_poll_ready(
               &adapter, socket, &ready) == TURBOWASM_OK);
    assert(!ready);

    /*
     * One listening socket owns one shared generation-safe accept request.
     * Repeated preparation never submits a duplicate accept.
     */
    assert(turbowasm_wasi02_cnet_socket_poll_prepare(
               &adapter, socket, &ready,
               &first_request) == TURBOWASM_OK);
    assert(!ready);
    assert(native_io_request_valid(first_request));

    assert(turbowasm_wasi02_cnet_socket_poll_prepare(
               &adapter, socket, &ready,
               &repeated_request) == TURBOWASM_OK);
    assert(!ready);
    assert(repeated_request.slot == first_request.slot);
    assert(repeated_request.generation ==
           first_request.generation);

    /*
     * The external CNet client and listener progress are distinct consumers
     * of the same runtime-owned NativeIO backend. Stopping the client does not
     * make a listener-owned terminal cancellation unroutable.
     */
    assert(turbowasm_wasi02_cnet_stop_external(
               &adapter) == SALTS_OK);

    /*
     * Dropping the WASI socket cancels accept but consumes the guest identity
     * immediately. The physical slot/generation is retained until the
     * authoritative terminal completion is observed.
     */
    assert(provider.tcp_drop(
               provider.context, socket) == TURBOWASM_OK);
    assert(provider.tcp_drop(
               provider.context, socket) == TURBOWASM_TRAPPED);

    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &replacement, &error) == TURBOWASM_OK);
    assert(error ==
           TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT);

    assert(provider.network_drop(
               provider.context, network) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);

    completion = observe_request(&backend, first_request);
    assert(completion.kind == NATIVE_IO_COMPLETION_CANCELLED ||
           completion.status == SALTS_ECANCELED);

    events = SIZE_MAX;
    assert(turbowasm_wasi02_cnet_route_external_completion(
               &adapter, &completion, &consumed,
               &events) == SALTS_OK);
    assert(consumed);
    assert(events == 0u);

    /*
     * Only after terminal drain may the bounded physical slot be reused, and
     * its generation must change so the consumed guest rep stays stale.
     */
    replacement = (turbowasm_value){0};
    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &replacement, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(replacement.kind == TURBOWASM_VALUE_I64);
    assert(replacement.as.i64 != socket.as.i64);
    assert(provider.tcp_drop(
               provider.context, replacement) == TURBOWASM_OK);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

int main(void) {
    test_listener_accept_cancel_drains_before_slot_reuse();
    return 0;
}
