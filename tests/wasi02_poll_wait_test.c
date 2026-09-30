#include "../src/wasi02_poll.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER     0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00

typedef struct wait_any_probe {
    turbowasm_wasi02_poll poll;
    uint32_t resources[2];
    bool ready_by_rep[2];
    uintptr_t operation_token;
    uint32_t ready_calls;
    uint32_t arm_many_calls;
    uint32_t host_entries;
} wait_any_probe;

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status probe_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    wait_any_probe *probe = (wait_any_probe *)context;

    assert(probe != NULL);
    assert(out_ready != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 >= 0 && rep.as.i64 < 2);
    ++probe->ready_calls;
    *out_ready = probe->ready_by_rep[(size_t)rep.as.i64];
    return TURBOWASM_OK;
}

static turbowasm_status probe_arm_many(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    uintptr_t *out_operation_token) {
    wait_any_probe *probe = (wait_any_probe *)context;

    assert(probe != NULL);
    assert(reps != NULL);
    assert(rep_count == 2u);
    assert(reps[0].kind == TURBOWASM_VALUE_I64);
    assert(reps[0].as.i64 == 0);
    assert(reps[1].kind == TURBOWASM_VALUE_I64);
    assert(reps[1].as.i64 == 1);
    assert(out_operation_token != NULL);
    ++probe->arm_many_calls;
    *out_operation_token = probe->operation_token;
    return TURBOWASM_OK;
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
    wait_any_probe *probe = (wait_any_probe *)context;
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

    ++probe->host_entries;
    status = turbowasm_wasi02_poll_many(
        &probe->poll,
        probe->resources,
        2u,
        call,
        &ready_indices,
        trap);
    if (status != TURBOWASM_OK)
        return status;

    assert(ready_indices.kind == TURBOWASM_COMPONENT_TYPE_LIST);
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

static void test_restartable_wait_any(void) {
    wait_any_probe probe = {0};
    turbowasm_wasi02_poll_provider provider = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    turbowasm_value rep = {0};
    const turbowasm_value *result;
    static const turbowasm_value_kind host_results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, host_results, 1u
    };
    size_t i;

    probe.operation_token = (uintptr_t)0x445566u;
    provider.context = &probe;
    provider.ready = probe_ready;
    provider.arm_many = probe_arm_many;

    assert(turbowasm_wasi02_poll_init(
               &probe.poll, &provider, 4u) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    for (i = 0u; i < 2u; ++i) {
        rep.as.i64 = (int64_t)i;
        assert(turbowasm_wasi02_pollable_new(
                   &probe.poll,
                   rep,
                   &probe.resources[i]) == TURBOWASM_OK);
    }

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
               &probe) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    /* local wrapper is function index 1 after imported function index 0 */
    assert(turbowasm_execution_create(
               &execution,
               &instance,
               1u,
               NULL, 0u) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.host_entries == 1u);
    assert(probe.arm_many_calls == 1u);
    assert(probe.ready_calls == 2u);

    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.operation_token == probe.operation_token);

    /* Provider completion means source 1 is now ready. */
    probe.ready_by_rep[1] = true;
    assert(turbowasm_execution_complete_host_wait(
               &execution,
               wait,
               0) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);

    /* The retained callback frame continued; it was not replayed or re-armed. */
    assert(probe.host_entries == 1u);
    assert(probe.arm_many_calls == 1u);
    assert(probe.ready_calls == 4u);

    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 1);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    assert(turbowasm_wasi02_pollable_drop(
               &probe.poll,
               probe.resources[0]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_drop(
               &probe.poll,
               probe.resources[1]) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &probe.poll) == TURBOWASM_OK);
}

int main(void) {
    test_restartable_wait_any();
    return 0;
}
