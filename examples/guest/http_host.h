#ifndef TURBOWASM_EXAMPLE_HTTP_HOST_H
#define TURBOWASM_EXAMPLE_HTTP_HOST_H

#include "http_abi.h"
#include <turbowasm/link.h>
#include <http_client/http.h>

/* Host policy bits, independent of Chttp's native method enum. Zero in an
 * endpoint preserves the original GET-only configuration. */
enum {
    APP_HTTP_ALLOW_GET = 1u << 0,
    APP_HTTP_ALLOW_HEAD = 1u << 1,
    APP_HTTP_ALLOW_POST = 1u << 2,
    APP_HTTP_ALLOW_PUT = 1u << 3,
    APP_HTTP_ALLOW_PATCH = 1u << 4,
    APP_HTTP_ALLOW_DELETE = 1u << 5,
    APP_HTTP_ALLOW_ALL = (1u << 6) - 1
};

/* The host selects the complete destination, request target and trust profile.
 * The guest receives only an index; no URL, credentials or native pointer.
 * HTTP redirects are returned, never followed by this adapter.
 */
typedef struct app_http_endpoint {
    const char *connection_uri;
    const char *authority;
    const char *target;
    const chttp_tls_profile *tls;
    uint32_t allowed_methods;
    /* Borrowed for the owner's lifetime; NULL selects application/octet-stream
     * for POST/PUT/PATCH. No guest-controlled headers or credentials. */
    const char *content_type;
} app_http_endpoint;

/* Example owner, not an installed Runtime API. Zero-initialize before init.
 * Single owner: initialize, register, invoke and destroy on the same thread;
 * do not share concurrently between instances. Endpoint storage/strings/TLS
 * profiles are borrowed and immutable until successful destroy. Instances and
 * active calls must end before destroy. The linker does not own this object.
 */
typedef struct app_http {
    chttp_client client;
    const app_http_endpoint *endpoints;
    size_t endpoint_count;
    uint32_t timeout_ms;
    uint32_t max_body;
    uint32_t max_request_body;
    uint8_t *request_body;
} app_http;

/* Bounded HTTP/1 defaults using the platform's ordinary NativeIO backend. */
chttp_client_config app_http_client_config(void);

/* Owns one Chttp client, borrows the endpoint table. Requires 1..MAX_ENDPOINTS,
 * timeout 1..MAX_TIMEOUT_MS, and request/response bounds 1..MAX_BODY. Returns native
 * SALTS status; failure leaves the zero-initialized owner empty. */
int app_http_init(app_http *http, const app_http_endpoint *endpoints, size_t count,
                  const chttp_client_config *config, uint32_t timeout_ms);

/* Register app:http/v1 get/head/delete (four i32 parameters) and post/put/patch
 * (six i32 parameters), each returning i32. Borrows http through all
 * consumers. Returns ordinary linker errors; a failed multi-module composition
 * must discard its fresh linker. The provider must already be initialized. */
turbowasm_status app_http_define(turbowasm_linker *linker, app_http *http);

/* Stop/drain the owned client. A native ETIMEDOUT preserves it for a later
 * destroy attempt; never release endpoint storage or http on failure.
 * No guest invocation may be active. Successful destroy clears the owner. */
int app_http_destroy(app_http *http, uint32_t timeout_ms);

#endif
