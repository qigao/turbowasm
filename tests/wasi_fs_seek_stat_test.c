#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x19, 0x04, 0x60,
    0x04, 0x7f, 0x7e, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7e, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
    0x6c, 0x03, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77,
    0x31, 0x07, 0x66, 0x64, 0x5f, 0x73, 0x65, 0x65, 0x6b, 0x00, 0x00, 0x16,
    0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f,
    0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x07, 0x66,
    0x64, 0x5f, 0x74, 0x65, 0x6c, 0x6c, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70,
    0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0f, 0x66, 0x64, 0x5f, 0x66,
    0x69, 0x6c, 0x65, 0x73, 0x74, 0x61, 0x74, 0x5f, 0x67, 0x65, 0x74, 0x00,
    0x01, 0x03, 0x04, 0x03, 0x02, 0x03, 0x03, 0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x19, 0x03, 0x07, 0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b, 0x07,
    0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d,
    0x00, 0x00, 0x0b
};

typedef struct fake_provider {
    uint32_t close_calls;
    uint32_t seek_calls;
    uint32_t tell_calls;
    uint32_t stat_calls;
    turbowasm_wasi_fs_file last_file;
    int64_t last_seek_offset;
    uint8_t last_whence;
} fake_provider;

static uint32_t close_file(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_provider *provider = (fake_provider *)context;
    assert(provider != NULL);
    ++provider->close_calls;
    provider->last_file = file;
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

static uint32_t seek_file(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    fake_provider *provider = (fake_provider *)context;

    assert(provider != NULL);
    assert(out_offset != NULL);
    ++provider->seek_calls;
    provider->last_file = file;
    provider->last_seek_offset = offset;
    provider->last_whence = whence;
    *out_offset = UINT64_C(0x1122334455667788);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t tell_file(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    fake_provider *provider = (fake_provider *)context;

    assert(provider != NULL);
    assert(out_offset != NULL);
    ++provider->tell_calls;
    provider->last_file = file;
    *out_offset = UINT64_C(0x8877665544332211);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t stat_file(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    fake_provider *provider = (fake_provider *)context;

    assert(provider != NULL);
    assert(out_stat != NULL);
    ++provider->stat_calls;
    provider->last_file = file;

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

static turbowasm_wasi_fs_config fs_config(
    fake_provider *provider) {
    turbowasm_wasi_fs_config config = {0};

    config.descriptor_capacity = 2u;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.seek = seek_file;
    config.provider.tell = tell_file;
    config.provider.stat = stat_file;
    return config;
}

static void bind_files(
    turbowasm_wasi_fs *filesystem) {
    turbowasm_wasi_fs_descriptor descriptor = {0};

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               3u,
               (turbowasm_wasi_fs_file){100u, 1u},
               false,
               NULL,
               TURBOWASM_WASI_RIGHT_FD_SEEK |
                   TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET,
               0u,
               &descriptor) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               filesystem,
               4u,
               (turbowasm_wasi_fs_file){200u, 1u},
               false,
               NULL,
               0u,
               0u,
               &descriptor) == TURBOWASM_OK);
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static turbowasm_value i64_value(int64_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I64;
    result.as.i64 = value;
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
               instance, 3u,
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
               instance, 5u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(trap == TURBOWASM_TRAP_NONE);
    return (uint32_t)result.as.i32;
}

static void test_low_level_rights_and_optional_provider(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_fs_stat stat = {0};
    uint64_t offset = 0u;
    uint32_t before;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_files(&filesystem);

    assert(turbowasm_wasi_fs_fd_seek(
               &filesystem, 3u, -9,
               TURBOWASM_WASI_WHENCE_END,
               &offset) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(offset == UINT64_C(0x1122334455667788));
    assert(provider.seek_calls == 1u);
    assert(provider.last_seek_offset == -9);
    assert(provider.last_whence ==
           TURBOWASM_WASI_WHENCE_END);

    /*
     * Preview1 specifies FD_SEEK implies FD_TELL, so fd 3 can tell without
     * carrying a separate FD_TELL bit.
     */
    assert(turbowasm_wasi_fs_fd_tell(
               &filesystem, 3u, &offset) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(offset == UINT64_C(0x8877665544332211));
    assert(provider.tell_calls == 1u);

    assert(turbowasm_wasi_fs_fd_stat(
               &filesystem, 3u, &stat) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(stat.size == UINT64_C(0x1020304050607080));
    assert(stat.modified_ns == 200u);
    assert(stat.file_type ==
           TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(provider.stat_calls == 1u);

    before = provider.seek_calls;
    assert(turbowasm_wasi_fs_fd_seek(
               &filesystem, 3u, 0, 9u, &offset) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(provider.seek_calls == before);

    assert(turbowasm_wasi_fs_fd_seek(
               &filesystem, 4u, 0,
               TURBOWASM_WASI_WHENCE_SET,
               &offset) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(turbowasm_wasi_fs_fd_tell(
               &filesystem, 4u, &offset) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(turbowasm_wasi_fs_fd_stat(
               &filesystem, 4u, &stat) ==
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

static void test_guest_seek_tell_filestat_abi(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value seek_args[4];
    turbowasm_value two_args[2];
    uint32_t before;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    bind_files(&filesystem);

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

    seek_args[0] = i32_value(3);
    seek_args[1] = i64_value(-5);
    seek_args[2] = i32_value(TURBOWASM_WASI_WHENCE_END);
    seek_args[3] = i32_value(0);
    assert(invoke_errno(
               &instance, 0u,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read64(&instance, 0u) ==
           UINT64_C(0x1122334455667788));
    assert(provider.seek_calls == 1u);

    two_args[0] = i32_value(3);
    two_args[1] = i32_value(8);
    assert(invoke_errno(
               &instance, 1u,
               two_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read64(&instance, 8u) ==
           UINT64_C(0x8877665544332211));
    assert(provider.tell_calls == 1u);

    two_args[1] = i32_value(64);
    assert(invoke_errno(
               &instance, 2u,
               two_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
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
    assert(provider.stat_calls == 1u);

    /* Output range validation occurs before any stateful provider call. */
    before = provider.seek_calls;
    seek_args[3] = i32_value(65532);
    assert(invoke_errno(
               &instance, 0u,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.seek_calls == before);

    seek_args[2] = i32_value(256);
    seek_args[3] = i32_value(0);
    assert(invoke_errno(
               &instance, 0u,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(provider.seek_calls == before);

    before = provider.tell_calls;
    two_args[0] = i32_value(3);
    two_args[1] = i32_value(65532);
    assert(invoke_errno(
               &instance, 1u,
               two_args, 2u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.tell_calls == before);

    before = provider.stat_calls;
    two_args[1] = i32_value(65504);
    assert(invoke_errno(
               &instance, 2u,
               two_args, 2u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.stat_calls == before);

    two_args[0] = i32_value(4);
    two_args[1] = i32_value(64);
    assert(invoke_errno(
               &instance, 2u,
               two_args, 2u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.stat_calls == before);

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
    test_low_level_rights_and_optional_provider();
    test_guest_seek_tell_filestat_abi();
    return 0;
}
