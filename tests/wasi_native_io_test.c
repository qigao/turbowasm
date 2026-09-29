#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_native_io.h>

#include <salts/error_codes.h>
#include <salts/native_io.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct mock_native_io {
    int submit_status;
    int cancel_status;
    native_io_request next_request;
    native_io_operation submitted;
    native_io_request cancelled;
    uint32_t submit_calls;
    uint32_t cancel_calls;
} mock_native_io;

int turbowasm_test_native_io_backend_submit(
    native_io_backend *backend,
    const native_io_operation *operation,
    native_io_request *out_request) {
    mock_native_io *mock;

    assert(backend != NULL);
    assert(operation != NULL);
    assert(out_request != NULL);
    mock = (mock_native_io *)backend->impl;
    assert(mock != NULL);

    ++mock->submit_calls;
    mock->submitted = *operation;
    if (mock->submit_status != SALTS_OK) {
        *out_request = (native_io_request){0};
        return mock->submit_status;
    }

    *out_request = mock->next_request;
    return SALTS_OK;
}

int turbowasm_test_native_io_backend_cancel(
    native_io_backend *backend,
    native_io_request request) {
    mock_native_io *mock;

    assert(backend != NULL);
    mock = (mock_native_io *)backend->impl;
    assert(mock != NULL);

    ++mock->cancel_calls;
    mock->cancelled = request;
    return mock->cancel_status;
}

/* Compile the exact production bridge against the deterministic OS boundary. */
#define native_io_backend_submit turbowasm_test_native_io_backend_submit
#define native_io_backend_cancel turbowasm_test_native_io_backend_cancel
#include "../src/native_io_adapter.c"
#undef native_io_backend_cancel
#undef native_io_backend_submit

/* Compile the exact production WASI NativeIO provider in the same test TU. */
#include "../src/wasi_native_io.c"

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

typedef struct resolver_state {
    native_io_endpoint endpoint;
    uint32_t calls;
} resolver_state;

static uint32_t resolve_fd(
    void *context,
    uint32_t fd,
    bool write,
    native_io_endpoint *out_endpoint,
    native_io_operation_kind *out_kind) {
    resolver_state *state =
        (resolver_state *)context;

    assert(state != NULL);
    assert(out_endpoint != NULL);
    assert(out_kind != NULL);
    ++state->calls;

    if ((!write && fd != 0u) ||
        (write && fd != 1u))
        return TURBOWASM_WASI_ERRNO_BADF;

    *out_endpoint = state->endpoint;
    *out_kind = write
        ? NATIVE_IO_OPERATION_PIPE_WRITE
        : NATIVE_IO_OPERATION_PIPE_READ;
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
    turbowasm_value arg =
        i32_value((int32_t)address);
    return invoke_i32(
        instance, function_index, &arg, 1u);
}

static void prepare_write_iovecs(
    turbowasm_instance *instance) {
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
    invoke_store(instance, 2u, 64u, 80u);
    invoke_store(instance, 2u, 68u, 2u);
    invoke_store(instance, 2u, 72u, 90u);
    invoke_store(instance, 2u, 76u, 3u);
}

static native_io_completion mock_completion(
    const mock_native_io *mock,
    native_io_completion_kind kind,
    size_t bytes,
    int status) {
    native_io_completion completion = {0};

    assert(mock != NULL);
    completion.request = mock->next_request;
    completion.endpoint = mock->submitted.endpoint;
    completion.kind = kind;
    completion.bytes = bytes;
    completion.status = status;
    completion.user_data =
        mock->submitted.user_data;
    return completion;
}

static void setup(
    mock_native_io *mock,
    native_io_backend *backend,
    turbowasm_native_io_bridge *bridge,
    turbowasm_wasi_native_io *provider,
    resolver_state *resolver,
    turbowasm_wasi_preview1 *wasi,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    turbowasm_wasi_native_io_config provider_config = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    turbowasm_linker linker = {0};

    backend->impl = mock;
    assert(turbowasm_native_io_bridge_init(
               bridge, backend, 1u) == SALTS_OK);

    resolver->endpoint =
        (native_io_endpoint){1u, 1u};
    provider_config.bridge = bridge;
    provider_config.resolve_fd = resolve_fd;
    provider_config.resolve_context = resolver;
    assert(turbowasm_wasi_native_io_init(
               provider, &provider_config) ==
           TURBOWASM_OK);

    assert(turbowasm_wasi_native_io_apply(
               provider, &wasi_config,
               true, true) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_init(
               wasi, &wasi_config) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
               module,
               module_bytes,
               sizeof(module_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) ==
           TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               instance, module, &linker) ==
           TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

static void teardown(
    native_io_backend *backend,
    turbowasm_native_io_bridge *bridge,
    turbowasm_wasi_native_io *provider,
    turbowasm_wasi_preview1 *wasi,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    turbowasm_instance_destroy(instance);
    turbowasm_module_destroy(module);
    turbowasm_wasi_preview1_destroy(wasi);
    turbowasm_wasi_native_io_destroy(provider);
    assert(turbowasm_native_io_bridge_destroy(
               bridge) == SALTS_OK);
    backend->impl = NULL;
}

static void test_async_read_write_and_short_iov(void) {
    mock_native_io mock = {0};
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    turbowasm_wasi_native_io provider = {0};
    resolver_state resolver = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution read_execution = {0};
    turbowasm_execution write_execution = {0};
    turbowasm_value read_args[4];
    turbowasm_value write_args[4];
    native_io_completion completion;
    const turbowasm_value *result;

    mock.submit_status = SALTS_OK;
    mock.cancel_status = SALTS_OK;
    mock.next_request =
        (native_io_request){3u, 10u};
    setup(
        &mock, &backend, &bridge, &provider,
        &resolver, &wasi, &module, &instance);

    prepare_read_iovecs(&instance);
    read_args[0] = i32_value(0);
    read_args[1] = i32_value(64);
    read_args[2] = i32_value(2);
    read_args[3] = i32_value(104);

    assert(turbowasm_execution_create(
               &read_execution,
               &instance, 1u,
               read_args, 4u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &read_execution, NULL) ==
           TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &read_execution) ==
           TURBOWASM_YIELD_HOST_WAIT);
    assert(mock.submit_calls == 1u);
    assert(mock.submitted.kind ==
           NATIVE_IO_OPERATION_PIPE_READ);
    assert(mock.submitted.endpoint.slot == 1u);
    assert(mock.submitted.endpoint.generation == 1u);
    assert(mock.submitted.length == 2u);

    memcpy(mock.submitted.buffer, "AB", 2u);
    completion = mock_completion(
        &mock, NATIVE_IO_COMPLETION_OK, 2u, SALTS_OK);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &completion) == SALTS_OK);
    assert(turbowasm_execution_resume(
               &read_execution, NULL) ==
           TURBOWASM_OK);
    result = turbowasm_execution_result_at(
        &read_execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 104u) == 2);
    assert(guest_read(&instance, 5u, 80u) == 'A');
    assert(guest_read(&instance, 5u, 81u) == 'B');
    /* Second iovec is untouched: this is one legal short read. */
    assert(guest_read(&instance, 5u, 90u) == 0);

    turbowasm_execution_destroy(&read_execution);

    prepare_write_iovecs(&instance);
    mock.next_request =
        (native_io_request){3u, 11u};
    write_args[0] = i32_value(1);
    write_args[1] = i32_value(0);
    write_args[2] = i32_value(2);
    write_args[3] = i32_value(24);

    assert(turbowasm_execution_create(
               &write_execution,
               &instance, 0u,
               write_args, 4u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &write_execution, NULL) ==
           TURBOWASM_YIELDED);
    assert(mock.submit_calls == 2u);
    assert(mock.submitted.kind ==
           NATIVE_IO_OPERATION_PIPE_WRITE);
    assert(mock.submitted.length == 2u);
    assert(memcmp(
        mock.submitted.buffer, "hi", 2u) == 0);

    completion = mock_completion(
        &mock, NATIVE_IO_COMPLETION_OK, 2u, SALTS_OK);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &completion) == SALTS_OK);
    assert(turbowasm_execution_resume(
               &write_execution, NULL) ==
           TURBOWASM_OK);
    result = turbowasm_execution_result_at(
        &write_execution, 0u);
    assert(result != NULL);
    assert(result->as.i32 ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 24u) == 2);

    turbowasm_execution_destroy(&write_execution);
    teardown(
        &backend, &bridge, &provider,
        &wasi, &module, &instance);
}

static void test_capacity_bad_fd_and_cancel_mapping(void) {
    mock_native_io mock = {0};
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    turbowasm_wasi_native_io provider = {0};
    resolver_state resolver = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution pending = {0};
    turbowasm_execution rejected = {0};
    turbowasm_execution bad_fd = {0};
    turbowasm_value read_args[4];
    turbowasm_value write_args[4];
    turbowasm_value bad_args[4];
    native_io_completion completion;
    const turbowasm_value *result;
    uint32_t submits_before;

    mock.submit_status = SALTS_OK;
    mock.cancel_status = SALTS_OK;
    mock.next_request =
        (native_io_request){7u, 20u};
    setup(
        &mock, &backend, &bridge, &provider,
        &resolver, &wasi, &module, &instance);

    prepare_read_iovecs(&instance);
    prepare_write_iovecs(&instance);

    read_args[0] = i32_value(0);
    read_args[1] = i32_value(64);
    read_args[2] = i32_value(2);
    read_args[3] = i32_value(104);
    assert(turbowasm_execution_create(
               &pending, &instance, 1u,
               read_args, 4u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &pending, NULL) ==
           TURBOWASM_YIELDED);

    /* One bridge route is occupied; a second fd call maps ENOBUFS -> AGAIN. */
    write_args[0] = i32_value(1);
    write_args[1] = i32_value(0);
    write_args[2] = i32_value(2);
    write_args[3] = i32_value(24);
    submits_before = mock.submit_calls;
    assert(turbowasm_execution_create(
               &rejected, &instance, 0u,
               write_args, 4u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &rejected, NULL) == TURBOWASM_OK);
    result = turbowasm_execution_result_at(
        &rejected, 0u);
    assert(result != NULL);
    assert(result->as.i32 ==
           TURBOWASM_WASI_ERRNO_AGAIN);
    assert(mock.submit_calls == submits_before);

    /* Resolver failure is returned before bridge submission. */
    bad_args[0] = i32_value(9);
    bad_args[1] = i32_value(64);
    bad_args[2] = i32_value(2);
    bad_args[3] = i32_value(104);
    assert(turbowasm_execution_create(
               &bad_fd, &instance, 1u,
               bad_args, 4u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &bad_fd, NULL) == TURBOWASM_OK);
    result = turbowasm_execution_result_at(
        &bad_fd, 0u);
    assert(result != NULL);
    assert(result->as.i32 ==
           TURBOWASM_WASI_ERRNO_BADF);
    assert(mock.submit_calls == submits_before);

    /* Cancel request is not terminal. CANCELLED completion maps to EINTR. */
    assert(turbowasm_native_io_cancel_execution(
               &bridge, &pending) == SALTS_OK);
    assert(mock.cancel_calls == 1u);
    assert(turbowasm_execution_resume(
               &pending, NULL) ==
           TURBOWASM_YIELDED);

    completion = mock_completion(
        &mock,
        NATIVE_IO_COMPLETION_CANCELLED,
        0u,
        SALTS_ECANCELED);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &completion) == SALTS_OK);
    assert(turbowasm_execution_resume(
               &pending, NULL) == TURBOWASM_OK);
    result = turbowasm_execution_result_at(
        &pending, 0u);
    assert(result != NULL);
    assert(result->as.i32 ==
           TURBOWASM_WASI_ERRNO_INTR);

    turbowasm_execution_destroy(&bad_fd);
    turbowasm_execution_destroy(&rejected);
    turbowasm_execution_destroy(&pending);
    teardown(
        &backend, &bridge, &provider,
        &wasi, &module, &instance);
}

int main(void) {
    test_async_read_write_and_short_iov();
    test_capacity_bad_fd_and_cancel_mapping();
    return 0;
}
