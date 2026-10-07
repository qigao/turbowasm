#include "component_exec.h"
#include "component_async_call.h"
#include "runtime_alloc.h"

typedef struct turbowasm_component_exec_async_call {
    turbowasm_component_async_call call;
    struct turbowasm_component_exec_async_call *next;
    turbowasm_status failure;
} turbowasm_component_exec_async_call;

static void append(turbowasm_component_exec *exec, turbowasm_component_exec_async_call *frame) {
    frame->next = NULL;
    if (exec->async_calls_tail != NULL) exec->async_calls_tail->next = frame;
    else exec->async_calls = frame;
    exec->async_calls_tail = frame;
}

static turbowasm_component_exec_async_call *pop(turbowasm_component_exec *exec) {
    turbowasm_component_exec_async_call *frame = exec->async_calls;
    if (frame == NULL) return NULL;
    exec->async_calls = frame->next;
    if (exec->async_calls == NULL) exec->async_calls_tail = NULL;
    frame->next = NULL;
    return frame;
}

static bool retireable(const turbowasm_component_exec *exec,
    const turbowasm_component_exec_async_call *frame) {
    const turbowasm_component_async_call *call = &frame->call;
    if (call->subtask.waitable.sync_waiter || call->subtask.waitable.delivering) return false;
    if (frame->failure != TURBOWASM_OK && !call->subtask.published && exec->task_domain.active == NULL)
        return true;
    return call->task.state >= TURBOWASM_EXECUTION_COMPLETED && call->subtask.waitable.handle == 0u &&
        call->subtask.waitable.state.subtask.resolve_delivered;
}

static turbowasm_status collect(turbowasm_component_exec *exec) {
    turbowasm_component_exec_async_call **link = &exec->async_calls, *previous = NULL;
    while (*link != NULL) {
        turbowasm_component_exec_async_call *frame = *link;
        turbowasm_status status;
        if (!retireable(exec, frame)) { previous = frame; link = &frame->next; continue; }
        status = turbowasm_component_async_call_destroy(&frame->call);
        if (frame->call.binding.callee_domain == NULL) {
            *link = frame->next;
            if (exec->async_calls_tail == frame) exec->async_calls_tail = previous;
            --exec->async_call_count; turbowasm_rt_free(frame);
        }
        if (status != TURBOWASM_OK) return status;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_async_lower(void *context, turbowasm_host_call *caller,
    const turbowasm_value *arguments, size_t argument_count, turbowasm_value *results,
    size_t result_capacity, size_t *result_count, turbowasm_trap *trap) {
    turbowasm_component_exec_canon_lower_context *lower = context;
    turbowasm_component_async_call_binding binding = {0};
    turbowasm_component_exec_async_call *frame;
    turbowasm_component_exec *exec;
    turbowasm_status status;
    uint32_t word;
    if (lower == NULL || lower->exec == NULL || !lower->is_async || caller == NULL ||
        result_count == NULL || trap == NULL || result_capacity < 1u || results == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    exec = lower->exec;
    if (exec->task_domain.active == NULL || exec->task_domain.active->destroying || !exec->may_leave)
        return TURBOWASM_TRAPPED;
    if (exec->async_functions == NULL || lower->local_adapter_index >= exec->adapter_count ||
        exec->async_functions[lower->local_adapter_index].instance == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = collect(exec);
    if (status != TURBOWASM_OK) return status;
    if (exec->task_domain.count >= exec->task_domain.limit ||
        exec->async_call_count >= exec->task_domain.limit) return TURBOWASM_OUT_OF_MEMORY;
    binding.caller_graph = lower->graph; binding.caller_function_type = lower->function_type;
    binding.caller_domain = binding.callee_domain = &exec->task_domain;
    binding.caller_memory = lower->memory;
    binding.callee = exec->async_functions[lower->local_adapter_index];
    frame = turbowasm_rt_calloc(1u, sizeof(*frame));
    if (frame == NULL) return TURBOWASM_OUT_OF_MEMORY;
    status = turbowasm_component_async_call_create(&frame->call, &binding, arguments, argument_count);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(frame); return status; }
    append(exec, frame); ++exec->async_call_count;
    status = turbowasm_component_async_call_start(&frame->call, caller, NULL, &word);
    if (status != TURBOWASM_OK) {
        frame->failure = status;
        *trap = frame->call.task.trap;
        return status;
    }
    /* Eager completion may free frame; never retain its pointer past collection. */
    status = collect(exec);
    if (status != TURBOWASM_OK) return status;
    results[0].kind = TURBOWASM_VALUE_I32; results[0].as.i32 = (int32_t)word;
    *result_count = 1u;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_async_poll(turbowasm_component_exec *exec,
    uint32_t max_quanta, const turbowasm_execution_options *options, uint32_t *out_pending) {
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    uint32_t i, count;
    if (exec == NULL || !exec->initialized || exec->task_domain.table == NULL || out_pending == NULL || max_quanta == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (exec->async_driving || exec->task_domain.active != NULL) return TURBOWASM_TRAPPED;
    scope = turbowasm_runtime_scope_enter(&exec->binary->config);
    exec->async_driving = true;
    status = collect(exec);
    count = exec->async_call_count < max_quanta ? exec->async_call_count : max_quanta;
    for (i = 0; status == TURBOWASM_OK && i < count; ++i) {
        turbowasm_component_exec_async_call *frame = pop(exec);
        if (frame == NULL) break;
        if (frame->failure != TURBOWASM_OK) status = frame->failure;
        else {
            status = turbowasm_component_task_resume(&frame->call.task, options);
            if (status == TURBOWASM_YIELDED) status = TURBOWASM_OK;
            if (status != TURBOWASM_OK) frame->failure = status;
        }
        append(exec, frame);
    }
    if (status == TURBOWASM_OK) status = collect(exec);
    exec->async_driving = false;
    *out_pending = exec->async_call_count;
    turbowasm_runtime_scope_leave(scope);
    return status != TURBOWASM_OK ? status : exec->async_call_count != 0u ? TURBOWASM_YIELDED : TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_async_abort(turbowasm_component_exec *exec, turbowasm_status reason) {
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_OK;
    if (exec == NULL || !exec->initialized || reason == TURBOWASM_OK || reason == TURBOWASM_YIELDED)
        return TURBOWASM_INVALID_ARGUMENT;
    if (exec->async_driving || exec->task_domain.active != NULL) return TURBOWASM_TRAPPED;
    /* The owner must unwind exported caller tasks before invalidating their
     * subtasks/result regions, including callers waiting through a shared set. */
    if (exec->task_domain.count != exec->async_call_count) return TURBOWASM_TRAPPED;
    scope = turbowasm_runtime_scope_enter(&exec->binary->config);
    exec->async_driving = true;
    while (exec->async_calls != NULL) {
        turbowasm_component_exec_async_call *frame = exec->async_calls;
        if (frame->call.subtask.waitable.sync_waiter || frame->call.subtask.waitable.delivering) {
            status = TURBOWASM_TRAPPED; break;
        }
        if (frame->call.subtask.waitable.failure == TURBOWASM_OK) frame->call.subtask.waitable.failure = reason;
        status = turbowasm_component_async_call_destroy(&frame->call);
        if (frame->call.binding.callee_domain == NULL) {
            (void)pop(exec); --exec->async_call_count; turbowasm_rt_free(frame);
        }
        if (status != TURBOWASM_OK) break;
    }
    exec->async_driving = false;
    turbowasm_runtime_scope_leave(scope);
    return status;
}
