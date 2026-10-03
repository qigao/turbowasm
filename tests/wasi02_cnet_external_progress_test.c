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

static native_io_backend_config backend_config(size_t request_capacity) {
    native_io_backend_config config = {0};
    config.kind = test_backend();
    config.endpoint_capacity = 8u;
    config.request_capacity = request_capacity;
    config.completion_batch_capacity =
        request_capacity < TEST_BATCH
            ? request_capacity
            : TEST_BATCH;
    return config;
}

static cnet_client_config client_config(void) {
    cnet_client_config config;
    memset(&config, 0, sizeof(config));
    config.backend = test_backend();
    config.connection_capacity = 2u;
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
    config.socket_capacity = 2u;
    config.default_listen_backlog = 8u;
    return config;
}

static void test_external_progress_borrows_backend(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config =
        backend_config(16u);
    native_io_backend_config observed = {0};
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_socket_provider provider = {0};
    native_io_completion unrelated = {0};
    bool consumed = true;
    size_t events = SIZE_MAX;
    uint32_t timeout_ms = UINT32_MAX;

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter, &config, &backend,
               &client) == TURBOWASM_OK);

    /*
     * External progress now qualifies the client-connect lifecycle. Accept
     * remains a separate listener-child slice and subscribe is exposed only
     * after the shared W4 poll registry is attached.
     */
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter, &provider) == TURBOWASM_OK);
    assert(provider.tcp_start_connect != NULL);
    assert(provider.tcp_finish_connect != NULL);
    assert(provider.tcp_accept == NULL);
    assert(provider.tcp_remote_address != NULL);
    assert(provider.tcp_subscribe == NULL);
    assert(provider.tcp_shutdown != NULL);

    events = SIZE_MAX;
    assert(turbowasm_wasi02_cnet_advance_external(
               &adapter, &events) == SALTS_OK);
    assert(events == 0u);

    assert(turbowasm_wasi02_cnet_external_timeout(
               &adapter, 50u, &timeout_ms) == SALTS_OK);
    assert(timeout_ms <= 50u);

    /*
     * The runtime may observe completions for other consumers of this backend.
     * CNet must decline unrelated completions without consuming them.
     */
    unrelated.user_data = 1u;
    events = SIZE_MAX;
    assert(turbowasm_wasi02_cnet_route_external_completion(
               &adapter, &unrelated, &consumed,
               &events) == SALTS_OK);
    assert(!consumed);
    assert(events == 0u);

    assert(turbowasm_wasi02_cnet_stop_external(
               &adapter) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_advance_external(
               &adapter, &events) == SALTS_ESHUTDOWN);
    assert(turbowasm_wasi02_cnet_stop_external(
               &adapter) == SALTS_OK);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);

    /*
     * CNet borrowed the runtime-owned backend. Adapter destruction therefore
     * leaves the backend live and independently destroyable by its owner.
     */
    assert(native_io_backend_get_config(
               &backend, &observed));
    assert(observed.kind == test_backend());
    assert(observed.request_capacity == 16u);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

static void test_external_progress_rejects_underprovisioned_backend(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config =
        backend_config(2u);
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet adapter = {0};

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter, &config, &backend,
               &client) == TURBOWASM_INVALID_ARGUMENT);
    assert(adapter.impl == NULL);

    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

static void test_external_progress_requires_matching_bounds(void) {
    native_io_backend backend = {0};
    native_io_backend_config native_config =
        backend_config(16u);
    cnet_client_config client = client_config();
    turbowasm_wasi02_cnet_config config = adapter_config();
    turbowasm_wasi02_cnet adapter = {0};

    assert(native_io_backend_init(
               &backend, &native_config) == SALTS_OK);

    client.connection_capacity = 1u;
    assert(turbowasm_wasi02_cnet_init_external(
               &adapter, &config, &backend,
               &client) == TURBOWASM_INVALID_ARGUMENT);
    assert(adapter.impl == NULL);

    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

int main(void) {
    test_external_progress_borrows_backend();
    test_external_progress_rejects_underprovisioned_backend();
    test_external_progress_requires_matching_bounds();
    return 0;
}
