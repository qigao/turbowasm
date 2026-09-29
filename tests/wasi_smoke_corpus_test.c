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

/*
 * Checked-in binary fixtures intentionally keep ordinary CI independent of a
 * particular wasi-sdk/WABT version. Each module has one guest "start" function
 * that composes several Preview1 imports, followed by tiny memory readers used
 * only by the host-side assertions.
 */
static const uint8_t process_info_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1c, 0x05, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7e, 0x7f, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f,
    0x01, 0x7e, 0x02, 0xa0, 0x01, 0x04, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f,
    0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65,
    0x76, 0x69, 0x65, 0x77, 0x31, 0x0e, 0x61, 0x72, 0x67, 0x73, 0x5f, 0x73,
    0x69, 0x7a, 0x65, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77,
    0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74,
    0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x11, 0x65, 0x6e,
    0x76, 0x69, 0x72, 0x6f, 0x6e, 0x5f, 0x73, 0x69, 0x7a, 0x65, 0x73, 0x5f,
    0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73,
    0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76,
    0x69, 0x65, 0x77, 0x31, 0x0e, 0x63, 0x6c, 0x6f, 0x63, 0x6b, 0x5f, 0x74,
    0x69, 0x6d, 0x65, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x01, 0x16, 0x77, 0x61,
    0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f,
    0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0a, 0x72, 0x61, 0x6e,
    0x64, 0x6f, 0x6d, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x03, 0x05, 0x04,
    0x02, 0x03, 0x04, 0x03, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0x3c, 0x04,
    0x22, 0x00, 0x41, 0x00, 0x41, 0x04, 0x10, 0x00, 0x1a, 0x41, 0x08, 0x41,
    0x0c, 0x10, 0x01, 0x1a, 0x41, 0x01, 0x42, 0x00, 0x41, 0x10, 0x10, 0x02,
    0x1a, 0x41, 0x18, 0x41, 0x08, 0x10, 0x03, 0x1a, 0x41, 0x00, 0x0b, 0x07,
    0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x29,
    0x03, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

static const uint8_t stdio_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x12, 0x03, 0x60,
    0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x02, 0x44, 0x02, 0x16, 0x77, 0x61, 0x73, 0x69,
    0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72,
    0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x08, 0x66, 0x64, 0x5f, 0x77, 0x72,
    0x69, 0x74, 0x65, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73,
    0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76,
    0x69, 0x65, 0x77, 0x31, 0x07, 0x66, 0x64, 0x5f, 0x72, 0x65, 0x61, 0x64,
    0x00, 0x00, 0x03, 0x04, 0x03, 0x01, 0x02, 0x02, 0x05, 0x03, 0x01, 0x00,
    0x01, 0x0a, 0x56, 0x03, 0x44, 0x00, 0x41, 0x00, 0x41, 0x30, 0x36, 0x02,
    0x00, 0x41, 0x04, 0x41, 0x02, 0x36, 0x02, 0x00, 0x41, 0x30, 0x41, 0x31,
    0x3a, 0x00, 0x00, 0x41, 0x31, 0x41, 0x32, 0x3a, 0x00, 0x00, 0x41, 0x08,
    0x41, 0x34, 0x36, 0x02, 0x00, 0x41, 0x0c, 0x41, 0x03, 0x36, 0x02, 0x00,
    0x41, 0x01, 0x41, 0x00, 0x41, 0x01, 0x41, 0x10, 0x10, 0x00, 0x1a, 0x41,
    0x00, 0x41, 0x08, 0x41, 0x01, 0x41, 0x14, 0x10, 0x01, 0x1a, 0x41, 0x00,
    0x0b, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x20,
    0x00, 0x2d, 0x00, 0x00, 0x0b
};

static const uint8_t fs_query_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x25, 0x06, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x05, 0x7f, 0x7f, 0x7f, 0x7e, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01,
    0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7e, 0x02,
    0x7a, 0x03, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77,
    0x31, 0x0e, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65, 0x73, 0x74, 0x61, 0x74,
    0x5f, 0x67, 0x65, 0x74, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f,
    0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65,
    0x76, 0x69, 0x65, 0x77, 0x31, 0x13, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65,
    0x73, 0x74, 0x61, 0x74, 0x5f, 0x64, 0x69, 0x72, 0x5f, 0x6e, 0x61, 0x6d,
    0x65, 0x00, 0x01, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69, 0x65,
    0x77, 0x31, 0x0a, 0x66, 0x64, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x64, 0x69,
    0x72, 0x00, 0x02, 0x03, 0x05, 0x04, 0x03, 0x04, 0x05, 0x04, 0x05, 0x03,
    0x01, 0x00, 0x01, 0x0a, 0x3b, 0x04, 0x21, 0x00, 0x41, 0x03, 0x41, 0x00,
    0x10, 0x00, 0x1a, 0x41, 0x03, 0x41, 0x08, 0x41, 0x01, 0x10, 0x01, 0x1a,
    0x41, 0x03, 0x41, 0x20, 0x41, 0x20, 0x42, 0x00, 0x41, 0x18, 0x10, 0x02,
    0x1a, 0x41, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
    0x07, 0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00,
    0x2d, 0x00, 0x00, 0x0b
};

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static turbowasm_value invoke_one(
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
    return result;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result =
        invoke_one(instance, function_index, NULL, 0u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static uint32_t guest_read32(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    turbowasm_value result =
        invoke_one(instance, function_index, &arg, 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return (uint32_t)result.as.i32;
}

static uint64_t guest_read64(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    turbowasm_value result =
        invoke_one(instance, function_index, &arg, 1u);
    assert(result.kind == TURBOWASM_VALUE_I64);
    return (uint64_t)result.as.i64;
}

static uint32_t guest_read8(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    return guest_read32(instance, function_index, address) & 0xffu;
}

static void setup_instance(
    const uint8_t *bytes,
    size_t byte_count,
    turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config *config,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    turbowasm_linker linker = {0};

    assert(turbowasm_wasi_preview1_init(wasi, config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
        module, bytes, byte_count) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
        instance, module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

typedef struct process_probe {
    uint32_t clock_calls;
    uint32_t random_calls;
} process_probe;

static uint32_t smoke_clock(
    void *context,
    uint32_t clock_id,
    uint64_t precision_ns,
    uint64_t *out_timestamp_ns) {
    process_probe *probe = (process_probe *)context;
    assert(probe != NULL);
    assert(out_timestamp_ns != NULL);
    ++probe->clock_calls;
    assert(clock_id == TURBOWASM_WASI_CLOCKID_MONOTONIC);
    assert(precision_ns == 0u);
    *out_timestamp_ns = UINT64_C(0x1122334455667788);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t smoke_random(
    void *context,
    uint8_t *buffer,
    size_t length) {
    process_probe *probe = (process_probe *)context;
    size_t index;
    assert(probe != NULL);
    assert(length == 8u);
    ++probe->random_calls;
    for (index = 0u; index < length; ++index)
        buffer[index] = (uint8_t)(0xa0u + index);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void test_process_info_smoke(void) {
    const char *args[] = {"prog", "x"};
    const char *environment[] = {"A=B"};
    process_probe probe = {0};
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    config.allow_args = true;
    config.args = args;
    config.arg_count = 2u;
    config.allow_environ = true;
    config.environment = environment;
    config.environment_count = 1u;
    config.allow_clock = true;
    config.clock_time = smoke_clock;
    config.clock_context = &probe;
    config.allow_random = true;
    config.random_fill = smoke_random;
    config.random_context = &probe;

    setup_instance(
        process_info_module, sizeof(process_info_module),
        &wasi, &config, &module, &instance);

    assert(invoke_i32(&instance, 4u) == 0);
    assert(guest_read32(&instance, 5u, 0u) == 2u);
    assert(guest_read32(&instance, 5u, 4u) == 7u);
    assert(guest_read32(&instance, 5u, 8u) == 1u);
    assert(guest_read32(&instance, 5u, 12u) == 4u);
    assert(guest_read64(&instance, 6u, 16u) ==
           UINT64_C(0x1122334455667788));
    assert(guest_read8(&instance, 7u, 24u) == 0xa0u);
    assert(guest_read8(&instance, 7u, 31u) == 0xa7u);
    assert(probe.clock_calls == 1u);
    assert(probe.random_calls == 1u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

typedef struct stdio_probe {
    uint32_t write_calls;
    uint32_t read_calls;
} stdio_probe;

static uint32_t smoke_fd_write(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL);
    assert(fd == 1u);
    assert(buffer_count == 1u);
    assert(buffers != NULL);
    assert(buffers[0].size == 2u);
    assert(memcmp(buffers[0].data, "12", 2u) == 0);
    assert(out_written != NULL);
    ++probe->write_calls;
    *out_written = 2u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t smoke_fd_read(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    stdio_probe *probe = (stdio_probe *)context;
    assert(probe != NULL);
    assert(fd == 0u);
    assert(buffer_count == 1u);
    assert(buffers != NULL);
    assert(buffers[0].size == 3u);
    assert(out_read != NULL);
    ++probe->read_calls;
    memcpy(buffers[0].data, "345", 3u);
    *out_read = 3u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void test_stdio_smoke(void) {
    stdio_probe probe = {0};
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    config.allow_fd_write = true;
    config.fd_write = smoke_fd_write;
    config.fd_write_context = &probe;
    config.allow_fd_read = true;
    config.fd_read = smoke_fd_read;
    config.fd_read_context = &probe;

    setup_instance(
        stdio_module, sizeof(stdio_module),
        &wasi, &config, &module, &instance);

    assert(invoke_i32(&instance, 2u) == 0);
    assert(probe.write_calls == 1u);
    assert(probe.read_calls == 1u);
    assert(guest_read32(&instance, 3u, 16u) == 2u);
    assert(guest_read32(&instance, 3u, 20u) == 3u);
    assert(guest_read8(&instance, 4u, 52u) == '3');
    assert(guest_read8(&instance, 4u, 53u) == '4');
    assert(guest_read8(&instance, 4u, 54u) == '5');

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

typedef struct fs_probe {
    uint32_t close_calls;
    uint32_t readdir_calls;
} fs_probe;

static uint32_t smoke_fs_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    fs_probe *probe = (fs_probe *)context;
    assert(probe != NULL);
    assert(file.object == UINT64_C(1));
    ++probe->close_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t smoke_fs_read(
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

static uint32_t smoke_fs_write(
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

static uint32_t smoke_fs_readdir(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    fs_probe *probe = (fs_probe *)context;
    assert(probe != NULL);
    assert(directory.object == UINT64_C(1));
    assert(out_entry != NULL);
    assert(out_has_entry != NULL);
    ++probe->readdir_calls;

    *out_entry = (turbowasm_wasi_fs_dirent){0};
    if (cookie == 0u) {
        *out_has_entry = true;
        out_entry->next_cookie = 1u;
        out_entry->inode = UINT64_C(0x22);
        out_entry->name_length = 1u;
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
        out_entry->name[0] = 'x';
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (cookie == 1u) {
        *out_has_entry = false;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    return TURBOWASM_WASI_ERRNO_INVAL;
}

static void test_preopen_memory_fs_smoke(void) {
    fs_probe probe = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config fs_config = {0};
    turbowasm_wasi_fs_descriptor descriptor = {0};
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    fs_config.descriptor_capacity = 1u;
    fs_config.provider.context = &probe;
    fs_config.provider.close = smoke_fs_close;
    fs_config.provider.read = smoke_fs_read;
    fs_config.provider.write = smoke_fs_write;
    fs_config.provider.readdir = smoke_fs_readdir;
    assert(turbowasm_wasi_fs_init(
        &filesystem, &fs_config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        (turbowasm_wasi_fs_file){UINT64_C(1), 1u},
        true,
        "/",
        TURBOWASM_WASI_RIGHT_FD_READDIR,
        0u,
        &descriptor) == TURBOWASM_OK);

    config.allow_filesystem = true;
    config.filesystem = &filesystem;
    setup_instance(
        fs_query_module, sizeof(fs_query_module),
        &wasi, &config, &module, &instance);

    assert(invoke_i32(&instance, 3u) == 0);
    assert(guest_read8(&instance, 6u, 0u) == 0u);
    assert(guest_read32(&instance, 4u, 4u) == 1u);
    assert(guest_read8(&instance, 6u, 8u) == '/');
    assert(guest_read32(&instance, 4u, 24u) == 25u);
    assert(guest_read64(&instance, 5u, 32u) == 1u);
    assert(guest_read64(&instance, 5u, 40u) == UINT64_C(0x22));
    assert(guest_read32(&instance, 4u, 48u) == 1u);
    assert(guest_read8(&instance, 6u, 52u) ==
           TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(guest_read8(&instance, 6u, 56u) == 'x');
    assert(probe.readdir_calls == 2u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.close_calls == 1u);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_process_info_smoke();
    test_stdio_smoke();
    test_preopen_memory_fs_smoke();
    return 0;
}
