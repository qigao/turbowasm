#include <turbowasm/native_io.h>
#include <turbowasm/turbowasm.h>

#include <salts/error_codes.h>

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

typedef struct mock_host_context {
    turbowasm_native_io_bridge *bridge;
    native_io_endpoint endpoint;
    unsigned char byte;
    int await_status;
    uint32_t calls;
    native_io_completion completion;
} mock_host_context;

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
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
    mock_host_context *host =
        (mock_host_context *)context;
    native_io_operation operation = {0};

    assert(host != NULL);
    assert(call != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++host->calls;
    operation.kind = NATIVE_IO_OPERATION_PIPE_READ;
    operation.endpoint = host->endpoint;
    operation.buffer = &host->byte;
    operation.length = 1u;
    operation.user_data = (uintptr_t)0xabcdu;

    memset(&host->completion, 0, sizeof(host->completion));
    host->await_status = turbowasm_native_io_await(
        host->bridge,
        call,
        &operation,
        &host->completion);
    if (host->await_status != SALTS_OK) {
        return host->await_status == SALTS_ENOBUFS
            ? TURBOWASM_UNSUPPORTED
            : TURBOWASM_INVALID_ARGUMENT;
    }

    assert(host->completion.user_data ==
           (uintptr_t)0xabcdu);
    assert(host->completion.kind ==
           NATIVE_IO_COMPLETION_OK);
    assert(host->completion.bytes == 1u);

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = (int32_t)host->byte;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status host_completion_kind(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    mock_host_context *host =
        (mock_host_context *)context;
    native_io_operation operation = {0};

    assert(host != NULL);
    assert(call != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++host->calls;
    operation.kind = NATIVE_IO_OPERATION_PIPE_READ;
    operation.endpoint = host->endpoint;
    operation.buffer = &host->byte;
    operation.length = 1u;
    operation.user_data = (uintptr_t)0xcafeu;

    memset(&host->completion, 0, sizeof(host->completion));
    host->await_status = turbowasm_native_io_await(
        host->bridge,
        call,
        &operation,
        &host->completion);
    if (host->await_status != SALTS_OK)
        return TURBOWASM_INVALID_ARGUMENT;

    assert(host->completion.user_data ==
           (uintptr_t)0xcafeu);
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 =
        (int32_t)host->completion.kind;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static const uint8_t host_read_module[] = {
    WASM_HEADER,
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x0d,
    0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x72, 0x65, 0x61, 0x64,
    0x00, 0x00
};

static void create_host_instance(
    turbowasm_module *module,
    turbowasm_instance *instance,
    mock_host_context *context,
    turbowasm_host_function_fn function) {
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        NULL, 0u, results, 1u
    };
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               module,
               host_read_module,
               sizeof(host_read_module)) ==
           TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) ==
           TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("read", 4u),
               &type,
               function,
               context) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               instance,
               module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

static native_io_completion mock_completion(
    const mock_native_io *mock,
    native_io_completion_kind kind,
    size_t bytes) {
    native_io_completion completion = {0};

    assert(mock != NULL);
    completion.request = mock->next_request;
    completion.endpoint = mock->submitted.endpoint;
    completion.kind = kind;
    completion.bytes = bytes;
    completion.status = SALTS_OK;
    completion.user_data =
        mock->submitted.user_data;
    return completion;
}

static void test_deterministic_completion_capacity_and_generation(void) {
    mock_native_io mock = {0};
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    turbowasm_module module1 = {0};
    turbowasm_module module2 = {0};
    turbowasm_instance instance1 = {0};
    turbowasm_instance instance2 = {0};
    turbowasm_execution execution1 = {0};
    turbowasm_execution execution2 = {0};
    mock_host_context host1 = {0};
    mock_host_context host2 = {0};
    native_io_request pending = {0};
    native_io_completion first;
    native_io_completion second;
    const turbowasm_value *result;

    mock.submit_status = SALTS_OK;
    mock.cancel_status = SALTS_OK;
    mock.next_request =
        (native_io_request){3u, 11u};
    backend.impl = &mock;

    assert(turbowasm_native_io_bridge_init(
               &bridge, &backend, 1u) == SALTS_OK);

    host1.bridge = &bridge;
    host1.endpoint =
        (native_io_endpoint){1u, 1u};
    host2.bridge = &bridge;
    host2.endpoint =
        (native_io_endpoint){1u, 1u};

    create_host_instance(
        &module1, &instance1,
        &host1, host_read_one);
    create_host_instance(
        &module2, &instance2,
        &host2, host_read_one);

    assert(turbowasm_execution_create(
               &execution1,
               &instance1, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution1, NULL) ==
           TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution1) ==
           TURBOWASM_YIELD_HOST_WAIT);
    assert(mock.submit_calls == 1u);
    assert(mock.submitted.user_data != 0u);
    assert(mock.submitted.user_data !=
           (uintptr_t)0xabcdu);
    assert(turbowasm_native_io_pending_request(
               &bridge, &execution1,
               &pending));
    assert(pending.slot == 3u);
    assert(pending.generation == 11u);

    /*
     * The bridge has one route slot. This second execution reaches the
     * real adapter callback but is rejected before mock NativeIO submit.
     */
    assert(turbowasm_execution_create(
               &execution2,
               &instance2, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution2, NULL) ==
           TURBOWASM_UNSUPPORTED);
    assert(host2.await_status == SALTS_ENOBUFS);
    assert(host2.calls == 1u);
    assert(mock.submit_calls == 1u);

    host1.byte = 42u;
    first = mock_completion(
        &mock, NATIVE_IO_COMPLETION_OK, 1u);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &first) == SALTS_OK);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &first) == SALTS_EALREADY);
    assert(turbowasm_execution_resume(
               &execution1, NULL) == TURBOWASM_OK);
    assert(host1.calls == 1u);
    result = turbowasm_execution_result_at(
        &execution1, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 42);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &first) == SALTS_ENOENT);

    /*
     * Reuse the same bridge slot with a new request generation. An old packet
     * names the same route pointer but must not complete the new wait.
     */
    turbowasm_execution_destroy(&execution2);
    execution2 = (turbowasm_execution){0};
    mock.next_request =
        (native_io_request){3u, 12u};
    host2.byte = 43u;
    assert(turbowasm_execution_create(
               &execution2,
               &instance2, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution2, NULL) ==
           TURBOWASM_YIELDED);
    assert(mock.submit_calls == 2u);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &first) == SALTS_ENOENT);

    second = mock_completion(
        &mock, NATIVE_IO_COMPLETION_OK, 1u);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &second) == SALTS_OK);
    assert(turbowasm_execution_resume(
               &execution2, NULL) == TURBOWASM_OK);
    assert(host2.calls == 2u);
    result = turbowasm_execution_result_at(
        &execution2, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 43);

    turbowasm_execution_destroy(&execution2);
    turbowasm_execution_destroy(&execution1);
    turbowasm_instance_destroy(&instance2);
    turbowasm_instance_destroy(&instance1);
    turbowasm_module_destroy(&module2);
    turbowasm_module_destroy(&module1);
    assert(turbowasm_native_io_bridge_destroy(
               &bridge) == SALTS_OK);
    backend.impl = NULL;
}

static void test_deterministic_cancellation_terminal(void) {
    mock_native_io mock = {0};
    native_io_backend backend = {0};
    turbowasm_native_io_bridge bridge = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    mock_host_context host = {0};
    native_io_request pending = {0};
    native_io_completion cancelled;
    const turbowasm_value *result;

    mock.submit_status = SALTS_OK;
    mock.cancel_status = SALTS_OK;
    mock.next_request =
        (native_io_request){5u, 21u};
    backend.impl = &mock;

    assert(turbowasm_native_io_bridge_init(
               &bridge, &backend, 1u) == SALTS_OK);
    host.bridge = &bridge;
    host.endpoint =
        (native_io_endpoint){2u, 4u};
    create_host_instance(
        &module, &instance,
        &host, host_completion_kind);

    assert(turbowasm_execution_create(
               &execution,
               &instance, 0u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) ==
           TURBOWASM_YIELDED);
    assert(turbowasm_native_io_pending_request(
               &bridge, &execution,
               &pending));
    assert(pending.slot == 5u);
    assert(pending.generation == 21u);

    assert(turbowasm_native_io_cancel_execution(
               &bridge, &execution) == SALTS_OK);
    assert(mock.cancel_calls == 1u);
    assert(mock.cancelled.slot == 5u);
    assert(mock.cancelled.generation == 21u);

    /* Cancellation request is not terminal until a completion is routed. */
    assert(turbowasm_execution_resume(
               &execution, NULL) ==
           TURBOWASM_YIELDED);

    cancelled = mock_completion(
        &mock,
        NATIVE_IO_COMPLETION_CANCELLED,
        0u);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &cancelled) ==
           SALTS_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) ==
           TURBOWASM_OK);

    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 ==
           (int32_t)NATIVE_IO_COMPLETION_CANCELLED);
    assert(host.calls == 1u);
    assert(turbowasm_native_io_bridge_complete(
               &bridge, &cancelled) ==
           SALTS_ENOENT);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    assert(turbowasm_native_io_bridge_destroy(
               &bridge) == SALTS_OK);
    backend.impl = NULL;
}

int main(void) {
    test_deterministic_completion_capacity_and_generation();
    test_deterministic_cancellation_terminal();
    return 0;
}
