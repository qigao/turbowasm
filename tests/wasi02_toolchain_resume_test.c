#include "../src/wasi02_exec.h"

#include "wasi02_toolchain_fixtures.h"

#include <turbowasm/execution.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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

static const turbowasm_component_core_call_adapter *
find_export_adapter(
    const turbowasm_component_exec *exec,
    const char *name) {
    size_t size = strlen(name);
    uint32_t i;

    assert(exec != NULL);
    assert(exec->binary != NULL);

    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &exec->binary->exports[i];
        uint32_t adapter_index;

        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->name.size != size ||
            memcmp(export_desc->name.bytes, name, size) != 0)
            continue;

        assert(export_desc->item_index < exec->function_count);
        adapter_index =
            exec->function_adapter_indices[
                export_desc->item_index];
        assert(adapter_index != UINT32_MAX);
        assert(adapter_index < exec->adapter_count);
        return &exec->functions[adapter_index];
    }

    return NULL;
}

int main(void) {
    resume_probe probe = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_exec_capabilities capabilities = {0};
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    const turbowasm_component_core_call_adapter *adapter;
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    const turbowasm_value *result;

    probe.operation_token = (uintptr_t)0x5753493032u;

    poll_provider.context = &probe;
    poll_provider.ready = poll_ready;
    poll_provider.arm = poll_arm;
    poll_provider.drop = poll_drop;

    stream_provider.context = &probe;
    stream_provider.get_stdin = get_stdin;
    stream_provider.input_subscribe = input_subscribe;
    stream_provider.input_drop = input_drop;

    assert(turbowasm_wasi02_poll_init(
               &poll, &poll_provider, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams, &poll) == TURBOWASM_OK);

    assert(turbowasm_component_binary_load(
               &binary,
               turbowasm_wasi02_fixture_stream_poll_block,
               turbowasm_wasi02_fixture_stream_poll_block_size) ==
           TURBOWASM_OK);

    capabilities.poll = &poll;
    capabilities.streams = &streams;
    assert(turbowasm_wasi02_exec_init(
               &exec, &binary, &capabilities) == TURBOWASM_OK);

    adapter = find_export_adapter(&exec, "run");
    assert(adapter != NULL);
    assert(adapter->initialized);
    assert(adapter->instance != NULL);

    assert(turbowasm_execution_create(
               &execution,
               adapter->instance,
               adapter->function_index,
               NULL, 0u) == TURBOWASM_OK);

    {
        turbowasm_status resume_status =
            turbowasm_execution_resume(&execution, NULL);
        if (resume_status != TURBOWASM_YIELDED) {
            fprintf(
                stderr,
                "toolchain stream resume expected yield, got %d (%s); "
                "state=%d reason=%d stdin=%u subscribe=%u ready=%u arm=%u "
                "poll_drop=%u input_drop=%u\n",
                (int)resume_status,
                turbowasm_status_string(resume_status),
                (int)turbowasm_execution_state_get(&execution),
                (int)turbowasm_execution_yield_reason_get(&execution),
                probe.stdin_calls,
                probe.subscribe_calls,
                probe.ready_calls,
                probe.arm_calls,
                probe.poll_drop_calls,
                probe.input_drop_calls);
        }
        assert(resume_status == TURBOWASM_YIELDED);
    }
    assert(turbowasm_execution_yield_reason_get(
               &execution) == TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.ready_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 0u);
    assert(probe.input_drop_calls == 0u);

    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.operation_token == probe.operation_token);

    probe.ready = true;
    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 0) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);

    /*
     * The retained host callback frame continued after completion. The
     * producer, subscribe and arm callbacks were not replayed.
     */
    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.ready_calls == 1u);
    assert(probe.arm_calls == 1u);
    assert(probe.poll_drop_calls == 1u);
    assert(probe.input_drop_calls == 1u);

    assert(turbowasm_execution_result_count(
               &execution) == 1u);
    result = turbowasm_execution_result_at(
        &execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 1);

    turbowasm_execution_destroy(&execution);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);

    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    return 0;
}
