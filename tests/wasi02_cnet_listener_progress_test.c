#include <turbowasm/wasi02_cnet.h>
#include <salts/error_codes.h>
#include <tinytest.h>

static void listener_cancel_drain(bool shutdown) {
    native_io_backend backend = {0};
    native_io_backend_config native = {0};
    native_io_backend_stats armed = {0}, repeated = {0};
    turbowasm_wasi02_io io = {0};
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_cnet_config config;
    turbowasm_wasi02_socket_provider sockets = {0};
    turbowasm_wasi02_stream_provider streams = {0};
    turbowasm_wasi02_poll_provider poll = {0};
    turbowasm_wasi02_socket_error error;
    turbowasm_wasi02_ip_socket_address address = {0};
    turbowasm_value network = {0}, socket = {0}, replacement = {0}, subscription = {0};
    native_io_completion completions[8];
    bool ready = false, complete = false, consumed = false;
    size_t events = 0, count = 0;

    turbowasm_wasi02_cnet_config_init(&config);
    config.socket_capacity = 1;
    config.allow_bind = config.allow_accept = true;
    native.kind = config.backend;
    native.endpoint_capacity = 8; native.request_capacity = 16; native.completion_batch_capacity = 8;
    check_equal(native_io_backend_init(&backend, &native), SALTS_OK);
    check_equal(turbowasm_wasi02_io_init(&io, NULL, NULL), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_init_external(&adapter, &io, &backend, &config, NULL), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_socket_provider(&adapter, &sockets), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_providers(&io, &streams, &poll), TURBOWASM_OK);
    check_equal(sockets.instance_network(sockets.context, &network), TURBOWASM_OK);
    check_equal(sockets.tcp_create(sockets.context, TURBOWASM_WASI02_IP_ADDRESS_IPV4, &socket, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_subscribe(sockets.context, socket, &subscription), TURBOWASM_OK);
    check_equal(poll.ready(poll.context, subscription, &ready), TURBOWASM_OK);
    check_true(ready);
    address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    address.as.ipv4.address[0] = 127; address.as.ipv4.address[3] = 1;
    check_equal(sockets.tcp_start_bind(sockets.context, socket, network, &address, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_finish_bind(sockets.context, socket, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(sockets.tcp_start_listen(sockets.context, socket, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    /* Completed listen is ready until finish-listen consumes the transition. */
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_equal(poll.ready(poll.context, subscription, &ready), TURBOWASM_OK);
    check_true(ready);
    check_true(native_io_backend_get_stats(&backend, &armed));
    check_equal(armed.submitted, (uint64_t)0);
    check_equal(sockets.tcp_finish_listen(sockets.context, socket, &error), TURBOWASM_OK);
    check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    check_equal(poll.ready(poll.context, subscription, &ready), TURBOWASM_OK);
    check_false(ready);
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_true(native_io_backend_get_stats(&backend, &armed));
    check_equal(armed.active_requests, (size_t)1);
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_true(native_io_backend_get_stats(&backend, &repeated));
    check_equal(repeated.submitted, armed.submitted);
    check_equal(repeated.active_requests, armed.active_requests);

    if (shutdown) check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
    check_equal(sockets.tcp_drop(sockets.context, socket), TURBOWASM_OK);
    check_equal(sockets.tcp_drop(sockets.context, socket), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    check_equal(poll.ready(poll.context, subscription, &ready), TURBOWASM_OK);
    check_true(ready);
    check_equal(poll.drop(poll.context, subscription), TURBOWASM_OK);
    if (!shutdown) {
        check_equal(sockets.tcp_create(sockets.context, address.family, &replacement, &error), TURBOWASM_OK);
        check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT);
    }
    check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_INVALID_ARGUMENT);
    if (shutdown) {
        check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK);
        check_false(complete);
    }
    /* The sole request must reach a routed cancellation before physical slot reuse. */
    for (unsigned attempt = 0; attempt < 8 && !count; ++attempt) {
        int rc = native_io_backend_observe(&backend, completions, 8, 250, &count);
        check_true(rc == SALTS_OK || rc == SALTS_ETIMEDOUT);
    }
    check_equal(count, (size_t)1);
    check_true(completions[0].kind == NATIVE_IO_COMPLETION_CANCELLED || completions[0].status == SALTS_ECANCELED);
    check_equal(turbowasm_wasi02_cnet_route_completion(&adapter, &completions[0], &consumed), TURBOWASM_OK);
    check_true(consumed);
    check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
    if (!shutdown) {
        check_equal(sockets.tcp_create(sockets.context, address.family, &replacement, &error), TURBOWASM_OK);
        check_equal(error, TURBOWASM_WASI02_SOCKET_ERROR_NONE);
        check_not_equal(replacement.as.i64, socket.as.i64);
        check_equal(sockets.tcp_drop(sockets.context, socket), TURBOWASM_TRAPPED);
        check_equal(sockets.tcp_drop(sockets.context, replacement), TURBOWASM_OK);
    }
    check_equal(sockets.network_drop(sockets.context, network), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK);
    check_true(complete);
    check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

suite("CNet listener terminal ownership") {
    it("consumes listen readiness and drains accept cancellation before slot reuse") {
        listener_cancel_drain(false);
    }
    it("routes listener cancellation while adapter shutdown is pending") {
        listener_cancel_drain(true);
    }
}
