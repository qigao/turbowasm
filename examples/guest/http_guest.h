#ifndef TURBOWASM_EXAMPLE_HTTP_GUEST_H
#define TURBOWASM_EXAMPLE_HTTP_GUEST_H

#include "http_abi.h"
#include <stdint.h>
#include <turbowasm/import.h>

#if !defined(__wasm32__)
#error "app:http/v1 uses wasm32 memory offsets"
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous GET of a host-granted endpoint (zero-based ID).
 * Returns the HTTP status, including 4xx/5xx, or an APP_HTTP_* negative error.
 * body is binary, never NUL-terminated; body_length receives its byte count.
 * BUFFER_TOO_SMALL writes the required length and leaves body unchanged; it
 * has already performed the GET. Calling again performs another request.
 * Other errors leave both outputs unchanged. Invalid guest ranges trap before
 * network effects. body[0..capacity) and the four-byte body_length must not
 * overlap. capacity is at most the host's configured limit (<= MAX_BODY).
 * Buffers must remain stable and exclusively accessed until the call returns.
 * All storage remains guest-owned. A zero capacity can query response length.
 */
TURBOWASM_IMPORT("app:http/v1", "get")
int32_t app_http_get(uint32_t endpoint, void *body, uint32_t capacity,
                     uint32_t *body_length);

/* Same output/error contract as GET. Each method requires a host grant for
 * this endpoint. HEAD reports zero received bytes, not Content-Length.
 * DELETE sends no request body. No automatic retry or redirect. */
TURBOWASM_IMPORT("app:http/v1", "head")
int32_t app_http_head(uint32_t endpoint, void *body, uint32_t capacity,
                      uint32_t *body_length);
TURBOWASM_IMPORT("app:http/v1", "delete")
int32_t app_http_delete(uint32_t endpoint, void *body, uint32_t capacity,
                        uint32_t *body_length);

/* POST/PUT/PATCH snapshot request[0..request_length) before network effects.
 * Request bytes may alias response storage. Keep input stable during the call;
 * no pointer is retained after return. Empty input is valid (NULL, 0).
 * Request length must fit the host's request limit (<= MAX_BODY); Content-Type
 * is host-selected. Output semantics match GET, including BUFFER_TOO_SMALL:
 * the remote mutation may already have committed, so do not blindly retry.
 */
TURBOWASM_IMPORT("app:http/v1", "post")
int32_t app_http_post(uint32_t endpoint, const void *request, uint32_t request_length,
                      void *body, uint32_t capacity, uint32_t *body_length);
TURBOWASM_IMPORT("app:http/v1", "put")
int32_t app_http_put(uint32_t endpoint, const void *request, uint32_t request_length,
                     void *body, uint32_t capacity, uint32_t *body_length);
TURBOWASM_IMPORT("app:http/v1", "patch")
int32_t app_http_patch(uint32_t endpoint, const void *request, uint32_t request_length,
                       void *body, uint32_t capacity, uint32_t *body_length);

#ifdef __cplusplus
}
#endif
#endif
