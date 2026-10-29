#include <tinytest.h>
#include "http_host.h"
#include "reactor_session.h"
#include <http_server/http.h>
#include <salts/fs.h>
#include <fmt.h>
#include <stdatomic.h>
#include <string.h>

static chttp_server server;
static app_http http;
static app_http_endpoint endpoints[7];
static atomic_int received_method;
static atomic_int received_content_type;
static chttp_server_deferred pending;
static atomic_bool deferred_ready;
static atomic_uint requests;
static bool server_started;
static tstr connection;
static turbowasm_module module;
static salts_fs_buf_t program;
static turbowasm_linker linker;
static reactor_session session;
static turbowasm_trap trap;

static int reply(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
    unsigned route = (unsigned)(uintptr_t)user;
    atomic_fetch_add(&requests, 1);
    atomic_store(&received_method, request->method);
    if (route == 5) {
        if (request->method == CHTTP_METHOD_HEAD)
            return chttp_server_reply(response, 200, "text/plain", "metadata", 8);
        if (request->method == CHTTP_METHOD_DELETE)
            return chttp_server_reply(response, request->body_size ? 400 : 204, NULL, NULL, 0);
        const char *type = chttp_server_request_header(request, "Content-Type");
        int kind = !type ? 0 : !strcmp(type, "application/octet-stream") ? 1 :
            !strcmp(type, "application/json") ? 2 : 0;
        atomic_store(&received_content_type, kind);
        if (!kind) return chttp_server_reply(response, 415, NULL, NULL, 0);
        unsigned status = request->method == CHTTP_METHOD_POST ? 201 :
            request->method == CHTTP_METHOD_PATCH ? 202 : 200;
        return chttp_server_reply(response, status, type, request->body, request->body_size);
    }
    if (route == 3) {
        int status = chttp_server_response_defer(response, &pending);
        if (status == SALTS_OK) atomic_store(&deferred_ready, true);
        return status;
    }
    if (route == 2) {
        int status = chttp_server_response_set_header(response, "Location", "/binary");
        if (status != SALTS_OK) return status;
        return chttp_server_reply(response, 302, "text/plain", NULL, 0);
    }
    static const uint8_t large[128] = {0};
    return route == 4 ? chttp_server_reply(response, 200, "application/octet-stream", large, sizeof(large)) :
        route == 1 ? chttp_server_reply(response, 404, "text/plain", "missing", 7) :
        chttp_server_reply(response, 200, "application/octet-stream", "h\0i", 3);
}

static void open_session(void) {
    check_equal(app_http_define(&linker, &http), TURBOWASM_OK);
    check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}
static turbowasm_status invoke(const char *symbol, const int *argument, int32_t *value) {
    turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32}, out = {0};
    if (argument) arg.as.i32 = *argument;
    size_t count = 0;
    turbowasm_status status = reactor_session_call(&session, symbol, argument ? &arg : NULL,
        argument ? 1 : 0, &out, 1, &count, &trap);
    if (status == TURBOWASM_OK) {
        check_equal(count, (size_t)1);
        check_equal(out.kind, TURBOWASM_VALUE_I32);
        *value = out.as.i32;
    }
    return status;
}
static void run(int which) {
    int32_t value = -1;
    check_equal(invoke("http_case", &which, &value), TURBOWASM_OK);
    info("guest case %d failed at line %d", which, value);
    check_equal(value, 0);
}

spec("compiled guest HTTP module with real Chttp transport") {
    before_all() {
        check_equal(salts_fs_read_file(GUEST_HTTP_PATH, &program), 0);
        check_equal(turbowasm_module_load_borrowed(&module,
            (const uint8_t *)program.base, program.len), TURBOWASM_OK);
    }
    after_all() {
        turbowasm_module_destroy(&module);
        salts_fs_buf_free(&program);
    }
    before_each() {
        memset(&session, 0, sizeof(session));
        pending = (chttp_server_deferred){0};
        atomic_store(&deferred_ready, false);
        atomic_store(&requests, 0);
        atomic_store(&received_method, 0);
        atomic_store(&received_content_type, 0);
        trap = TURBOWASM_TRAP_NONE;
        chttp_client_config client_config = app_http_client_config();
        /* Deliberately smaller than the /large response; no giant test payload. */
        client_config.max_response_body_bytes = 64;
        client_config.max_request_body_bytes = 64;
        chttp_server_config server_config = {
            .host = "127.0.0.1", .port = 0, .backlog = 4,
            .network = client_config.network,
            .route_capacity = 11, .middleware_capacity = 1,
            .max_route_middleware_count = 1, .max_route_param_count = 1,
            .max_route_param_bytes = 64, .max_target_bytes = 256,
            .max_header_count = 16, .max_header_bytes = 4096,
            .max_request_body_bytes = 64, .max_response_header_count = 16,
            .max_response_header_bytes = 4096, .max_response_body_bytes = 256,
            .poll_slice_ms = 1};
        check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
        const char *paths[] = {"/binary", "/missing", "/redirect", "/pending", "/large"};
        for (size_t i = 0; i < 5; ++i)
            check_equal(chttp_server_route(&server, CHTTP_METHOD_GET, paths[i], reply, (void *)(uintptr_t)i), SALTS_OK);
        check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/pending", reply,
            (void *)(uintptr_t)3), SALTS_OK);
        const chttp_method methods[] = {CHTTP_METHOD_HEAD, CHTTP_METHOD_DELETE,
            CHTTP_METHOD_POST, CHTTP_METHOD_PUT, CHTTP_METHOD_PATCH};
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
            check_equal(chttp_server_route(&server, methods[i], "/methods", reply, (void *)(uintptr_t)5), SALTS_OK);
        check_equal(chttp_server_start(&server), SALTS_OK);
        server_started = true;
        uint16_t port = 0;
        check_equal(chttp_server_port(&server, &port), SALTS_OK);
        connection = tstr_format("tcp://127.0.0.1:{}", (unsigned)port);
        check_not_null(connection);
        for (size_t i = 0; i < 5; ++i)
            endpoints[i] = (app_http_endpoint){.connection_uri = connection,
                .authority = "localhost", .target = paths[i]};
        endpoints[5] = (app_http_endpoint){.connection_uri = connection,
            .authority = "localhost", .target = "/methods", .allowed_methods = APP_HTTP_ALLOW_ALL};
        endpoints[6] = (app_http_endpoint){.connection_uri = connection,
            .authority = "localhost", .target = "/methods", .allowed_methods = APP_HTTP_ALLOW_POST,
            .content_type = "application/json"};
        endpoints[3].allowed_methods = APP_HTTP_ALLOW_GET | APP_HTTP_ALLOW_POST;
        check_equal(app_http_init(&http, endpoints, 7, &client_config, 1000), SALTS_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    }
    after_each() {
        check_true(reactor_session_destroy(&session));
        turbowasm_linker_destroy(&linker);
        check_equal(app_http_destroy(&http, 5000), SALTS_OK);
        if (atomic_load(&deferred_ready))
            check_equal(chttp_server_deferred_cancel(&pending), SALTS_OK);
        if (server_started) {
            check_equal(chttp_server_stop(&server, 5000), SALTS_OK);
            server_started = false;
        }
        check_equal(chttp_server_destroy(&server), SALTS_OK);
        tstr_free(connection);
        connection = NULL;
    }
    it("runs the example and copies binary responses after linker destruction") {
        check_equal(app_http_destroy(&http, 5000), SALTS_OK);
        chttp_client_config config = app_http_client_config();
        check_equal(app_http_init(&http, endpoints, 7, &config, 1000), SALTS_OK);
        open_session();
        int32_t value = -1;
        check_equal(invoke("fetch", NULL, &value), TURBOWASM_OK);
        check_equal(value, 200);
        check_equal(invoke("response_length", NULL, &value), TURBOWASM_OK);
        check_equal(value, 3);
        for (int i = 0; i < 8; ++i) run(0);
        check_equal(atomic_load(&requests), 9u);
    }
    it("reports required capacity without truncation or automatic replay") {
        open_session();
        run(1);
        check_equal(atomic_load(&requests), 1u);
        run(8);
        check_equal(atomic_load(&requests), 2u);
        run(0);
        check_equal(atomic_load(&requests), 3u);
    }
    it("denies unknown capabilities and oversized or overlapping outputs before network effects") {
        open_session();
        run(2); run(3); run(6);
        check_equal(atomic_load(&requests), 0u);
    }
    it("traps an invalid body span before sending") {
        open_session();
        int which = 4; int32_t value;
        check_equal(invoke("http_case", &which, &value), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(atomic_load(&requests), 0u);
    }
    it("traps an invalid length output before sending") {
        open_session();
        int which = 5; int32_t value;
        check_equal(invoke("http_case", &which, &value), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(atomic_load(&requests), 0u);
    }
    it("returns HTTP failures and redirects without following Location") {
        open_session();
        run(7); run(9);
        check_equal(atomic_load(&requests), 2u);
    }
    it("rejects a response above the configured host bound and remains reusable") {
        open_session();
        run(11);
        run(0);
        check_equal(atomic_load(&requests), 2u);
    }
    it("times out a deferred response, drains, and preserves guest outputs") {
        open_session();
        run(10);
        check_true(atomic_load(&deferred_ready));
        check_equal(atomic_load(&requests), 1u);
        check_equal(chttp_server_deferred_cancel(&pending), SALTS_OK);
        atomic_store(&deferred_ready, false);
        run(0);
        check_equal(atomic_load(&requests), 2u);
    }
    it("fails linking when the host does not grant the HTTP module") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_LINK_ERROR);
        check_null(session.instance.impl);
        check_equal(atomic_load(&requests), 0u);
    }
    it("propagates connection failure through the guest error channel") {
        check_equal(chttp_server_stop(&server, 5000), SALTS_OK);
        server_started = false;
        /* Allow the OS to report refused connection before our request deadline. */
        http.timeout_ms = 10000;
        open_session();
        run(12);
        check_equal(atomic_load(&requests), 0u);
    }
    it("sends HEAD without receiving the advertised representation body") {
        open_session();
        run(13);
        check_equal(atomic_load(&received_method), CHTTP_METHOD_HEAD);
        run(0);
        check_equal(atomic_load(&requests), 2u);
    }
    it("sends DELETE and reports an empty 204 response") {
        open_session();
        run(14);
        check_equal(atomic_load(&received_method), CHTTP_METHOD_DELETE);
        check_equal(atomic_load(&requests), 1u);
    }
    it("sends binary POST PUT and PATCH bodies through the actual guest imports") {
        open_session();
        const chttp_method methods[] = {CHTTP_METHOD_POST, CHTTP_METHOD_PUT, CHTTP_METHOD_PATCH};
        for (int i = 0; i < 3; ++i) {
            run(15 + i);
            check_equal(atomic_load(&received_method), methods[i]);
            check_equal(atomic_load(&received_content_type), 1);
        }
        check_equal(atomic_load(&requests), 3u);
    }
    it("accepts empty bodies and snapshots aliased input before response writes") {
        open_session();
        run(18); run(19); run(29);
        check_equal(atomic_load(&requests), 3u);
    }
    it("never replays a write when the response buffer is too small") {
        open_session();
        run(20);
        check_equal(atomic_load(&received_method), CHTTP_METHOD_POST);
        check_equal(atomic_load(&requests), 1u);
    }
    it("keeps old endpoints GET-only and enforces explicit per-method grants") {
        open_session();
        run(21);
        check_equal(atomic_load(&requests), 0u);
        run(26);
        check_equal(atomic_load(&received_method), CHTTP_METHOD_POST);
        check_equal(atomic_load(&received_content_type), 2);
        check_equal(atomic_load(&requests), 1u);
    }
    it("rejects request limit overflow and output overlap before sending writes") {
        open_session();
        run(22); run(24);
        check_equal(atomic_load(&requests), 0u);
        run(27); /* Exactly the configured request capacity. */
        check_equal(atomic_load(&requests), 1u);
    }
    it("traps an invalid request span before sending a write") {
        open_session();
        int which = 23; int32_t value;
        check_equal(invoke("http_case", &which, &value), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(atomic_load(&requests), 0u);
    }
    it("traps invalid write response storage before performing the mutation") {
        open_session();
        int which = 25; int32_t value;
        check_equal(invoke("http_case", &which, &value), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(atomic_load(&requests), 0u);
    }
    it("rejects unknown policy bits and invalid request limits without an owner") {
        app_http empty = {0};
        chttp_client_config config = app_http_client_config();
        app_http_endpoint endpoint = endpoints[0];
        endpoint.allowed_methods = UINT32_C(0x80000000);
        check_equal(app_http_init(&empty, &endpoint, 1, &config, 1000), SALTS_EINVAL);
        check_null(empty.client.impl);
        check_null(empty.request_body);
        endpoint.allowed_methods = 0;
        config.max_request_body_bytes = APP_HTTP_MAX_BODY + 1;
        check_equal(app_http_init(&empty, &endpoint, 1, &config, 1000), SALTS_EINVAL);
        config.max_request_body_bytes = 0;
        check_equal(app_http_init(&empty, &endpoint, 1, &config, 1000), SALTS_EINVAL);
        check_null(empty.client.impl);
        check_null(empty.request_body);
    }
    it("does not replay a timed-out POST and remains reusable after drain") {
        open_session();
        run(28);
        check_true(atomic_load(&deferred_ready));
        check_equal(atomic_load(&requests), 1u);
        check_equal(atomic_load(&received_method), CHTTP_METHOD_POST);
        check_equal(chttp_server_deferred_cancel(&pending), SALTS_OK);
        atomic_store(&deferred_ready, false);
        run(15);
        check_equal(atomic_load(&requests), 2u);
    }
}
