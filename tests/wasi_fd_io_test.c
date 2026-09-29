#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

    /* type0: (i32,i32,i32,i32)->i32
       type1: (i32,i32)->()
       type2: (i32)->i32 */
    0x01, 0x13, 0x03,
    0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x60, 0x01, 0x7f, 0x01, 0x7f,

    /* import fd_write type0, fd_read type0 */
    0x02, 0x44, 0x02,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x08, 0x66, 0x64, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65,
    0x00, 0x00,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x07, 0x66, 0x64, 0x5f, 0x72, 0x65, 0x61, 0x64,
    0x00, 0x00,

    /* local: store32, store8, read32, read8 */
    0x03, 0x05, 0x04, 0x01, 0x01, 0x02, 0x02,

    /* memory0 min=1 */
    0x05, 0x03, 0x01, 0x00, 0x01,

    0x0a, 0x25, 0x04,

    /* f2 store32(addr,value) */
    0x09, 0x00,
    0x20, 0x00, 0x20, 0x01,
    0x36, 0x02, 0x00, 0x0b,

    /* f3 store8(addr,value) */
    0x09, 0x00,
    0x20, 0x00, 0x20, 0x01,
    0x3a, 0x00, 0x00, 0x0b,

    /* f4 read32(addr) */
    0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,

    /* f5 read8(addr) */
    0x07, 0x00,
    0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

typedef struct fd_probe {
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t last_fd;
    size_t last_buffer_count;
    bool overflow_read;
} fd_probe;

static uint32_t fd_write(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    fd_probe *probe = (fd_probe *)context;

    assert(probe != NULL);
    assert(out_written != NULL);
    ++probe->write_calls;
    probe->last_fd = fd;
    probe->last_buffer_count = buffer_count;

    if (fd != 1u)
        return TURBOWASM_WASI_ERRNO_BADF;

    assert(buffer_count == 2u);
    assert(buffers != NULL);
    assert(buffers[0].size == 2u);
    assert(buffers[1].size == 3u);
    assert(memcmp(buffers[0].data, "hi", 2u) == 0);
    assert(memcmp(buffers[1].data, "xyz", 3u) == 0);

    /* Qualify partial write accounting. */
    *out_written = 4u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fd_read(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    fd_probe *probe = (fd_probe *)context;

    assert(probe != NULL);
    assert(out_read != NULL);
    ++probe->read_calls;
    probe->last_fd = fd;
    probe->last_buffer_count = buffer_count;

    if (fd != 0u)
        return TURBOWASM_WASI_ERRNO_BADF;

    assert(buffer_count == 2u);
    assert(buffers != NULL);
    assert(buffers[0].size == 2u);
    assert(buffers[1].size == 3u);

    if (probe->overflow_read) {
        *out_read = 99u;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    memcpy(buffers[0].data, "AB", 2u);
    memcpy(buffers[1].data, "CDE", 3u);
    *out_read = 5u;
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

static void invoke_store(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address,
    uint32_t value) {
    turbowasm_value args[2] = {
        i32_value((int32_t)address),
        i32_value((int32_t)value)
    };
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               args, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static int32_t guest_read(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    return invoke_i32(instance, function_index, &arg, 1u);
}

static int32_t invoke_fd(
    turbowasm_instance *instance,
    uint32_t function_index,
    uint32_t fd,
    uint32_t iovs,
    uint32_t iov_count,
    uint32_t out_count) {
    turbowasm_value args[4] = {
        i32_value((int32_t)fd),
        i32_value((int32_t)iovs),
        i32_value((int32_t)iov_count),
        i32_value((int32_t)out_count)
    };
    return invoke_i32(instance, function_index, args, 4u);
}

static void setup(
    turbowasm_wasi_preview1 *wasi,
    turbowasm_module *module,
    turbowasm_instance *instance,
    fd_probe *probe) {
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_linker linker = {0};

    config.allow_fd_write = true;
    config.fd_write = fd_write;
    config.fd_write_context = probe;
    config.allow_fd_read = true;
    config.fd_read = fd_read;
    config.fd_read_context = probe;

    assert(turbowasm_wasi_preview1_init(
               wasi, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               module, module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               instance, module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

static void prepare_write_iovecs(
    turbowasm_instance *instance) {
    /* iovec0={32,2}, iovec1={40,3}. */
    invoke_store(instance, 2u, 0u, 32u);
    invoke_store(instance, 2u, 4u, 2u);
    invoke_store(instance, 2u, 8u, 40u);
    invoke_store(instance, 2u, 12u, 3u);

    invoke_store(instance, 3u, 32u, 'h');
    invoke_store(instance, 3u, 33u, 'i');
    invoke_store(instance, 3u, 40u, 'x');
    invoke_store(instance, 3u, 41u, 'y');
    invoke_store(instance, 3u, 42u, 'z');
}

static void prepare_read_iovecs(
    turbowasm_instance *instance) {
    /* iovec0={80,2}, iovec1={90,3}. */
    invoke_store(instance, 2u, 64u, 80u);
    invoke_store(instance, 2u, 68u, 2u);
    invoke_store(instance, 2u, 72u, 90u);
    invoke_store(instance, 2u, 76u, 3u);
}

static void test_fd_write_and_read(void) {
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    fd_probe probe = {0};

    setup(&wasi, &module, &instance, &probe);

    prepare_write_iovecs(&instance);
    assert(invoke_fd(
               &instance, 0u, 1u, 0u, 2u, 24u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.write_calls == 1u);
    assert(probe.last_fd == 1u);
    assert(probe.last_buffer_count == 2u);
    assert(guest_read(&instance, 4u, 24u) == 4);

    /* Provider errno is returned verbatim. */
    assert(invoke_fd(
               &instance, 0u, 9u, 0u, 2u, 24u) ==
           TURBOWASM_WASI_ERRNO_BADF);
    assert(probe.write_calls == 2u);

    prepare_read_iovecs(&instance);
    assert(invoke_fd(
               &instance, 1u, 0u, 64u, 2u, 104u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.read_calls == 1u);
    assert(guest_read(&instance, 4u, 104u) == 5);
    assert(guest_read(&instance, 5u, 80u) == 'A');
    assert(guest_read(&instance, 5u, 81u) == 'B');
    assert(guest_read(&instance, 5u, 90u) == 'C');
    assert(guest_read(&instance, 5u, 92u) == 'E');

    /* Provider cannot claim more bytes than validated capacity. */
    probe.overflow_read = true;
    assert(invoke_fd(
               &instance, 1u, 0u, 64u, 2u, 104u) ==
           TURBOWASM_WASI_ERRNO_IO);
    assert(probe.read_calls == 2u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

static void test_validation_precedes_provider(void) {
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    fd_probe probe = {0};
    uint32_t before;

    setup(&wasi, &module, &instance, &probe);
    prepare_write_iovecs(&instance);

    before = probe.write_calls;
    assert(invoke_fd(
               &instance, 0u, 1u, 65535u, 1u, 24u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(probe.write_calls == before);

    /* Valid table but OOB payload. */
    invoke_store(&instance, 2u, 0u, 65535u);
    invoke_store(&instance, 2u, 4u, 2u);
    assert(invoke_fd(
               &instance, 0u, 1u, 0u, 1u, 24u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(probe.write_calls == before);

    /* OOB nwritten pointer is checked first. */
    prepare_write_iovecs(&instance);
    assert(invoke_fd(
               &instance, 0u, 1u, 0u, 2u, 65535u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(probe.write_calls == before);

    /* Hard bound prevents guest-controlled vector allocation. */
    assert(invoke_fd(
               &instance, 0u, 1u, 0u,
               TURBOWASM_WASI_IOV_MAX + 1u, 24u) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(probe.write_calls == before);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

static void test_capability_gating(void) {
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_wasi_preview1 wasi = {0};
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
    test_fd_write_and_read();
    test_validation_precedes_provider();
    test_capability_gating();
    return 0;
}
