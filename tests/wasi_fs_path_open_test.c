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

typedef struct fake_provider {
    uint32_t open_calls;
    uint32_t close_calls;
    uint32_t read_calls;
    uint32_t write_calls;
    turbowasm_wasi_fs_file last_directory;
    turbowasm_wasi_fs_file last_closed;
    uint8_t last_path[32];
    size_t last_path_length;
    uint32_t last_dirflags;
    uint32_t last_oflags;
    uint64_t last_rights_base;
    uint64_t last_rights_inheriting;
    uint32_t last_fdflags;
    uint64_t next_object;
} fake_provider;

static uint32_t close_file(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_provider *provider = (fake_provider *)context;
    assert(provider != NULL);
    ++provider->close_calls;
    provider->last_closed = file;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t read_file(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    fake_provider *provider = (fake_provider *)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    assert(provider != NULL);
    assert(out_read != NULL);
    ++provider->read_calls;
    *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t write_file(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    fake_provider *provider = (fake_provider *)context;
    size_t index;
    uint32_t total = 0u;
    (void)file;
    assert(provider != NULL);
    assert(out_written != NULL);
    ++provider->write_calls;
    for (index = 0u; index < buffer_count; ++index)
        total += (uint32_t)buffers[index].size;
    *out_written = total;
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
    fake_provider *provider = (fake_provider *)context;

    assert(provider != NULL);
    assert(out_file != NULL);
    assert(path_length <= sizeof(provider->last_path));
    assert(path_length == 0u || path != NULL);

    ++provider->open_calls;
    provider->last_directory = directory;
    provider->last_path_length = path_length;
    if (path_length != 0u)
        memcpy(provider->last_path, path, path_length);
    provider->last_dirflags = dirflags;
    provider->last_oflags = oflags;
    provider->last_rights_base = rights_base;
    provider->last_rights_inheriting = rights_inheriting;
    provider->last_fdflags = fdflags;

    ++provider->next_object;
    out_file->object = UINT64_C(1000) + provider->next_object;
    out_file->generation = 1u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_wasi_fs_config fs_config(
    fake_provider *provider,
    size_t capacity) {
    turbowasm_wasi_fs_config config = {0};
    config.descriptor_capacity = capacity;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.path_open = path_open_file;
    return config;
}

static void test_low_level_path_open_rights_and_rollback(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider, 2u);
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint32_t opened_fd = 0u;
    const uint8_t embedded_nul_path[] = {'a', 0u, 'b'};
    const uint8_t second_path[] = {'x'};
    const uint8_t payload[] = {'o','k'};
    turbowasm_wasi_const_buffer buffer = {
        payload, sizeof(payload)
    };
    uint32_t written = 0u;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem,
               3u,
               (turbowasm_wasi_fs_file){10u, 1u},
               true,
               "/",
               TURBOWASM_WASI_RIGHT_PATH_OPEN,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               &root) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_path_open(
               &filesystem,
               3u,
               7u,
               embedded_nul_path,
               sizeof(embedded_nul_path),
               11u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u,
               13u,
               &opened_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(opened_fd == 4u);
    assert(provider.open_calls == 1u);
    assert(provider.last_directory.object == 10u);
    assert(provider.last_path_length == 3u);
    assert(provider.last_path[0] == 'a');
    assert(provider.last_path[1] == 0u);
    assert(provider.last_path[2] == 'b');
    assert(provider.last_dirflags == 7u);
    assert(provider.last_oflags == 11u);
    assert(provider.last_rights_base ==
           TURBOWASM_WASI_RIGHT_FD_WRITE);
    assert(provider.last_rights_inheriting == 0u);
    assert(provider.last_fdflags == 13u);

    assert(turbowasm_wasi_fs_descriptor_info_get(
               &filesystem, 4u, &info));
    assert(!info.preopen);
    assert(info.rights_base ==
           TURBOWASM_WASI_RIGHT_FD_WRITE);
    assert(info.rights_inheriting == 0u);

    assert(turbowasm_wasi_fs_fd_write(
               &filesystem, 4u,
               &buffer, 1u,
               &written) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(written == 2u);
    assert(provider.write_calls == 1u);

    assert(turbowasm_wasi_fs_fd_read(
               &filesystem, 4u,
               NULL, 0u,
               &written) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.read_calls == 0u);

    /* Rights escalation is rejected before the provider sees the request. */
    assert(turbowasm_wasi_fs_path_open(
               &filesystem,
               3u, 0u,
               second_path, sizeof(second_path),
               0u,
               TURBOWASM_WASI_RIGHT_FD_READ,
               0u, 0u,
               &opened_fd) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.open_calls == 1u);

    /*
     * Capacity is full. Provider open succeeds, descriptor bind fails, and the
     * newly returned provider identity is closed exactly once.
     */
    assert(turbowasm_wasi_fs_path_open(
               &filesystem,
               3u, 0u,
               second_path, sizeof(second_path),
               0u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u, 0u,
               &opened_fd) == TURBOWASM_WASI_ERRNO_MFILE);
    assert(provider.open_calls == 2u);
    assert(provider.close_calls == 1u);
    assert(provider.last_closed.object == 1002u);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.close_calls == 2u);

    /* Lowest available guest fd is deterministically reused. */
    assert(turbowasm_wasi_fs_path_open(
               &filesystem,
               3u, 0u,
               second_path, sizeof(second_path),
               0u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u, 0u,
               &opened_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(opened_fd == 4u);
    assert(provider.open_calls == 3u);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

static const uint8_t guest_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

    /* type0: path_open
       type1: (i32)->i32
       type2: (i32,i32)->() */
    0x01, 0x18, 0x03,
    0x60, 0x09,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7e, 0x7e, 0x7f, 0x7f,
    0x01, 0x7f,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x00,

    /* import wasi_snapshot_preview1.path_open */
    0x02, 0x24, 0x01,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x09, 0x70, 0x61, 0x74, 0x68, 0x5f, 0x6f, 0x70, 0x65, 0x6e,
    0x00, 0x00,

    /* local read32/store8 */
    0x03, 0x03, 0x02, 0x01, 0x02,

    0x05, 0x03, 0x01, 0x00, 0x01,

    0x0a, 0x13, 0x02,
    0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
    0x09, 0x00,
    0x20, 0x00, 0x20, 0x01,
    0x3a, 0x00, 0x00, 0x0b
};

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static turbowasm_value i64_value(int64_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I64;
    out.as.i64 = value;
    return out;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void guest_store8(
    turbowasm_instance *instance,
    uint32_t address,
    uint32_t value) {
    turbowasm_value args[2] = {
        i32_value((int32_t)address),
        i32_value((int32_t)value)
    };
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 2u,
               args, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static int32_t guest_read32(
    turbowasm_instance *instance,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    return invoke_i32(instance, 1u, &arg, 1u);
}

static int32_t guest_path_open(
    turbowasm_instance *instance,
    uint32_t directory_fd,
    uint32_t path_address,
    uint32_t path_length,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t output_address) {
    turbowasm_value args[9] = {
        i32_value((int32_t)directory_fd),
        i32_value(0),
        i32_value((int32_t)path_address),
        i32_value((int32_t)path_length),
        i32_value(0),
        i64_value((int64_t)rights_base),
        i64_value((int64_t)rights_inheriting),
        i32_value(0),
        i32_value((int32_t)output_address)
    };
    return invoke_i32(instance, 0u, args, 9u);
}

static void test_guest_path_open_abi(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider, 3u);
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    uint32_t before;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem,
               3u,
               (turbowasm_wasi_fs_file){20u, 1u},
               true,
               "/",
               TURBOWASM_WASI_RIGHT_PATH_OPEN,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               &root) == TURBOWASM_OK);

    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, guest_module,
               sizeof(guest_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    guest_store8(&instance, 32u, 'a');
    guest_store8(&instance, 33u, 'b');
    guest_store8(&instance, 34u, 'c');

    assert(guest_path_open(
               &instance,
               3u, 32u, 3u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u,
               64u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read32(&instance, 64u) == 4);
    assert(provider.open_calls == 1u);
    assert(provider.last_path_length == 3u);
    assert(memcmp(provider.last_path, "abc", 3u) == 0);

    before = provider.open_calls;
    assert(guest_path_open(
               &instance,
               3u, 65535u, 2u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u,
               64u) == TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.open_calls == before);

    assert(guest_path_open(
               &instance,
               3u, 32u, 3u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u,
               65535u) == TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.open_calls == before);

    assert(guest_path_open(
               &instance,
               3u, 32u, 3u,
               TURBOWASM_WASI_RIGHT_FD_READ,
               0u,
               64u) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.open_calls == before);

    /* The just-opened fd is not a preopen directory. */
    assert(guest_path_open(
               &instance,
               4u, 32u, 3u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u,
               64u) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.open_calls == before);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

static void test_path_open_provider_missing(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider, 1u);
    turbowasm_wasi_fs_descriptor root = {0};
    uint32_t opened_fd = 0u;
    const uint8_t path[] = {'x'};

    config.provider.path_open = NULL;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem,
               3u,
               (turbowasm_wasi_fs_file){30u, 1u},
               true,
               "/",
               TURBOWASM_WASI_RIGHT_PATH_OPEN,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               &root) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_path_open(
               &filesystem,
               3u, 0u,
               path, sizeof(path),
               0u,
               TURBOWASM_WASI_RIGHT_FD_WRITE,
               0u, 0u,
               &opened_fd) == TURBOWASM_WASI_ERRNO_NOSYS);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_low_level_path_open_rights_and_rollback();
    test_guest_path_open_abi();
    test_path_open_provider_missing();
    return 0;
}
