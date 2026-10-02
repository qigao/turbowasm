#include <turbowasm/wasi02.h>

#include "wasi02_toolchain_fixtures.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct resume_probe {
    bool ready;
    uintptr_t operation_token;

    uint32_t stdin_calls;
    uint32_t subscribe_calls;
    uint32_t ready_calls;
    uint32_t arm_calls;
    uint32_t poll_drop_calls;
    uint32_t input_drop_calls;
} resume_probe;

static turbowasm_name run_name(void) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)"run";
    name.size = 3u;
    return name;
}

static turbowasm_status get_stdin(
    void *context,
    turbowasm_value *out_stream_rep) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(out_stream_rep != NULL);
    ++probe->stdin_calls;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = 11;
    return TURBOWASM_OK;
}

static turbowasm_status input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(stream_rep.as.i64 == 11);
    assert(out_pollable_rep != NULL);
    ++probe->subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 77;
    return TURBOWASM_OK;
}

static void input_drop(
    void *context,
    turbowasm_value rep) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static turbowasm_status poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    assert(out_ready != NULL);
    ++probe->ready_calls;
    *out_ready = probe->ready;
    return TURBOWASM_OK;
}

static turbowasm_status poll_arm(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    assert(out_operation_token != NULL);
    ++probe->arm_calls;
    *out_operation_token = probe->operation_token;
    return TURBOWASM_OK;
}

static turbowasm_status poll_drop(
    void *context,
    turbowasm_value rep) {
    resume_probe *probe = (resume_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 77);
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

int main(void) {
    resume_probe probe = {0};
    turbowasm_wasi02_config config = {0};
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_call call = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_host_wait wait = {0};
    turbowasm_host_wait stale = {0};

    probe.operation_token = (uintptr_t)0x5753493032u;

    config.poll.context = &probe;
    config.poll.ready = poll_ready;
    config.poll.arm = poll_arm;
    config.poll.drop = poll_drop;
    config.pollable_capacity = 8u;

    config.streams.context = &probe;
    config.streams.get_stdin = get_stdin;
    config.streams.input_subscribe = input_subscribe;
    config.streams.input_drop = input_drop;
    config.stream_resource_capacity = 8u;

    assert(turbowasm_wasi02_init(
               &wasi02, &config, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_load_borrowed(
               &component,
               turbowasm_wasi02_fixture_stream_poll_block,
               turbowasm_wasi02_fixture_stream_poll_block_size) ==
           TURBOWASM_OK);

    assert(turbowasm_wasi02_component_instance_create(
               &instance,
               &component,
               &wasi02) == TURBOWASM_OK);

    assert(turbowasm_component_call_create(
               &call,
               &instance,
               run_name(),
               NULL, 0u) == TURBOWASM_OK);

    /* Create performs canonical lowering only; it does not enter Core Wasm. */
    assert(probe.stdin_calls == 0u);
    assert(probe.subscribe_calls == 0u);
    assert(probe.ready_calls == 0u);
    assert(probe.arm_calls == 0u);

    assert(turbowasm_component_call_state_get(
               &call) == TURBOWASM_EXECUTION_READY);

    assert(turbowasm_component_call_resume(
               &call, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_component_call_state_get(
               &call) == TURBOWASM_EXECUTION_YIELDED);
    assert(turbowasm_component_call_yield_reason_get(
               &call) == TURBOWASM_YIELD_HOST_WAIT);

    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.ready_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 0u);
    assert(probe.input_drop_calls == 0u);

    assert(turbowasm_component_call_pending_host_wait(
               &call, &wait));
    assert(wait.generation != 0u);
    assert(wait.operation_token == probe.operation_token);

    /* Resume without completion must not re-enter the retained callback. */
    assert(turbowasm_component_call_resume(
               &call, NULL) == TURBOWASM_YIELDED);
    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.ready_calls == 1u);
    assert(probe.arm_calls == 1u);

    stale = wait;
    ++stale.generation;
    assert(turbowasm_component_call_complete_host_wait(
               &call, stale, 0) == TURBOWASM_INVALID_ARGUMENT);

    probe.ready = true;
    assert(turbowasm_component_call_complete_host_wait(
               &call, wait, 0) == TURBOWASM_OK);
    assert(turbowasm_component_call_complete_host_wait(
               &call, wait, 0) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_component_call_resume(
               &call, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_call_state_get(
               &call) == TURBOWASM_EXECUTION_COMPLETED);
    assert(turbowasm_component_call_terminal_status(
               &call) == TURBOWASM_OK);
    assert(turbowasm_component_call_trap(
               &call) == TURBOWASM_TRAP_NONE);
    assert(!turbowasm_component_call_pending_host_wait(
               &call, &wait));

    /*
     * The retained host callback frame continued after completion. Canonical
     * export resolution and provider callbacks were not replayed.
     */
    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.ready_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 1u);
    assert(probe.input_drop_calls == 1u);

    assert(turbowasm_component_call_result_count(
               &call) == 1u);
    assert(turbowasm_component_call_take_result(
               &call, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(result.as.u32 == 1u);
    assert(turbowasm_component_call_take_result(
               &call, &result) == TURBOWASM_INVALID_ARGUMENT);

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_call_destroy(&call);
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    return 0;
}
