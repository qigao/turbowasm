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
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20, 0x05, 0x60,
    0x05, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7f,
    0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7e, 0x60, 0x01, 0x7f, 0x01,
    0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x02, 0xb4, 0x01, 0x04, 0x16, 0x77,
    0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74,
    0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x11, 0x70, 0x61,
    0x74, 0x68, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x74, 0x61, 0x74, 0x5f,
    0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73,
    0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76,
    0x69, 0x65, 0x77, 0x31, 0x15, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x63, 0x72,
    0x65, 0x61, 0x74, 0x65, 0x5f, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6f,
    0x72, 0x79, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e,
    0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31, 0x15, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x72, 0x65, 0x6d,
    0x6f, 0x76, 0x65, 0x5f, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6f, 0x72,
    0x79, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x10, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x75, 0x6e, 0x6c, 0x69,
    0x6e, 0x6b, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x00, 0x01, 0x03, 0x04, 0x03,
    0x02, 0x03, 0x04, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0x1b, 0x03, 0x07,
    0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d,
    0x00, 0x00, 0x0b, 0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0x3a, 0x00, 0x00,
    0x0b
};

typedef struct fake_provider {
    uint32_t close_calls;
    uint32_t stat_calls;
    uint32_t create_calls;
    uint32_t remove_calls;
    uint32_t unlink_calls;
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

static void record_path(
    fake_provider *provider,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    assert(provider != NULL);
    assert(path_length <= sizeof(provider->last_path));
    assert(path_length == 0u || path != NULL);

    provider->last_directory = directory;
    provider->last_path_length = path_length;
    if (path_length != 0u)
        memcpy(provider->last_path, path, path_length);
}

static uint32_t path_stat(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    fake_provider *provider = (fake_provider *)context;

    assert(out_stat != NULL);
    ++provider->stat_calls;
    provider->last_lookup_flags = lookup_flags;
    record_path(provider, directory, path, path_length);

    *out_stat = (turbowasm_wasi_fs_stat){
        .size = UINT64_C(0x0102030405060708),
        .modified_ns = UINT64_C(22),
        .file_type = TURBOWASM_WASI_FILETYPE_DIRECTORY,
        .device = UINT64_C(0x31),
        .inode = UINT64_C(0x42),
        .link_count = UINT64_C(2),
        .accessed_ns = UINT64_C(11),
        .changed_ns = UINT64_C(33)
    };
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t path_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_provider *provider = (fake_provider *)context;
    ++provider->create_calls;
    record_path(provider, directory, path, path_length);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t path_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_provider *provider = (fake_provider *)context;
    ++provider->remove_calls;
    record_path(provider, directory, path, path_length);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t path_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_provider *provider = (fake_provider *)context;
    ++provider->unlink_calls;
    record_path(provider, directory, path, path_length);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_wasi_fs_config fs_config(
    fake_provider *provider) {
    turbowasm_wasi_fs_config config = {0};

    config.descriptor_capacity = 2u;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.path_stat = path_stat;
    config.provider.path_create_directory =
        path_create_directory;
    config.provider.path_remove_directory =
        path_remove_directory;
    config.provider.path_unlink_file =
        path_unlink_file;
    return config;
}

static void bind_roots(
    turbowasm_wasi_fs *filesystem) {
    const uint64_t path_rights =
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
    turbowasm_wasi_fs_descriptor descriptor = {0};

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               3u,
               (turbowasm_wasi_fs_file){100u, 1u},
               true,
               "/sandbox",
               path_rights,
               0u,
               &descriptor) == TURBOWASM_OK);

    /*
     * Even with the same bits, a non-preopen descriptor is not an ambient path
     * authority in the current preopen-relative provider contract.
     */
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               4u,
               (turbowasm_wasi_fs_file){200u, 1u},
               false,
               NULL,
               path_rights,
               0u,
               &descriptor) == TURBOWASM_OK);
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static int32_t invoke_errno(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance,
               function_index,
               arguments,
               argument_count,
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
               instance, 4u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_VALUE_I64);
    assert(result_count == 1u);
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
               instance, 5u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result_count == 1u);
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
               instance, 6u,
               arguments, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_low_level_path_capabilities(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_fs_stat stat = {0};
    const uint8_t path[] = {'a', 'b', 'c'};
    uint32_t before;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_roots(&filesystem);

    assert(turbowasm_wasi_fs_path_stat(
               &filesystem, 3u,
               TURBOWASM_WASI_LOOKUP_SYMLINK_FOLLOW,
               path, sizeof(path), &stat) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.stat_calls == 1u);
    assert(provider.last_directory.object == 100u);
    assert(provider.last_lookup_flags ==
           TURBOWASM_WASI_LOOKUP_SYMLINK_FOLLOW);
    assert(provider.last_path_length == sizeof(path));
    assert(memcmp(provider.last_path, path, sizeof(path)) == 0);
    assert(stat.size == UINT64_C(0x0102030405060708));

    before = provider.stat_calls;
    assert(turbowasm_wasi_fs_path_stat(
               &filesystem, 3u, 2u,
               path, sizeof(path), &stat) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(provider.stat_calls == before);

    assert(turbowasm_wasi_fs_path_create_directory(
               &filesystem, 3u,
               path, sizeof(path)) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_path_remove_directory(
               &filesystem, 3u,
               path, sizeof(path)) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_path_unlink_file(
               &filesystem, 3u,
               path, sizeof(path)) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.create_calls == 1u);
    assert(provider.remove_calls == 1u);
    assert(provider.unlink_calls == 1u);

    assert(turbowasm_wasi_fs_path_stat(
               &filesystem, 4u, 0u,
               path, sizeof(path), &stat) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(turbowasm_wasi_fs_path_create_directory(
               &filesystem, 4u,
               path, sizeof(path)) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

static void test_guest_path_metadata_and_mutation_abi(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value stat_args[5];
    turbowasm_value mutation_args[3];
    uint32_t before;
    uint32_t index;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_roots(&filesystem);

    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module,
               module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(
               &linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    guest_store8(&instance, 32u, 'a');
    guest_store8(&instance, 33u, 'b');
    guest_store8(&instance, 34u, 'c');

    stat_args[0] = i32_value(3);
    stat_args[1] = i32_value(
        TURBOWASM_WASI_LOOKUP_SYMLINK_FOLLOW);
    stat_args[2] = i32_value(32);
    stat_args[3] = i32_value(3);
    stat_args[4] = i32_value(64);
    assert(invoke_errno(
               &instance, 0u,
               stat_args, 5u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.stat_calls == 1u);
    assert(guest_read64(&instance, 64u) == UINT64_C(0x31));
    assert(guest_read64(&instance, 72u) == UINT64_C(0x42));
    assert(guest_read8(&instance, 80u) ==
           TURBOWASM_WASI_FILETYPE_DIRECTORY);
    assert(guest_read64(&instance, 88u) == UINT64_C(2));
    assert(guest_read64(&instance, 96u) ==
           UINT64_C(0x0102030405060708));
    assert(guest_read64(&instance, 104u) == UINT64_C(11));
    assert(guest_read64(&instance, 112u) == UINT64_C(22));
    assert(guest_read64(&instance, 120u) == UINT64_C(33));

    mutation_args[0] = i32_value(3);
    mutation_args[1] = i32_value(32);
    mutation_args[2] = i32_value(3);
    for (index = 1u; index <= 3u; ++index) {
        assert(invoke_errno(
                   &instance, index,
                   mutation_args, 3u) ==
               TURBOWASM_WASI_ERRNO_SUCCESS);
    }
    assert(provider.create_calls == 1u);
    assert(provider.remove_calls == 1u);
    assert(provider.unlink_calls == 1u);

    /* Both guest ranges are validated before path_stat provider side effects. */
    before = provider.stat_calls;
    stat_args[4] = i32_value(65504);
    assert(invoke_errno(
               &instance, 0u,
               stat_args, 5u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.stat_calls == before);

    stat_args[2] = i32_value(65535);
    stat_args[3] = i32_value(2);
    stat_args[4] = i32_value(64);
    assert(invoke_errno(
               &instance, 0u,
               stat_args, 5u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.stat_calls == before);

    stat_args[1] = i32_value(2);
    stat_args[2] = i32_value(32);
    stat_args[3] = i32_value(3);
    assert(invoke_errno(
               &instance, 0u,
               stat_args, 5u) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(provider.stat_calls == before);

    before = provider.create_calls;
    mutation_args[1] = i32_value(65535);
    mutation_args[2] = i32_value(2);
    assert(invoke_errno(
               &instance, 1u,
               mutation_args, 3u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.create_calls == before);

    mutation_args[0] = i32_value(4);
    mutation_args[1] = i32_value(32);
    mutation_args[2] = i32_value(3);
    assert(invoke_errno(
               &instance, 3u,
               mutation_args, 3u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.unlink_calls == 1u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_low_level_path_capabilities();
    test_guest_path_metadata_and_mutation_abi();
    return 0;
}
