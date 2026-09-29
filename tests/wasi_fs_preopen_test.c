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
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

    /* type0: (i32)->i32
       type1: (i32,i32)->i32
       type2: (i32,i32,i32)->i32
       type3: (i32)->i32
       type4: (i32,i32)->() */
    0x01, 0x1d, 0x05,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x00,

    /* imports: fd_close, fd_prestat_get, fd_prestat_dir_name */
    0x02, 0x78, 0x03,

    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x08, 0x66, 0x64, 0x5f, 0x63, 0x6c, 0x6f, 0x73, 0x65,
    0x00, 0x00,

    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x0e, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65, 0x73, 0x74, 0x61,
    0x74, 0x5f, 0x67, 0x65, 0x74,
    0x00, 0x01,

    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x13, 0x66, 0x64, 0x5f, 0x70, 0x72, 0x65, 0x73, 0x74, 0x61,
    0x74, 0x5f, 0x64, 0x69, 0x72, 0x5f, 0x6e, 0x61, 0x6d, 0x65,
    0x00, 0x02,

    /* local read32/read8/store8 */
    0x03, 0x04, 0x03, 0x03, 0x03, 0x04,

    /* memory0 min=1 */
    0x05, 0x03, 0x01, 0x00, 0x01,

    /* code */
    0x0a, 0x1b, 0x03,

    /* f3 read32(addr) */
    0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,

    /* f4 read8(addr) */
    0x07, 0x00,
    0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b,

    /* f5 store8(addr,value) */
    0x09, 0x00,
    0x20, 0x00, 0x20, 0x01,
    0x3a, 0x00, 0x00, 0x0b
};

typedef struct fake_provider {
    uint32_t close_calls;
    turbowasm_wasi_fs_file last_closed;
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

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
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

static int32_t guest_read(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    return invoke_i32(instance, function_index, &arg, 1u);
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
               instance, 5u,
               args, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static int32_t invoke_close(
    turbowasm_instance *instance,
    uint32_t fd) {
    turbowasm_value arg = i32_value((int32_t)fd);
    return invoke_i32(instance, 0u, &arg, 1u);
}

static int32_t invoke_prestat(
    turbowasm_instance *instance,
    uint32_t fd,
    uint32_t output) {
    turbowasm_value args[2] = {
        i32_value((int32_t)fd),
        i32_value((int32_t)output)
    };
    return invoke_i32(instance, 1u, args, 2u);
}

static int32_t invoke_dirname(
    turbowasm_instance *instance,
    uint32_t fd,
    uint32_t output,
    uint32_t length) {
    turbowasm_value args[3] = {
        i32_value((int32_t)fd),
        i32_value((int32_t)output),
        i32_value((int32_t)length)
    };
    return invoke_i32(instance, 2u, args, 3u);
}

static void test_preopen_and_close(void) {
    fake_provider provider = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config fs_config = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi_fs_descriptor ordinary = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    fs_config.descriptor_capacity = 2u;
    fs_config.provider.context = &provider;
    fs_config.provider.close = close_file;
    fs_config.provider.read = read_file;
    fs_config.provider.write = write_file;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &fs_config) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               3u,
               (turbowasm_wasi_fs_file){100u, 1u},
               true,
               "/sandbox",
               &root) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               4u,
               (turbowasm_wasi_fs_file){200u, 1u},
               false,
               NULL,
               &ordinary) == TURBOWASM_OK);

    wasi_config.allow_filesystem = true;
    wasi_config.filesystem = &filesystem;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    assert(invoke_prestat(
               &instance, 3u, 0u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 0u) == 0);
    assert(guest_read(&instance, 4u, 1u) == 0);
    assert(guest_read(&instance, 3u, 4u) == 8);

    /* Mark byte after the name to prove no implicit NUL is copied. */
    guest_store8(&instance, 40u, 0x7fu);
    assert(invoke_dirname(
               &instance, 3u, 32u, 9u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 32u) == '/');
    assert(guest_read(&instance, 4u, 33u) == 's');
    assert(guest_read(&instance, 4u, 39u) == 'x');
    assert(guest_read(&instance, 4u, 40u) == 0x7f);

    assert(invoke_dirname(
               &instance, 3u, 32u, 7u) ==
           TURBOWASM_WASI_ERRNO_NAMETOOLONG);

    assert(invoke_prestat(
               &instance, 3u, 65532u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(invoke_dirname(
               &instance, 3u, 65535u, 8u) ==
           TURBOWASM_WASI_ERRNO_FAULT);

    assert(invoke_prestat(
               &instance, 4u, 0u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(invoke_dirname(
               &instance, 4u, 32u, 8u) ==
           TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(invoke_prestat(
               &instance, 99u, 0u) ==
           TURBOWASM_WASI_ERRNO_BADF);

    assert(invoke_close(
               &instance, 4u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.close_calls == 1u);
    assert(provider.last_closed.object == 200u);
    assert(invoke_close(
               &instance, 4u) ==
           TURBOWASM_WASI_ERRNO_BADF);
    assert(provider.close_calls == 1u);

    assert(invoke_close(
               &instance, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.close_calls == 2u);
    assert(provider.last_closed.object == 100u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

static void test_filesystem_capability_gating(void) {
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_wasi_preview1_init(
               &wasi, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) ==
           TURBOWASM_LINK_ERROR);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

int main(void) {
    test_preopen_and_close();
    test_filesystem_capability_gating();
    return 0;
}
