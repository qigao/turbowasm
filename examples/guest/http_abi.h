#ifndef TURBOWASM_EXAMPLE_HTTP_ABI_H
#define TURBOWASM_EXAMPLE_HTTP_ABI_H

/* Application wire constants shared by the native adapter and wasm32 guest. */
enum {
    APP_HTTP_DENIED = -1,
    APP_HTTP_BUFFER_TOO_SMALL = -2,
    APP_HTTP_LIMIT = -3,
    APP_HTTP_TIMEOUT = -4,
    APP_HTTP_IO = -5,
    APP_HTTP_INVALID_ARGUMENT = -6,
    APP_HTTP_MAX_BODY = 64 * 1024,
    APP_HTTP_MAX_ENDPOINTS = 8,
    APP_HTTP_MAX_TIMEOUT_MS = 60000
};

#endif
