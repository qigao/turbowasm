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
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x02, 0x60,
    0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x02,
    0x89, 0x01, 0x03, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x15, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x63, 0x72, 0x65, 0x61,
    0x74, 0x65, 0x5f, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x79,
    0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77,
    0x31, 0x15, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x72, 0x65, 0x6d, 0x6f, 0x76,
    0x65, 0x5f, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x79, 0x00,
    0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73,
    0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31,
    0x10, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x75, 0x6e, 0x6c, 0x69, 0x6e, 0x6b,
    0x5f, 0x66, 0x69, 0x6c, 0x65, 0x00, 0x00, 0x03, 0x02, 0x01, 0x01, 0x05,
    0x03, 0x01, 0x00, 0x01, 0x0a, 0x0b, 0x01, 0x09, 0x00, 0x20, 0x00, 0x20,
    0x01, 0x3a, 0x00, 0x00, 0x0b,
};

enum mutation_kind {
    MUTATION_CREATE = 1,
    MUTATION_REMOVE = 2,
    MUTATION_UNLINK = 3
};

typedef struct fake_provider {
    uint32_t calls[4];
    uint32_t last_kind;
    turbowasm_wasi_fs_file last_directory;
    uint8_t last_path[32];
    size_t last_path_length;
    uint32_t next_error;
} fake_provider;

static uint32_t close_file(void *context, turbowasm_wasi_fs_file file) {
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
    (void)context; (void)file; (void)buffers; (void)buffer_count;
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
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    assert(out_written != NULL);
    *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t record_mutation(
    fake_provider *provider,
    uint32_t kind,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    uint32_t error;

    assert(provider != NULL);
    assert(path_length <= sizeof(provider->last_path));
    assert(path_length == 0u || path != NULL);
    ++provider->calls[kind];
    provider->last_kind = kind;
    provider->last_directory = directory;
    provider->last_path_length = path_length;
    if (path_length != 0u)
        memcpy(provider->last_path, path, path_length);
    error = provider->next_error;
    provider->next_error = 0u;
    return error;
}

static uint32_t create_directory(
    void *context, turbowasm_wasi_fs_file directory,
    const uint8_t *path, size_t path_length) {
    return record_mutation(
        (fake_provider *)context, MUTATION_CREATE,
        directory, path, path_length);
}

static uint32_t remove_directory(
    void *context, turbowasm_wasi_fs_file directory,
    const uint8_t *path, size_t path_length) {
    return record_mutation(
        (fake_provider *)context, MUTATION_REMOVE,
        directory, path, path_length);
}

static uint32_t unlink_file(
    void *context, turbowasm_wasi_fs_file directory,
    const uint8_t *path, size_t path_length) {
    return record_mutation(
        (fake_provider *)context, MUTATION_UNLINK,
        directory, path, path_length);
}

static turbowasm_wasi_fs_config fs_config(fake_provider *provider) {
    turbowasm_wasi_fs_config config = {0};
    config.descriptor_capacity = 4u;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.path_create_directory = create_directory;
    config.provider.path_remove_directory = remove_directory;
    config.provider.path_unlink_file = unlink_file;
    return config;
}

static void bind_directories(turbowasm_wasi_fs *filesystem) {
    turbowasm_wasi_fs_descriptor descriptor = {0};

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 3u, (turbowasm_wasi_fs_file){100u, 1u},
        true, "/create", TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY,
        0u, &descriptor) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 4u, (turbowasm_wasi_fs_file){200u, 1u},
        true, "/remove", TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY,
        0u, &descriptor) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 5u, (turbowasm_wasi_fs_file){300u, 1u},
        true, "/unlink", TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE,
        0u, &descriptor) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 6u, (turbowasm_wasi_fs_file){400u, 1u},
        true, "/none", 0u, 0u, &descriptor) == TURBOWASM_OK);
}

static void close_directories(turbowasm_wasi_fs *filesystem) {
    uint32_t fd;
    for (fd = 3u; fd <= 6u; ++fd) {
        assert(turbowasm_wasi_fs_close_fd(
            filesystem, fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    }
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static int32_t invoke_errno(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t fd,
    uint32_t path_address,
    uint32_t path_length) {
    turbowasm_value arguments[3] = {
        i32_value((int32_t)fd),
        i32_value((int32_t)path_address),
        i32_value((int32_t)path_length)
    };
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
        instance, function_index, arguments, 3u,
        &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
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
        instance, 3u, arguments, 2u, NULL, 0u,
        &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_low_level_mutations(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    const uint8_t path[] = {'a', 0u, 'b'};

    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);

    assert(turbowasm_wasi_fs_path_create_directory(
        &filesystem, 3u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.calls[MUTATION_CREATE] == 1u);
    assert(provider.last_directory.object == 100u);
    assert(provider.last_path_length == sizeof(path));
    assert(memcmp(provider.last_path, path, sizeof(path)) == 0);

    assert(turbowasm_wasi_fs_path_remove_directory(
        &filesystem, 4u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.calls[MUTATION_REMOVE] == 1u);

    assert(turbowasm_wasi_fs_path_unlink_file(
        &filesystem, 5u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.calls[MUTATION_UNLINK] == 1u);

    assert(turbowasm_wasi_fs_path_create_directory(
        &filesystem, 4u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(turbowasm_wasi_fs_path_remove_directory(
        &filesystem, 5u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(turbowasm_wasi_fs_path_unlink_file(
        &filesystem, 3u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);

    provider.next_error = TURBOWASM_WASI_ERRNO_IO;
    assert(turbowasm_wasi_fs_path_unlink_file(
        &filesystem, 5u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_IO);

    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);

    config.provider.path_create_directory = NULL;
    filesystem = (turbowasm_wasi_fs){0};
    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);
    assert(turbowasm_wasi_fs_path_create_directory(
        &filesystem, 3u, path, sizeof(path)) == TURBOWASM_WASI_ERRNO_NOSYS);
    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);
}

static void test_guest_mutations(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    uint32_t before;

    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);
    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(&wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
        &module, module_bytes, sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(&wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    guest_store8(&instance, 32u, 'x');
    guest_store8(&instance, 33u, 0u);
    guest_store8(&instance, 34u, 'y');

    assert(invoke_errno(&instance, 0u, 3u, 32u, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(invoke_errno(&instance, 1u, 4u, 32u, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(invoke_errno(&instance, 2u, 5u, 32u, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.last_path_length == 3u);
    assert(provider.last_path[0] == 'x');
    assert(provider.last_path[1] == 0u);
    assert(provider.last_path[2] == 'y');

    before = provider.calls[MUTATION_CREATE];
    assert(invoke_errno(&instance, 0u, 3u, 65535u, 2u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.calls[MUTATION_CREATE] == before);

    before = provider.calls[MUTATION_REMOVE];
    assert(invoke_errno(&instance, 1u, 6u, 32u, 3u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.calls[MUTATION_REMOVE] == before);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_low_level_mutations();
    test_guest_mutations();
    return 0;
}
