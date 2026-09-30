#include "../src/wasi02_poll_native_io.h"

#include <turbowasm/turbowasm.h>

#include <salts/error_codes.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER     0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef struct cancel_probe {
    uint32_t calls;
    native_io_request last_request;
    int status;
} cancel_probe;

typedef struct host_probe {
    turbowasm_wasi02_poll *poll;
    uint32_t resources[2];
    uint32_t calls;
} host_probe;

static int fake_cancel(
    native_io_backend *backend,
    native_io_request request) {
    cancel_probe *probe;

    assert(backend != NULL);
    probe = (cancel_probe *)backend->impl;
    assert(probe != NULL);
    ++probe->calls;
    probe->last_request = request;
    return probe->status;
}

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status host_poll_wait_any(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_probe *probe = (host_probe *)context;
    turbowasm_component_value ready_indices = {0};
    turbowasm_status status;

    assert(probe != NULL);
    assert(call != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++probe->calls;
    status = turbowasm_wasi02_poll_many(
        probe->poll,
        probe->resources,
        2u,
        call,
        &ready_indices,
        trap);
    if (status != TURBOWASM_OK)
        return status;

    assert(ready_indices.kind ==
           TURBOWASM_COMPONENT_TYPE_LIST);
    assert(ready_indices.as.list.count == 1u);
    assert(ready_indices.as.list.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_U32);

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 =
        (int32_t)ready_indices.as.list.items[0].as.u32;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;

    turbowasm_component_value_destroy(&ready_indices);
    return TURBOWASM_OK;
}

static const uint8_t module_bytes[] = {
    WASM_HEADER,

    /* type0: () -> i32 */
    0x01,0x05,
    0x01,0x60,0x00,0x01,0x7f,

    /* import host.poll type0 */
    0x02,0x0d,
    0x01,
    0x04,'h','o','s','t',
    0x04,'p','o','l','l',
    0x00,0x00,

    /* one local wrapper, type0 */
    0x03,0x02,
    0x01,0x00,

    /* wrapper body: call host.poll */
    0x0a,0x06,
    0x01,
    0x04,0x00,0x10,0x00,0x0b
};

static void test_native_io_terminal_routes_one_wait_any(void) {
    cancel_probe cancel = {0};
    native_io_backend backend = {0};
    turbowasm_wasi02_native_io_poll native_poll = {0};
    turbowasm_wasi02_poll_provider provider = {0};
    turbowasm_wasi02_poll poll = {0};
    native_io_request requests[3] = {
        {1u, 11u},
        {2u, 12u},
        {3u, 13u}
    };
    turbowasm_value reps[3] = {{0}};
    host_probe host = {0};
    turbowasm_module module = {0};
    turbowasm_linker linker = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    native_io_completion completion = {0};
    const turbowasm_value *result;
    bool ready = false;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };

    cancel.status = SALTS_OK;
    backend.impl = &cancel;

    assert(turbowasm_wasi02_native_io_poll_init(
               &native_poll,
               &backend,
               4u,
               2u,
               4u,
               fake_cancel) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_provider(
               &native_poll,
               &provider) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll, &provider, 4u) == TURBOWASM_OK);

    assert(turbowasm_wasi02_native_io_poll_register_request(
               &native_poll,
               requests[0],
               &reps[0]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_register_request(
               &native_poll,
               requests[1],
               &reps[1]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_register_request(
               &native_poll,
               requests[1],
               &reps[1]) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_wasi02_pollable_new(
               &poll,
               reps[0],
               &host.resources[0]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll,
               reps[1],
               &host.resources[1]) == TURBOWASM_OK);
    host.poll = &poll;

    assert(turbowasm_module_load_borrowed(
               &module,
               module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("poll", 4u),
               &host_type,
               host_poll_wait_any,
               &host) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    assert(turbowasm_execution_create(
               &execution,
               &instance,
               1u,
               NULL, 0u) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(host.calls == 1u);
    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.generation != 0u);
    assert(wait.operation_token != 0u);

    completion.request = requests[1];
    completion.kind = NATIVE_IO_COMPLETION_OK;
    completion.status = SALTS_OK;
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);
    assert(host.calls == 1u);
    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 1);

    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resources[0],
               &ready) == TURBOWASM_OK);
    assert(!ready);
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resources[1],
               &ready) == TURBOWASM_OK);
    assert(ready);

    /* Terminal pollable releases immediately and does not cancel. */
    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               host.resources[1]) == TURBOWASM_OK);
    assert(cancel.calls == 0u);

    /*
     * Abandon a second aggregate wait before destroying its coroutine.
     * The later terminal completion must not retain/dereference that old
     * host-call frame.
     */
    turbowasm_execution_destroy(&execution);
    assert(turbowasm_wasi02_native_io_poll_register_request(
               &native_poll,
               requests[2],
               &reps[2]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll,
               reps[2],
               &host.resources[1]) == TURBOWASM_OK);

    assert(turbowasm_execution_create(
               &execution,
               &instance,
               1u,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.generation != 0u);
    assert(wait.operation_token != 0u);
    assert(turbowasm_wasi02_native_io_poll_abandon_wait(
               &native_poll,
               wait.operation_token) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_abandon_wait(
               &native_poll,
               wait.operation_token) == TURBOWASM_TRAPPED);

    turbowasm_execution_destroy(&execution);

    completion = (native_io_completion){0};
    completion.request = requests[2];
    completion.kind = NATIVE_IO_COMPLETION_OK;
    completion.status = SALTS_OK;
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_ready(
               &poll,
               host.resources[1],
               &ready) == TURBOWASM_OK);
    assert(ready);
    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               host.resources[1]) == TURBOWASM_OK);
    assert(cancel.calls == 0u);

    /*
     * Non-terminal drop requests NativeIO cancellation but the internal
     * registration remains retained until the terminal CANCELLED packet.
     */
    assert(turbowasm_wasi02_pollable_drop(
               &poll,
               host.resources[0]) == TURBOWASM_OK);
    assert(cancel.calls == 1u);
    assert(cancel.last_request.slot == requests[0].slot);
    assert(cancel.last_request.generation ==
           requests[0].generation);

    completion = (native_io_completion){0};
    completion.request = requests[0];
    completion.kind = NATIVE_IO_COMPLETION_CANCELLED;
    completion.status = SALTS_ECANCELED;
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_complete(
               &native_poll,
               &completion) == TURBOWASM_TRAPPED);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    assert(turbowasm_wasi02_native_io_poll_destroy(
               &native_poll) == TURBOWASM_OK);
    backend.impl = NULL;
}

int main(void) {
    test_native_io_terminal_routes_one_wait_any();
    return 0;
}
