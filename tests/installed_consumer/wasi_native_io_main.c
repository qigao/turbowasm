#include <turbowasm/wasi.h>
#include <turbowasm/wasi_native_io.h>

#include <salts/error_codes.h>
#include <salts/native_io.h>

#include <stdbool.h>
#include <stdint.h>

static uint32_t resolve_fd(
    void *context,
    uint32_t fd,
    bool write,
    native_io_endpoint *out_endpoint,
    native_io_operation_kind *out_kind) {
    (void)context;
    (void)fd;
    (void)write;
    (void)out_endpoint;
    (void)out_kind;
    return TURBOWASM_WASI_ERRNO_BADF;
}

int main(void) {
    native_io_backend backend = {0};
    native_io_backend_config backend_config = {0};
    turbowasm_native_io_bridge bridge = {0};
    turbowasm_wasi_native_io provider = {0};
    turbowasm_wasi_native_io_config provider_config = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_linker linker = {0};

#if defined(_WIN32)
    backend_config.kind = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    backend_config.kind = NATIVE_IO_BACKEND_EPOLL;
#else
    backend_config.kind = NATIVE_IO_BACKEND_KQUEUE;
#endif
    backend_config.endpoint_capacity = 1u;
    backend_config.request_capacity = 1u;
    backend_config.completion_batch_capacity = 1u;

    if (native_io_backend_init(
            &backend, &backend_config) != SALTS_OK)
        return 1;
    if (turbowasm_native_io_bridge_init(
            &bridge, &backend, 1u) != SALTS_OK)
        return 2;

    provider_config.bridge = &bridge;
    provider_config.resolve_fd = resolve_fd;
    if (turbowasm_wasi_native_io_init(
            &provider, &provider_config) != TURBOWASM_OK)
        return 3;

    if (turbowasm_wasi_native_io_apply(
            &provider, &wasi_config,
            true, true) != TURBOWASM_OK)
        return 4;
    if (turbowasm_wasi_preview1_init(
            &wasi, &wasi_config) != TURBOWASM_OK)
        return 5;
    if (turbowasm_linker_init(&linker) != TURBOWASM_OK)
        return 6;
    if (turbowasm_wasi_preview1_define(
            &wasi, &linker) != TURBOWASM_OK)
        return 7;

    turbowasm_linker_destroy(&linker);
    turbowasm_wasi_preview1_destroy(&wasi);
    turbowasm_wasi_native_io_destroy(&provider);
    if (turbowasm_native_io_bridge_destroy(
            &bridge) != SALTS_OK)
        return 8;
    if (native_io_backend_close(&backend) != SALTS_OK)
        return 9;
    if (native_io_backend_destroy(&backend) != SALTS_OK)
        return 10;
    return 0;
}
