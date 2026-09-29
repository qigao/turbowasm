#include <turbowasm/native_io.h>
#include <turbowasm/turbowasm.h>

#include <salts/error_codes.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static native_io_backend_kind test_backend_kind(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void test_bridge_lifecycle(void) {
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    const native_io_backend_config config = {
        test_backend_kind(), 1u, 2u, 2u
    };

    assert(native_io_backend_init(&backend, &config) == SALTS_OK);
    assert(turbowasm_native_io_bridge_init(
               &bridge, &backend, 2u) == SALTS_OK);
    assert(turbowasm_native_io_bridge_destroy(
               &bridge) == SALTS_OK);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

#if !defined(_WIN32)

typedef struct io_host_context {
    turbowasm_native_io_bridge *bridge;
    native_io_endpoint endpoint;
    unsigned char byte;
    int await_status;
    uint32_t calls;
    native_io_completion completion;
} io_host_context;

static turbowasm_name name_span(const char *text, uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status host_read_one(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    io_host_context *host = (io_host_context *)context;
    native_io_operation operation = {0};

    assert(host != NULL);
    assert(call != NULL);
    assert(argument_count == 0u);
    assert(arguments == NULL);
    assert(results != NULL && result_capacity >= 1u);
    assert(result_count != NULL && trap != NULL);

    ++host->calls;
    operation.kind = NATIVE_IO_OPERATION_PIPE_READ;
    operation.endpoint = host->endpoint;
    operation.buffer = &host->byte;
    operation.length = 1u;
    operation.user_data = (uintptr_t)0xabcdu;

    memset(&host->completion, 0, sizeof(host->completion));
    host->await_status = turbowasm_native_io_await(
        host->bridge, call, &operation, &host->completion);
    if (host->await_status != SALTS_OK)
        return host->await_status == SALTS_ENOBUFS
            ? TURBOWASM_UNSUPPORTED
            : TURBOWASM_INVALID_ARGUMENT;

    assert(host->completion.user_data == (uintptr_t)0xabcdu);
    if (host->completion.kind != NATIVE_IO_COMPLETION_OK ||
        host->completion.bytes != 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = (int32_t)host->byte;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static const uint8_t host_read_module[] = {
    WASM_HEADER,
    /* type0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    /* import host.read type0 */
    0x02, 0x0d,
    0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x72, 0x65, 0x61, 0x64,
    0x00, 0x00
};

static void make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void create_host_instance(
    turbowasm_module *module,
    turbowasm_instance *instance,
    io_host_context *context) {
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        NULL, 0u, results, 1u
    };
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               module, host_read_module,
               sizeof(host_read_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("read", 4u),
               &type,
               host_read_one,
               context) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               instance, module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

static void test_real_pipe_bridge_and_capacity(void) {
    int descriptors[2] = {-1, -1};
    native_io_backend backend = {0};
    native_io_endpoint endpoint = {0};
    turbowasm_native_io_bridge bridge = {0};
    const native_io_backend_config config = {
        test_backend_kind(), 1u, 2u, 2u
    };
    turbowasm_module module1 = {0};
    turbowasm_module module2 = {0};
    turbowasm_instance instance1 = {0};
    turbowasm_instance instance2 = {0};
    turbowasm_execution execution1 = {0};
    turbowasm_execution execution2 = {0};
    io_host_context host1 = {0};
    io_host_context host2 = {0};
    native_io_request request = {0};
    native_io_completion events[2] = {{0}};
    size_t event_count = 0u;
    const turbowasm_value *result;
    unsigned char payload = 0x2au;

    assert(pipe(descriptors) == 0);
    make_nonblocking(descriptors[0]);
    make_nonblocking(descriptors[1]);

    assert(native_io_backend_init(&backend, &config) == SALTS_OK);
    assert(native_io_backend_attach_pipe(
               &backend,
               (uintptr_t)descriptors[0],
               NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
               &endpoint) == SALTS_OK);
    assert(turbowasm_native_io_bridge_init(
               &bridge, &backend, 1u) == SALTS_OK);

    host1.bridge = &bridge;
    host1.endpoint = endpoint;
    host2.bridge = &bridge;
    host2.endpoint = endpoint;
    create_host_instance(&module1, &instance1, &host1);
    create_host_instance(&module2, &instance2, &host2);

    assert(turbowasm_execution_create(
               &execution1, &instance1, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution1, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(&execution1) ==
           TURBOWASM_YIELD_HOST_WAIT);
    assert(turbowasm_native_io_pending_request(
               &bridge, &execution1, &request));

    /*
     * Bridge capacity is one. A second instance reaches the host callback but
     * is rejected before NativeIO submission and therefore does not yield.
     */
    assert(turbowasm_execution_create(
               &execution2, &instance2, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution2, NULL) == TURBOWASM_UNSUPPORTED);
    assert(host2.await_status == SALTS_ENOBUFS);
    assert(host2.calls == 1u);

    assert(write(descriptors[1], &payload, 1u) == 1);
    assert(native_io_backend_observe(
               &backend, events, 2u, 1000u,
               &event_count) == SALTS_OK);
    assert(event_count == 1u);
    assert(events[0].kind == NATIVE_IO_COMPLETION_OK);
    assert(events[0].bytes == 1u);

    assert(turbowasm_native_io_bridge_complete(
               &bridge, &events[0]) == SALTS_OK);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &events[0]) == SALTS_EALREADY);

    assert(turbowasm_execution_resume(
               &execution1, NULL) == TURBOWASM_OK);
    assert(host1.await_status == SALTS_OK);
    assert(host1.calls == 1u);
    result = turbowasm_execution_result_at(
        &execution1, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 42);
    assert(!turbowasm_native_io_pending_request(
        &bridge, &execution1, &request));

    /* Slot is released after the callback resumes; old completion is stale. */
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &events[0]) == SALTS_ENOENT);

    turbowasm_execution_destroy(&execution2);
    turbowasm_execution_destroy(&execution1);
    turbowasm_instance_destroy(&instance2);
    turbowasm_instance_destroy(&instance1);
    turbowasm_module_destroy(&module2);
    turbowasm_module_destroy(&module1);

    assert(turbowasm_native_io_bridge_destroy(
               &bridge) == SALTS_OK);
    assert(native_io_backend_release_pipe(
               &backend, endpoint) == SALTS_OK);
    close(descriptors[0]);
    close(descriptors[1]);
    assert(native_io_backend_close(&backend) == SALTS_OK);
    assert(native_io_backend_destroy(&backend) == SALTS_OK);
}

#endif /* !_WIN32 */

int main(void) {
    test_bridge_lifecycle();
#if !defined(_WIN32)
    test_real_pipe_bridge_and_capacity();
#endif
    return 0;
}
