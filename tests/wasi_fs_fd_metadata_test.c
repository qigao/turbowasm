#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t fs3_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
    0x04, 0x7f, 0x7e, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02, 0x6c, 0x03, 0x16, 0x77, 0x61,
    0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f,
    0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x07, 0x66, 0x64, 0x5f,
    0x73, 0x65, 0x65, 0x6b, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f,
    0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65,
    0x76, 0x69, 0x65, 0x77, 0x31, 0x07, 0x66, 0x64, 0x5f, 0x74, 0x65, 0x6c,
    0x6c, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x0f, 0x66, 0x64, 0x5f, 0x66, 0x69, 0x6c, 0x65, 0x73, 0x74,
    0x61, 0x74, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x01, 0x03, 0x06, 0x05, 0x00,
    0x01, 0x01, 0x02, 0x02, 0x04, 0x05, 0x01, 0x70, 0x01, 0x01, 0x01, 0x05,
    0x03, 0x01, 0x00, 0x02, 0x06, 0x08, 0x01, 0x7f, 0x01, 0x41, 0x80, 0x88,
    0x04, 0x0b, 0x07, 0x3f, 0x06, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
    0x02, 0x00, 0x09, 0x63, 0x61, 0x6c, 0x6c, 0x5f, 0x73, 0x65, 0x65, 0x6b,
    0x00, 0x03, 0x09, 0x63, 0x61, 0x6c, 0x6c, 0x5f, 0x74, 0x65, 0x6c, 0x6c,
    0x00, 0x04, 0x09, 0x63, 0x61, 0x6c, 0x6c, 0x5f, 0x73, 0x74, 0x61, 0x74,
    0x00, 0x05, 0x06, 0x6c, 0x6f, 0x61, 0x64, 0x33, 0x32, 0x00, 0x06, 0x05,
    0x6c, 0x6f, 0x61, 0x64, 0x38, 0x00, 0x07, 0x0a, 0x3c, 0x05, 0x10, 0x00,
    0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03, 0x10, 0x80, 0x80, 0x80,
    0x80, 0x00, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x81, 0x80,
    0x80, 0x80, 0x00, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x82,
    0x80, 0x80, 0x80, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00,
    0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

typedef struct fs_probe {
    uint32_t close_calls;
    uint32_t seek_calls;
    uint32_t tell_calls;
    uint32_t stat_calls;
    turbowasm_wasi_fs_file last_file;
    int64_t last_offset;
    uint8_t last_whence;
} fs_probe;

static uint32_t close_file(
    void *context,
    turbowasm_wasi_fs_file file) {
    fs_probe *probe = (fs_probe *)context;
    assert(probe != NULL);
    ++probe->close_calls;
    probe->last_file = file;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t unsupported_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_read != NULL)
        *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_NOSYS;
}

static uint32_t unsupported_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_written != NULL)
        *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_NOSYS;
}

static uint32_t seek_file(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    fs_probe *probe = (fs_probe *)context;

    assert(probe != NULL);
    assert(out_offset != NULL);
    ++probe->seek_calls;
    probe->last_file = file;
    probe->last_offset = offset;
    probe->last_whence = whence;
    *out_offset = UINT64_C(0x1122334455667788);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t tell_file(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    fs_probe *probe = (fs_probe *)context;

    assert(probe != NULL);
    assert(out_offset != NULL);
    ++probe->tell_calls;
    probe->last_file = file;
    *out_offset = UINT64_C(0x8877665544332211);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t stat_file(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    fs_probe *probe = (fs_probe *)context;

    assert(probe != NULL);
    assert(out_stat != NULL);
    ++probe->stat_calls;
    probe->last_file = file;
    out_stat->size = UINT64_C(0x0102030405060708);
    out_stat->modified_ns = UINT64_C(0x1112131415161718);
    out_stat->file_type = 4u; /* regular_file */
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static bool name_equal(
    turbowasm_name name,
    const char *text) {
    size_t length = strlen(text);
    return name.size == length &&
           memcmp(name.bytes, text, length) == 0;
}

static uint32_t export_function(
    const turbowasm_module *module,
    const char *name) {
    size_t index;

    for (index = 0u;
         index < turbowasm_module_export_count(module);
         ++index) {
        const turbowasm_export_desc *desc =
            turbowasm_module_export_at(module, index);
        if (desc != NULL &&
            desc->kind == TURBOWASM_EXTERN_FUNCTION &&
            name_equal(desc->name, name))
            return desc->item_index;
    }
    assert(!"missing function export");
    return UINT32_MAX;
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

static uint32_t load32(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    return (uint32_t)invoke_i32(
        instance, function_index, &argument, 1u);
}

static uint8_t load8(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    return (uint8_t)invoke_i32(
        instance, function_index, &argument, 1u);
}

static void test_fd_seek_tell_filestat(void) {
    fs_probe probe = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config fs_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    uint32_t call_seek;
    uint32_t call_tell;
    uint32_t call_stat;
    uint32_t read32;
    uint32_t read8;
    turbowasm_value seek_args[4];
    turbowasm_value pair_args[2];
    uint32_t before;

    fs_config.descriptor_capacity = 3u;
    fs_config.provider.context = &probe;
    fs_config.provider.close = close_file;
    fs_config.provider.read = unsupported_read;
    fs_config.provider.write = unsupported_write;
    fs_config.provider.seek = seek_file;
    fs_config.provider.tell = tell_file;
    fs_config.provider.stat = stat_file;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &fs_config) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem, 5u,
               (turbowasm_wasi_fs_file){101u, 1u},
               false, NULL,
               TURBOWASM_WASI_RIGHT_FD_SEEK |
                   TURBOWASM_WASI_RIGHT_FD_TELL |
                   TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET,
               0u, NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem, 6u,
               (turbowasm_wasi_fs_file){102u, 1u},
               false, NULL,
               TURBOWASM_WASI_RIGHT_FD_TELL,
               0u, NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem, 7u,
               (turbowasm_wasi_fs_file){103u, 1u},
               false, NULL,
               0u, 0u, NULL) == TURBOWASM_OK);

    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, fs3_module,
               sizeof(fs3_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    call_seek = export_function(&module, "call_seek");
    call_tell = export_function(&module, "call_tell");
    call_stat = export_function(&module, "call_stat");
    read32 = export_function(&module, "load32");
    read8 = export_function(&module, "load8");

    seek_args[0] = i32_value(5);
    seek_args[1] = i64_value(-7);
    seek_args[2] = i32_value(TURBOWASM_WASI_WHENCE_END);
    seek_args[3] = i32_value(128);
    assert(invoke_i32(
               &instance, call_seek,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.seek_calls == 1u);
    assert(probe.last_file.object == 101u);
    assert(probe.last_offset == -7);
    assert(probe.last_whence == TURBOWASM_WASI_WHENCE_END);
    assert(load32(&instance, read32, 128u) == UINT32_C(0x55667788));
    assert(load32(&instance, read32, 132u) == UINT32_C(0x11223344));

    pair_args[0] = i32_value(5);
    pair_args[1] = i32_value(160);
    assert(invoke_i32(
               &instance, call_tell,
               pair_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.tell_calls == 1u);
    assert(load32(&instance, read32, 160u) == UINT32_C(0x44332211));
    assert(load32(&instance, read32, 164u) == UINT32_C(0x88776655));

    /* fd_seek(fd, 0, CUR) may use FD_TELL alone and routes to tell(). */
    before = probe.tell_calls;
    seek_args[0] = i32_value(6);
    seek_args[1] = i64_value(0);
    seek_args[2] = i32_value(TURBOWASM_WASI_WHENCE_CUR);
    seek_args[3] = i32_value(176);
    assert(invoke_i32(
               &instance, call_seek,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.tell_calls == before + 1u);
    assert(probe.seek_calls == 1u);

    /* Non-zero seek still requires FD_SEEK. */
    seek_args[1] = i64_value(1);
    assert(invoke_i32(
               &instance, call_seek,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(probe.seek_calls == 1u);

    /* Guest i32 whence is validated before narrowing to provider uint8_t. */
    before = probe.seek_calls + probe.tell_calls;
    seek_args[0] = i32_value(5);
    seek_args[1] = i64_value(0);
    seek_args[2] = i32_value(256);
    assert(invoke_i32(
               &instance, call_seek,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(probe.seek_calls + probe.tell_calls == before);

    /* Output range is validated before provider side effects. */
    before = probe.seek_calls;
    seek_args[2] = i32_value(TURBOWASM_WASI_WHENCE_SET);
    seek_args[3] = i32_value(131068);
    assert(invoke_i32(
               &instance, call_seek,
               seek_args, 4u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(probe.seek_calls == before);

    pair_args[0] = i32_value(5);
    pair_args[1] = i32_value(256);
    assert(invoke_i32(
               &instance, call_stat,
               pair_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.stat_calls == 1u);
    assert(load32(&instance, read32, 256u) == 0u); /* dev */
    assert(load32(&instance, read32, 264u) == 0u); /* ino */
    assert(load8(&instance, read8, 272u) == 4u);   /* regular_file */
    assert(load32(&instance, read32, 280u) == 0u); /* nlink */
    assert(load32(&instance, read32, 288u) == UINT32_C(0x05060708));
    assert(load32(&instance, read32, 292u) == UINT32_C(0x01020304));
    assert(load32(&instance, read32, 296u) == 0u); /* atim */
    assert(load32(&instance, read32, 304u) == UINT32_C(0x15161718));
    assert(load32(&instance, read32, 308u) == UINT32_C(0x11121314));
    assert(load32(&instance, read32, 312u) == 0u); /* ctim */

    pair_args[0] = i32_value(7);
    pair_args[1] = i32_value(352);
    assert(invoke_i32(
               &instance, call_stat,
               pair_args, 2u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(probe.stat_calls == 1u);

    pair_args[0] = i32_value(99);
    assert(invoke_i32(
               &instance, call_tell,
               pair_args, 2u) ==
           TURBOWASM_WASI_ERRNO_BADF);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 5u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 6u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 7u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.close_calls == 3u);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_fd_seek_tell_filestat();
    return 0;
}
