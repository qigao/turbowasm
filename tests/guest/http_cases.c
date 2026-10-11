#include "http_guest.h"
#include <string.h>

/* Exercise the actual compiled guest ABI, including binary data and untouched
 * output on errors. A nonzero return identifies the failing source line. */
int http_case(int which) {
    uint8_t bytes[64];
    uint32_t length = 0x12345678;
    memset(bytes, 0xa5, sizeof(bytes));
    int32_t code;
    switch (which) {
    case 0:
        code = app_http_get(0, bytes, sizeof(bytes), &length);
        if (code != 200 || length != 3 || memcmp(bytes, "h\0i", 3)) return __LINE__;
        if (bytes[3] != 0xa5) return __LINE__;
        return 0;
    case 1:
        code = app_http_get(0, bytes, 2, &length);
        if (code != APP_HTTP_BUFFER_TOO_SMALL || length != 3) return __LINE__;
        break;
    case 2:
        code = app_http_get(99, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_DENIED || length != 0x12345678) return __LINE__;
        break;
    case 3:
        code = app_http_get(0, bytes, sizeof(bytes), (uint32_t *)bytes);
        if (code != APP_HTTP_INVALID_ARGUMENT || length != 0x12345678) return __LINE__;
        break;
    case 4:
        (void)app_http_get(0, (void *)(uintptr_t)UINT32_C(0xfffffff0), 32, &length);
        return __LINE__; /* must trap */
    case 5:
        (void)app_http_get(0, bytes, sizeof(bytes), (uint32_t *)(uintptr_t)UINT32_C(0xfffffffe));
        return __LINE__; /* must trap */
    case 6:
        code = app_http_get(0, bytes, APP_HTTP_MAX_BODY + 1, &length);
        if (code != APP_HTTP_LIMIT || length != 0x12345678) return __LINE__;
        break;
    case 7:
        code = app_http_get(1, bytes, sizeof(bytes), &length);
        if (code != 404 || length != 7 || memcmp(bytes, "missing", 7)) return __LINE__;
        return 0;
    case 8:
        code = app_http_get(0, NULL, 0, &length);
        if (code != APP_HTTP_BUFFER_TOO_SMALL || length != 3) return __LINE__;
        break;
    case 9:
        code = app_http_get(2, bytes, sizeof(bytes), &length);
        if (code != 302 || length != 0) return __LINE__;
        break;
    case 10:
        code = app_http_get(3, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_TIMEOUT || length != 0x12345678) return __LINE__;
        break;
    case 11:
        code = app_http_get(4, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_LIMIT || length != 0x12345678) return __LINE__;
        break;
    case 12:
        code = app_http_get(0, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_IO || length != 0x12345678) return __LINE__;
        break;
    case 13:
        code = app_http_head(5, bytes, sizeof(bytes), &length);
        if (code != 200 || length != 0) return __LINE__;
        break;
    case 14:
        code = app_http_delete(5, bytes, sizeof(bytes), &length);
        if (code != 204 || length != 0) return __LINE__;
        break;
    case 15:
    case 16:
    case 17:
        code = which == 15 ? app_http_post(5, "p\0q", 3, bytes, sizeof(bytes), &length) :
            which == 16 ? app_http_put(5, "p\0q", 3, bytes, sizeof(bytes), &length) :
                app_http_patch(5, "p\0q", 3, bytes, sizeof(bytes), &length);
        if (code != (which == 15 ? 201 : which == 16 ? 200 : 202) ||
            length != 3 || memcmp(bytes, "p\0q", 3) || bytes[3] != 0xa5) return __LINE__;
        return 0;
    case 18:
        code = app_http_post(5, NULL, 0, bytes, sizeof(bytes), &length);
        if (code != 201 || length != 0) return __LINE__;
        break;
    case 19:
        memcpy(bytes, "p\0q", 3);
        code = app_http_patch(5, bytes, 3, bytes, sizeof(bytes), &length);
        if (code != 202 || length != 3 || memcmp(bytes, "p\0q", 3) || bytes[3] != 0xa5)
            return __LINE__;
        return 0;
    case 20:
        code = app_http_post(5, "p\0q", 3, bytes, 2, &length);
        if (code != APP_HTTP_BUFFER_TOO_SMALL || length != 3) return __LINE__;
        break;
    case 21:
        if (app_http_head(0, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            app_http_delete(0, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            app_http_post(0, "x", 1, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            app_http_put(0, "x", 1, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            app_http_patch(0, "x", 1, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            app_http_get(6, bytes, sizeof(bytes), &length) != APP_HTTP_DENIED ||
            length != 0x12345678) return __LINE__;
        break;
    case 22:
        code = app_http_put(5, bytes, 65, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_LIMIT || length != 0x12345678) return __LINE__;
        code = app_http_post(5, bytes, UINT32_MAX, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_LIMIT || length != 0x12345678) return __LINE__;
        break;
    case 23:
        (void)app_http_post(5, (void *)(uintptr_t)UINT32_C(0xfffffffe), 4,
            bytes, sizeof(bytes), &length);
        return __LINE__; /* must trap before sending */
    case 24:
        code = app_http_post(5, "x", 1, bytes, sizeof(bytes), (uint32_t *)bytes);
        if (code != APP_HTTP_INVALID_ARGUMENT || length != 0x12345678) return __LINE__;
        break;
    case 25:
        (void)app_http_patch(5, "x", 1, bytes, sizeof(bytes),
            (uint32_t *)(uintptr_t)UINT32_C(0xfffffffe));
        return __LINE__; /* must trap before sending */
    case 26:
        code = app_http_post(6, "{}", 2, bytes, sizeof(bytes), &length);
        if (code != 201 || length != 2 || memcmp(bytes, "{}", 2)) return __LINE__;
        return 0;
    case 27:
        code = app_http_put(5, bytes, sizeof(bytes), bytes, sizeof(bytes), &length);
        if (code != 200 || length != sizeof(bytes)) return __LINE__;
        break;
    case 28:
        code = app_http_post(3, "x", 1, bytes, sizeof(bytes), &length);
        if (code != APP_HTTP_TIMEOUT || length != 0x12345678) return __LINE__;
        break;
    case 29:
        code = app_http_put(5, &length, sizeof(length), bytes, sizeof(bytes), &length);
        if (code != 200 || length != 4 || memcmp(bytes, "\x78\x56\x34\x12", 4))
            return __LINE__;
        return 0;
    default:
        return __LINE__;
    }
    for (size_t i = 0; i < sizeof(bytes); ++i)
        if (bytes[i] != 0xa5) return __LINE__;
    return 0;
}
