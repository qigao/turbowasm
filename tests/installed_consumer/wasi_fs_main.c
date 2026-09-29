#include <turbowasm/wasi_fs.h>

static uint32_t close_file(
    void *context,
    turbowasm_wasi_fs_file file) {
    (void)context;
    (void)file;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t read_file(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_read == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t write_file(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_written == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

int main(void) {
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_descriptor descriptor = {0};
    turbowasm_wasi_fs_file root = {1u, 1u};

    config.descriptor_capacity = 1u;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;

    if (turbowasm_wasi_fs_init(
            &filesystem, &config) != TURBOWASM_OK)
        return 1;
    if (turbowasm_wasi_fs_bind_descriptor(
            &filesystem, 3u, root, true, "/",
            &descriptor) != TURBOWASM_OK)
        return 2;
    if (descriptor.generation == 0u)
        return 3;
    if (turbowasm_wasi_fs_close_descriptor(
            &filesystem, descriptor) !=
        TURBOWASM_WASI_ERRNO_SUCCESS)
        return 4;
    if (turbowasm_wasi_fs_destroy(
            &filesystem) != TURBOWASM_OK)
        return 5;
    return 0;
}
