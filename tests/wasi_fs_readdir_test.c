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
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
    0x05, 0x7f, 0x7f, 0x7f, 0x7e, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01,
    0x7e, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02, 0x25, 0x01, 0x16, 0x77, 0x61,
    0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f,
    0x70, 0x72, 0x65, 0x76, 0x69, 0x65, 0x77, 0x31, 0x0a, 0x66, 0x64, 0x5f,
    0x72, 0x65, 0x61, 0x64, 0x64, 0x69, 0x72, 0x00, 0x00, 0x03, 0x04, 0x03,
    0x01, 0x02, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0x19, 0x03, 0x07,
    0x00, 0x20, 0x00, 0x29, 0x00, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28,
    0x00, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b,
};

enum provider_mode {
    PROVIDER_NORMAL = 0,
    PROVIDER_OVERSIZED_NAME,
    PROVIDER_STAGNANT_COOKIE
};

typedef struct fake_provider {
    uint32_t readdir_calls;
    uint64_t last_cookie;
    turbowasm_wasi_fs_file last_directory;
    uint32_t next_error;
    enum provider_mode mode;
} fake_provider;

static uint32_t close_file(void *context, turbowasm_wasi_fs_file file) {
    (void)context;
    (void)file;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t read_file(
    void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t buffer_count,
    uint32_t *out_read) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    assert(out_read != NULL);
    *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t write_file(
    void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t buffer_count,
    uint32_t *out_written) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    assert(out_written != NULL);
    *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t readdir_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    fake_provider *provider = (fake_provider *)context;
    uint32_t error;

    assert(provider != NULL);
    assert(out_entry != NULL);
    assert(out_has_entry != NULL);
    ++provider->readdir_calls;
    provider->last_cookie = cookie;
    provider->last_directory = directory;

    error = provider->next_error;
    provider->next_error = 0u;
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    *out_entry = (turbowasm_wasi_fs_dirent){0};
    *out_has_entry = true;

    if (provider->mode == PROVIDER_OVERSIZED_NAME) {
        out_entry->next_cookie = cookie + 1u;
        out_entry->name_length =
            TURBOWASM_WASI_FS_DIRENT_NAME_MAX + 1u;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (provider->mode == PROVIDER_STAGNANT_COOKIE) {
        out_entry->next_cookie = cookie;
        out_entry->name_length = 1u;
        out_entry->name[0] = 'x';
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    if (cookie == 0u) {
        out_entry->next_cookie = 10u;
        out_entry->inode = UINT64_C(0x11);
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_DIRECTORY;
        out_entry->name_length = 3u;
        out_entry->name[0] = 'a';
        out_entry->name[1] = 0u;
        out_entry->name[2] = 'b';
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (cookie == 10u) {
        out_entry->next_cookie = 20u;
        out_entry->inode = UINT64_C(0x22);
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
        out_entry->name_length = 2u;
        out_entry->name[0] = 'x';
        out_entry->name[1] = 'y';
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (cookie == 20u) {
        *out_has_entry = false;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    return TURBOWASM_WASI_ERRNO_INVAL;
}

static turbowasm_wasi_fs_config fs_config(fake_provider *provider) {
    turbowasm_wasi_fs_config config = {0};
    config.descriptor_capacity = 2u;
    config.provider.context = provider;
    config.provider.close = close_file;
    config.provider.read = read_file;
    config.provider.write = write_file;
    config.provider.readdir = readdir_file;
    return config;
}

static void bind_directories(turbowasm_wasi_fs *filesystem) {
    turbowasm_wasi_fs_descriptor descriptor = {0};

    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 3u, (turbowasm_wasi_fs_file){100u, 1u},
        true, "/dir", TURBOWASM_WASI_RIGHT_FD_READDIR,
        0u, &descriptor) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem, 4u, (turbowasm_wasi_fs_file){200u, 1u},
        true, "/none", 0u, 0u, &descriptor) == TURBOWASM_OK);
}

static void close_directories(turbowasm_wasi_fs *filesystem) {
    assert(turbowasm_wasi_fs_close_fd(
        filesystem, 4u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
        filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
}

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

static int32_t invoke_readdir(
    turbowasm_instance *instance,
    uint32_t fd,
    uint32_t buffer,
    uint32_t buffer_length,
    uint64_t cookie,
    uint32_t bufused) {
    turbowasm_value arguments[5] = {
        i32_value((int32_t)fd),
        i32_value((int32_t)buffer),
        i32_value((int32_t)buffer_length),
        i64_value((int64_t)cookie),
        i32_value((int32_t)bufused)
    };
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
        instance, 0u, arguments, 5u,
        &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static uint64_t guest_read64(
    turbowasm_instance *instance, uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
        instance, 1u, &argument, 1u, &result, 1u,
        &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.kind == TURBOWASM_VALUE_I64);
    return (uint64_t)result.as.i64;
}

static uint32_t guest_read32(
    turbowasm_instance *instance, uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
        instance, 2u, &argument, 1u, &result, 1u,
        &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.kind == TURBOWASM_VALUE_I32);
    return (uint32_t)result.as.i32;
}

static uint32_t guest_read8(
    turbowasm_instance *instance, uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
        instance, 3u, &argument, 1u, &result, 1u,
        &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.kind == TURBOWASM_VALUE_I32);
    return (uint32_t)result.as.i32;
}

static void test_low_level_readdir(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_fs_dirent entry = {0};
    bool has_entry = false;

    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);

    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 0u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(has_entry);
    assert(provider.last_directory.object == 100u);
    assert(entry.next_cookie == 10u);
    assert(entry.inode == UINT64_C(0x11));
    assert(entry.name_length == 3u);
    assert(entry.name[0] == 'a' && entry.name[1] == 0u &&
           entry.name[2] == 'b');

    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 4u, 0u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_NOTCAPABLE);

    provider.next_error = TURBOWASM_WASI_ERRNO_IO;
    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 0u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_IO);

    provider.mode = PROVIDER_OVERSIZED_NAME;
    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 0u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_NAMETOOLONG);

    provider.mode = PROVIDER_STAGNANT_COOKIE;
    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 7u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_INVAL);

    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);

    config.provider.readdir = NULL;
    filesystem = (turbowasm_wasi_fs){0};
    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);
    bind_directories(&filesystem);
    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 0u, &entry, &has_entry) ==
        TURBOWASM_WASI_ERRNO_NOSYS);
    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);
}

static void test_guest_readdir(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = fs_config(&provider);
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

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

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 64u, 64u, 0u, 32u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.readdir_calls == 3u);
    assert(guest_read32(&instance, 32u) == 53u);
    assert(guest_read64(&instance, 64u) == 10u);
    assert(guest_read64(&instance, 72u) == UINT64_C(0x11));
    assert(guest_read32(&instance, 80u) == 3u);
    assert(guest_read8(&instance, 84u) == TURBOWASM_WASI_FILETYPE_DIRECTORY);
    assert(guest_read8(&instance, 88u) == 'a');
    assert(guest_read8(&instance, 89u) == 0u);
    assert(guest_read8(&instance, 90u) == 'b');
    assert(guest_read64(&instance, 91u) == 20u);
    assert(guest_read64(&instance, 99u) == UINT64_C(0x22));
    assert(guest_read32(&instance, 107u) == 2u);
    assert(guest_read8(&instance, 111u) ==
           TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(guest_read8(&instance, 115u) == 'x');
    assert(guest_read8(&instance, 116u) == 'y');

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 128u, 10u, 0u, 40u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.readdir_calls == 1u);
    assert(guest_read32(&instance, 40u) == 10u);
    assert(guest_read64(&instance, 128u) == 10u);

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 160u, 27u, 0u, 44u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.readdir_calls == 1u);
    assert(guest_read32(&instance, 44u) == 27u);

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 192u, 0u, 0u, 48u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.readdir_calls == 0u);
    assert(guest_read32(&instance, 48u) == 0u);

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 65535u, 2u, 0u, 52u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.readdir_calls == 0u);

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 3u, 224u, 10u, 0u, 65534u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(provider.readdir_calls == 0u);

    provider.readdir_calls = 0u;
    assert(invoke_readdir(&instance, 4u, 224u, 10u, 0u, 56u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(provider.readdir_calls == 0u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    close_directories(&filesystem);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_low_level_readdir();
    test_guest_readdir();
    return 0;
}
