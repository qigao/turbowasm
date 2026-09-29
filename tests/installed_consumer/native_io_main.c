#include <turbowasm/native_io.h>

#include <salts/error_codes.h>

int main(void) {
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    native_io_backend_config config = {0};

#if defined(_WIN32)
    config.kind = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    config.kind = NATIVE_IO_BACKEND_EPOLL;
#else
    config.kind = NATIVE_IO_BACKEND_KQUEUE;
#endif
    config.endpoint_capacity = 1u;
    config.request_capacity = 1u;
    config.completion_batch_capacity = 1u;

    if (native_io_backend_init(&backend, &config) != SALTS_OK)
        return 1;
    if (turbowasm_native_io_bridge_init(
            &bridge, &backend, 1u) != SALTS_OK)
        return 2;
    if (turbowasm_native_io_bridge_destroy(
            &bridge) != SALTS_OK)
        return 3;
    if (native_io_backend_close(&backend) != SALTS_OK)
        return 4;
    if (native_io_backend_destroy(&backend) != SALTS_OK)
        return 5;
    return 0;
}
