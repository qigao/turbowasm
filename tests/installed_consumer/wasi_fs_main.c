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

static uint32_t path_open_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    turbowasm_wasi_fs_file *out_file) {
    (void)context;
    (void)directory;
    (void)dirflags;
    (void)path;
    (void)path_length;
    (void)oflags;
    (void)rights_base;
    (void)rights_inheriting;
    (void)fdflags;
    if (out_file == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    out_file->object = 2u;
    out_file->generation = 1u;
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
    config.provider.path_open = path_open_file;

    if (turbowasm_wasi_fs_init(
            &filesystem, &config) != TURBOWASM_OK)
        return 1;
    if (turbowasm_wasi_fs_bind_descriptor_with_rights(
            &filesystem, 3u, root, true, "/",
            TURBOWASM_WASI_RIGHT_PATH_OPEN,
            TURBOWASM_WASI_RIGHT_FD_READ |
                TURBOWASM_WASI_RIGHT_FD_WRITE,
            &descriptor) != TURBOWASM_OK)
        return 2;
    if (descriptor.generation == 0u)
        return 3;

    {
        turbowasm_wasi_preview1 wasi = {0};
        turbowasm_wasi_preview1_config wasi_config = {
            .allow_filesystem = true,
            .filesystem = &filesystem
        };
        turbowasm_linker linker = {0};

        if (turbowasm_wasi_preview1_init(
                &wasi, &wasi_config) != TURBOWASM_OK)
            return 6;
        if (turbowasm_linker_init(&linker) != TURBOWASM_OK)
            return 7;
        if (turbowasm_wasi_preview1_define(
                &wasi, &linker) != TURBOWASM_OK)
            return 8;

        turbowasm_linker_destroy(&linker);
        turbowasm_wasi_preview1_destroy(&wasi);
    }
    if (turbowasm_wasi_fs_close_descriptor(
            &filesystem, descriptor) !=
        TURBOWASM_WASI_ERRNO_SUCCESS)
        return 4;
    if (turbowasm_wasi_fs_destroy(
            &filesystem) != TURBOWASM_OK)
        return 5;
    return 0;
}
