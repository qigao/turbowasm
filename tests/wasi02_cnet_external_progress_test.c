#include <turbowasm/wasi02_cnet.h>
#include <salts/error_codes.h>
#include <tinytest.h>

static void check_external_progress(size_t endpoints, size_t requests, bool admissible) {
    native_io_backend backend = {0};
    native_io_backend_config native = {0}, observed = {0};
    turbowasm_wasi02_io io = {0};
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_cnet_config config;
    turbowasm_wasi02_socket_provider provider = {0};
    native_io_completion unrelated = {0};
    bool consumed = true, complete = false;
    size_t events = SIZE_MAX;
    uint32_t timeout = UINT32_MAX;

    turbowasm_wasi02_cnet_config_init(&config);
    config.socket_capacity = 2;
    native.kind = config.backend;
    native.endpoint_capacity = endpoints;
    native.request_capacity = requests;
    native.completion_batch_capacity = requests < 8 ? requests : 8;
    check_equal(native_io_backend_init(&backend, &native), SALTS_OK);
    check_equal(turbowasm_wasi02_io_init(&io, NULL, NULL), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_init_external(&adapter, &io, &backend, &config, NULL),
        admissible ? TURBOWASM_OK : TURBOWASM_INVALID_ARGUMENT);
    if (admissible) {
        /* The completed public adapter now exposes the data plane as well. */
        check_equal(turbowasm_wasi02_cnet_socket_provider(&adapter, &provider), TURBOWASM_OK);
        check_true(provider.tcp_start_connect != NULL);
        check_true(provider.tcp_finish_connect != NULL);
        check_true(provider.tcp_accept != NULL);
        check_true(provider.tcp_subscribe != NULL);
        check_true(provider.tcp_shutdown != NULL);
        check_equal(turbowasm_wasi02_cnet_advance(&adapter, &events), TURBOWASM_OK);
        check_equal(events, (size_t)0);
        check_equal(turbowasm_wasi02_cnet_next_timeout(&adapter, 50, &timeout), TURBOWASM_OK);
        check_less_equal(timeout, (uint32_t)50);
        unrelated.user_data = 1;
        check_equal(turbowasm_wasi02_cnet_route_completion(&adapter, &unrelated, &consumed), TURBOWASM_OK);
        check_false(consumed);
        check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK);
        check_true(complete);
        check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter, &complete), TURBOWASM_OK);
        check_true(complete);
        check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_OK);
    } else {
        check_null(adapter.impl);
    }
    /* Destroying or failing to create the adapter must preserve its borrowed owners. */
    check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
    check_true(native_io_backend_get_config(&backend, &observed));
    check_equal(observed.kind, native.kind);
    check_equal(observed.request_capacity, requests);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

suite("CNet external progress ownership") {
    it("borrows the backend and retains the IO domain through shutdown") {
        check_external_progress(8, 16, true);
    }
    it("rejects insufficient request capacity without consuming either owner") {
        check_external_progress(8, 2, false);
    }
    it("rejects insufficient endpoint capacity without consuming either owner") {
        check_external_progress(3, 16, false);
    }
}
