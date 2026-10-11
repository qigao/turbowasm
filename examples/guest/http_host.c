#include "http_host.h"
#include <stdlib.h>

chttp_client_config app_http_client_config(void) {
    chttp_client_config config = {
        .network = {
#if defined(_WIN32)
            .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
            .backend = NATIVE_IO_BACKEND_EPOLL,
#else
            .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
            .connection_capacity = 1, .command_capacity = 8,
            .request_capacity = 4, .completion_batch_capacity = 4,
            .event_capacity = 8, .max_send_bytes = APP_HTTP_MAX_BODY + 8192,
            .receive_buffer_bytes = 4096, .connect_timeout_ms = 5000,
            .read_timeout_ms = 5000, .write_timeout_ms = 5000},
        .request_capacity = 1, .max_start_line_bytes = 2048,
        .max_header_count = 32, .max_header_bytes = 8192,
        .max_request_body_bytes = APP_HTTP_MAX_BODY, .max_response_body_bytes = APP_HTTP_MAX_BODY,
        .max_informational_responses = 4};
    return config;
}

int app_http_init(app_http *http, const app_http_endpoint *endpoints, size_t count,
                  const chttp_client_config *config, uint32_t timeout_ms) {
    if (!http || http->client.impl || !endpoints || !count ||
        count > APP_HTTP_MAX_ENDPOINTS || !config ||
        !config->max_request_body_bytes || config->max_request_body_bytes > APP_HTTP_MAX_BODY ||
        !config->max_response_body_bytes || config->max_response_body_bytes > APP_HTTP_MAX_BODY ||
        !timeout_ms || timeout_ms > APP_HTTP_MAX_TIMEOUT_MS) return SALTS_EINVAL;
    for (size_t i = 0; i < count; ++i) {
        if (!endpoints[i].connection_uri || !endpoints[i].authority ||
            !endpoints[i].target || (endpoints[i].allowed_methods & ~APP_HTTP_ALLOW_ALL))
            return SALTS_EINVAL;
    }
    uint8_t *request_body = malloc(config->max_request_body_bytes);
    if (!request_body) return SALTS_ENOMEM;
    int status = chttp_client_init(&http->client, config);
    if (status != SALTS_OK) {
        free(request_body);
        return status;
    }
    http->endpoints = endpoints;
    http->endpoint_count = count;
    http->timeout_ms = timeout_ms;
    http->max_body = (uint32_t)config->max_response_body_bytes;
    http->max_request_body = (uint32_t)config->max_request_body_bytes;
    http->request_body = request_body;
    return SALTS_OK;
}

static turbowasm_status result(turbowasm_value *out, size_t *count, int32_t code) {
    *out = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = code};
    *count = 1;
    return TURBOWASM_OK;
}

typedef int (*native_request_fn)(chttp_client *, const chttp_options *,
    chttp_response *, chttp_error *);

static turbowasm_status request(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *out,
    size_t capacity, size_t *count, turbowasm_trap *trap,
    uint32_t method, bool has_body, native_request_fn perform) {
    app_http *http = context;
    if (!http || !http->client.impl || !call || !args || argc != (has_body ? 6u : 4u) ||
        !out || !capacity || !count || !trap) return TURBOWASM_INVALID_ARGUMENT;
    *count = 0;
    *trap = TURBOWASM_TRAP_NONE;
    uint32_t endpoint = (uint32_t)args[0].as.i32;
    size_t output_index = has_body ? 3 : 1;
    uint64_t request_address = has_body ? (uint32_t)args[1].as.i32 : 0;
    uint32_t request_size = has_body ? (uint32_t)args[2].as.i32 : 0;
    uint64_t address = (uint32_t)args[output_index].as.i32;
    uint32_t size = (uint32_t)args[output_index + 1].as.i32;
    uint64_t length_address = (uint32_t)args[output_index + 2].as.i32;
    if (endpoint >= http->endpoint_count) return result(out, count, APP_HTTP_DENIED);
    const app_http_endpoint *target = &http->endpoints[endpoint];
    uint32_t allowed = target->allowed_methods ? target->allowed_methods : APP_HTTP_ALLOW_GET;
    if (!(allowed & method)) return result(out, count, APP_HTTP_DENIED);
    if (size > http->max_body || request_size > http->max_request_body)
        return result(out, count, APP_HTTP_LIMIT);
    if (size && address < length_address + 4 && length_address < address + size)
        return result(out, count, APP_HTTP_INVALID_ARGUMENT);

    /* Validate every output before side effects, then keep only offsets during
     * Chttp progress. Protected copies also support a shared backing; callers
     * must synchronize their own output buffers across guest threads. */
    turbowasm_status status = turbowasm_host_call_memory_check64(call, 0, address, size, trap);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_host_call_memory_check64(call, 0, length_address, 4, trap);
    if (status != TURBOWASM_OK) return status;
    if (has_body) {
        /* Input may alias outputs: take the entire bounded snapshot before
         * entering native code. No guest memory pointer survives progress. */
        status = turbowasm_host_call_memory_read64(call, 0, request_address,
            http->request_body, request_size, trap);
        if (status != TURBOWASM_OK) return status;
    }
    status = turbowasm_host_call_check_interrupt(call);
    if (status != TURBOWASM_OK) return status;

    const chttp_header content_type = {"Content-Type",
        target->content_type ? target->content_type : "application/octet-stream"};
    chttp_options options = {.connection_uri = target->connection_uri,
        .authority = target->authority, .target = target->target, .tls = target->tls,
        .headers = has_body ? &content_type : NULL, .header_count = has_body ? 1 : 0,
        .body = request_size ? http->request_body : NULL, .body_size = request_size,
        .timeout_ms = http->timeout_ms};
    chttp_response response = {0};
    chttp_error error = {0};
    int native_status = perform(&http->client, &options, &response, &error);
    status = turbowasm_host_call_check_interrupt(call);
    if (status != TURBOWASM_OK) goto done;
    if (native_status != SALTS_OK) {
        int32_t code = native_status == SALTS_ETIMEDOUT ? APP_HTTP_TIMEOUT :
            (native_status == SALTS_ENOBUFS || native_status == SALTS_EMSGSIZE)
                ? APP_HTTP_LIMIT : APP_HTTP_IO;
        status = result(out, count, code);
        goto done;
    }
    if (response.body_size > http->max_body || response.status_code < 100 ||
        response.status_code > 599 || (response.body_size && !response.body)) {
        status = result(out, count, APP_HTTP_IO);
        goto done;
    }
    uint32_t length = (uint32_t)response.body_size;
    uint8_t encoded_length[4] = {(uint8_t)length, (uint8_t)(length >> 8),
        (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    int32_t code = length > size ? APP_HTTP_BUFFER_TOO_SMALL : (int32_t)response.status_code;
    if (length <= size) {
        status = turbowasm_host_call_memory_write64(call, 0, address, response.body, length, trap);
        if (status != TURBOWASM_OK) goto done;
    }
    status = turbowasm_host_call_memory_write64(call, 0, length_address, encoded_length, 4, trap);
    if (status == TURBOWASM_OK) status = result(out, count, code);
done:
    chttp_response_destroy(&response);
    return status;
}

#define HTTP_CALLBACK(name, permission, has_body) \
    static turbowasm_status name(void *context, turbowasm_host_call *call, \
        const turbowasm_value *args, size_t argc, turbowasm_value *out, \
        size_t capacity, size_t *count, turbowasm_trap *trap) { \
        return request(context, call, args, argc, out, capacity, count, trap, \
            permission, has_body, chttp_##name); \
    }
HTTP_CALLBACK(get, APP_HTTP_ALLOW_GET, false)
HTTP_CALLBACK(head, APP_HTTP_ALLOW_HEAD, false)
HTTP_CALLBACK(delete, APP_HTTP_ALLOW_DELETE, false)
HTTP_CALLBACK(post, APP_HTTP_ALLOW_POST, true)
HTTP_CALLBACK(put, APP_HTTP_ALLOW_PUT, true)
HTTP_CALLBACK(patch, APP_HTTP_ALLOW_PATCH, true)
#undef HTTP_CALLBACK

turbowasm_status app_http_define(turbowasm_linker *linker, app_http *http) {
    static const uint8_t module[] = "app:http/v1";
    static const turbowasm_value_kind params[] = {TURBOWASM_VALUE_I32,
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32,
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32};
    static const turbowasm_value_kind returns = TURBOWASM_VALUE_I32;
    static const struct {
        const char *name;
        size_t name_size;
        size_t param_count;
        turbowasm_host_function_fn callback;
    } methods[] = {
        {"get", 3, 4, get}, {"head", 4, 4, head}, {"delete", 6, 4, delete},
        {"post", 4, 6, post}, {"put", 3, 6, put}, {"patch", 5, 6, patch}};
    if (!http || !http->client.impl) return TURBOWASM_INVALID_ARGUMENT;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const turbowasm_host_function_type type = {params, methods[i].param_count, &returns, 1};
        turbowasm_status status = turbowasm_linker_define_host_function(linker,
            (turbowasm_name){module, sizeof(module) - 1},
            (turbowasm_name){(const uint8_t *)methods[i].name, methods[i].name_size},
            &type, methods[i].callback, http);
        if (status != TURBOWASM_OK) return status;
    }
    return TURBOWASM_OK;
}

int app_http_destroy(app_http *http, uint32_t timeout_ms) {
    if (!http) return SALTS_EINVAL;
    if (!http->client.impl) return SALTS_OK;
    int status = chttp_client_destroy(&http->client, timeout_ms);
    if (status == SALTS_OK) {
        free(http->request_body);
        *http = (app_http){0};
    }
    return status;
}
