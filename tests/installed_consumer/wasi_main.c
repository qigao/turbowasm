#include <turbowasm/wasi.h>

#include <stdint.h>

static uint32_t fd_write(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    (void)context;
    (void)fd;
    (void)buffers;
    (void)buffer_count;
    if (out_written == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fd_read(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    (void)context;
    (void)fd;
    (void)buffers;
    (void)buffer_count;
    if (out_read == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

int main(void) {
    const char *args[] = {"app"};
    turbowasm_wasi_preview1_config config = {
        .allow_args = true,
        .args = args,
        .arg_count = 1u,
        .allow_fd_write = true,
        .fd_write = fd_write,
        .allow_fd_read = true,
        .fd_read = fd_read
    };
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_linker linker = {0};

    if (turbowasm_wasi_preview1_init(
            &wasi, &config) != TURBOWASM_OK)
        return 1;
    if (turbowasm_linker_init(&linker) != TURBOWASM_OK)
        return 2;
    if (turbowasm_wasi_preview1_define(
            &wasi, &linker) != TURBOWASM_OK)
        return 3;

    turbowasm_linker_destroy(&linker);
    turbowasm_wasi_preview1_destroy(&wasi);
    return 0;
}
