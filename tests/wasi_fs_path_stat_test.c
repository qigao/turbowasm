#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x19, 0x04, 0x60,
    0x05, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01,
    0x7e, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x02,
    0x2c, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77,
    0x31, 0x11, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x73,
    0x74, 0x61, 0x74, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x03, 0x04, 0x03,
    0x01, 0x02, 0x03, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0x1b, 0x03, 0x07,
    0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d,
    0x00, 0x00, 0x0b, 0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0x3a, 0x00, 0x00,
    0x0b,
};

typedef struct fake_provider {
    uint32_t close_calls;
    uint32_t path_stat_calls;
    turbowasm_wasi_fs_file last_directory;
    uint32_t last_lookup_flags;
    uint8_t last_path[32];
    size_t last_path_length;
} fake_provider;

static uint32_t close_file(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_provider *provider = (fake_provider *)context;
    (void)file;
    assert(provider != NULL);
    ++provider->close_calls;
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
    assert(out_read != NULL);
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
    assert(out_written != NULL);
    *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t path_stat_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    fake_provider *provider = (fake_provider *)context;

    assert(provider != NULL);
    assert(out_stat != NULL);
    assert(path_length <= sizeof(provider->last_path));
    assert(path_length == 0u || path != NULL);

    ++provider->path_stat_calls;
    provider->last_directory = directory;
    provider->last_lookup_flags = lookup_flags;
    provider->last_path_length = path_length;
    if (path_length != 0u)
        memcpy(provider->last_path, path, path_length);

    *out_stat = (turbowasm_wasi_fs_stat){
        .size = UINT64_C(0x1020304050607080),
        .modified_ns = UINT64_C(200),
        .file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE,
        .device = UINT64_C(0x11),
        .inode = UINT64_C(0x22),
        .link_count = UINT64_C(3),
        .accessed_ns = UINT64_C(100),
        .changed_ns = UINT64_C(300)
    };
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_wasi_fs_config fs_config(fake_provider *provider) {
    turbowasm_wasi_fs_config config = {0};
    config.descriptor_capacity = 2u;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.path_stat = path_stat_file;
    return config;
}

static void bind_directories(turbowasm_wasi_fs *filesystem) {
    turbowasm_wasi_fs_descriptor descriptor = {0};

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               3u,
               (turbowasm_wasi_fs_file){100u, 1u},
               true,
               "/",
               TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET,
               0u,
               &descriptor) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               4u,
               (turbowasm_wasi_fs_file){200u, 1u},
               true,
               "/readonly",
               0u,
               0u,
               &descriptor) == TURBOWASM_OK);
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static int32_t invoke_errno(
    turbowasm_instance *instance,
    const turbowasm_value *arguments) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance,
               0u,
               arguments,
               5u,
               &result,
               1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static uint64_t guest_read64(
    turbowasm_instance *instance,
    uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 1u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I64);
    assert(trap == TURBOWASM_TRAP_NONE);
    return (uint64_t)result.as.i64;
}

static uint32_t guest_read8(
    turbowasm_instance *instance,
    uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 2u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(trap == TURBOWASM_TRAP_NONE);
    return (uint32_t)result.as.i32;
}

static void guest_store8(
    turbowasm_instance *instance,
    uint32_t address,
    uint32_t value) {
    turbowasm_value arguments[2] = {
        i32_value((int32_t)address),
        i32_value((int32_t)value)
    };
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 3u,
               arguments, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_low_level_path_stat(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_fs_stat stat = {0};
    const uint8_t path[] = {'a', 0u, 'b'};

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);

    assert(turbowasm_wasi_fs_path_stat(
               &filesystem,
               3u,
               7u,
               path,
               sizeof(path),
               &stat) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.path_stat_calls == 1u);
    assert(provider.last_directory.object == 100u);
    assert(provider.last_lookup_flags == 7u);
    assert(provider.last_path_length == sizeof(path));
    assert(memcmp(provider.last_path, path, sizeof(path)) == 0);
    assert(stat.size == UINT64_C(0x1020304050607080));

    assert(turbowasm_wasi_fs_path_stat(
               &filesystem,
               4u,
               0u,
               path,
               sizeof(path),
               &stat) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.path_stat_calls == 1u);

    config.provider.path_stat = NULL;
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);

    filesystem = (turbowasm_wasi_fs){0};
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);
    assert(turbowasm_wasi_fs_path_stat(
               &filesystem,
               3u,
               0u,
               path,
               sizeof(path),
               &stat) == TURBOWASM_WASI_ERRNO_NOSYS);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

static void test_guest_path_filestat_get(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value arguments[5];
    uint32_t before;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);

    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module,
               module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    guest_store8(&instance, 32u, 'a');
    guest_store8(&instance, 33u, 0u);
    guest_store8(&instance, 34u, 'b');

    arguments[0] = i32_value(3);
    arguments[1] = i32_value(9);
    arguments[2] = i32_value(32);
    arguments[3] = i32_value(3);
    arguments[4] = i32_value(64);

    assert(invoke_errno(&instance, arguments) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.path_stat_calls == 1u);
    assert(provider.last_lookup_flags == 9u);
    assert(provider.last_path_length == 3u);
    assert(provider.last_path[0] == 'a');
    assert(provider.last_path[1] == 0u);
    assert(provider.last_path[2] == 'b');

    assert(guest_read64(&instance, 64u) == UINT64_C(0x11));
    assert(guest_read64(&instance, 72u) == UINT64_C(0x22));
    assert(guest_read8(&instance, 80u) ==
           TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(guest_read64(&instance, 88u) == UINT64_C(3));
    assert(guest_read64(&instance, 96u) ==
           UINT64_C(0x1020304050607080));
    assert(guest_read64(&instance, 104u) == UINT64_C(100));
    assert(guest_read64(&instance, 112u) == UINT64_C(200));
    assert(guest_read64(&instance, 120u) == UINT64_C(300));

    before = provider.path_stat_calls;
    arguments[2] = i32_value(65535);
    arguments[3] = i32_value(2);
    assert(invoke_errno(&instance, arguments) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.path_stat_calls == before);

    arguments[2] = i32_value(32);
    arguments[3] = i32_value(3);
    arguments[4] = i32_value(65504);
    assert(invoke_errno(&instance, arguments) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.path_stat_calls == before);

    arguments[0] = i32_value(4);
    arguments[4] = i32_value(64);
    assert(invoke_errno(&instance, arguments) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.path_stat_calls == before);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_low_level_path_stat();
    test_guest_path_filestat_get();
    return 0;
}
