#include "http_guest.h"

static uint8_t body[4096];
static uint32_t length;

/* Endpoint 0 is assigned by the embedding host. Even 404/500 is a completed
 * HTTP exchange; transport errors remain distinguishable negative results. */
int32_t fetch(void) {
    return app_http_get(0, body, sizeof(body), &length);
}
uint32_t response_length(void) { return length; }
uint32_t response_address(void) { return (uint32_t)(uintptr_t)body; }
int http_close(void) { return 0; }
