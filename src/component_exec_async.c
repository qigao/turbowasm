#include "component_exec.h"
#include "component_async_call.h"
#include "runtime_alloc.h"

typedef struct turbowasm_component_exec_async_transaction {
    turbowasm_component_endpoint_codec endpoints;
    turbowasm_component_exec_resource_codec resources;
} turbowasm_component_exec_async_transaction;

typedef struct turbowasm_component_exec_async_call {
    turbowasm_component_async_call call;
    turbowasm_component_exec_async_transaction parameters, result;
    turbowasm_component_exec_realloc_context result_realloc;
    struct turbowasm_component_exec_async_call *next;
    turbowasm_status failure;
} turbowasm_component_exec_async_call;

turbowasm_status turbowasm_component_exec_async_bind(
    turbowasm_component_exec_canon_lower_context *lower,
    turbowasm_component_exec *provider, uint32_t adapter_index) {
    const turbowasm_component_type *source, *target;
    const turbowasm_component_task_binding *binding;
    uint32_t i;
    if (lower == NULL || lower->exec == NULL || lower->async_provider != NULL || !lower->is_async ||
        provider == NULL || !provider->initialized || provider == lower->exec ||
        provider->async_functions == NULL || adapter_index >= provider->adapter_count)
        return TURBOWASM_INVALID_ARGUMENT;
    binding = &provider->async_functions[adapter_index];
    source = turbowasm_component_type_graph_get(lower->graph, lower->function_type);
    target = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    if (binding->instance == NULL || source == NULL || target == NULL ||
        source->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION || target->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        !source->as.function.is_async || !target->as.function.is_async ||
        source->as.function.param_count != target->as.function.param_count ||
        source->as.function.has_result != target->as.function.has_result) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; ; ++i) {
        bool result = i == source->as.function.param_count;
        turbowasm_component_type_ref a, b;
        if (result && !source->as.function.has_result) break;
        a = result ? source->as.function.result : source->as.function.params[i];
        b = result ? target->as.function.result : target->as.function.params[i];
        if (!turbowasm_component_value_type_equal(lower->graph, a, binding->graph, b)) return TURBOWASM_TYPE_MISMATCH;
        if (!turbowasm_component_value_type_async_importable(lower->graph, a)) return TURBOWASM_UNSUPPORTED;
        if (result) break;
    }
    if (provider->async_import_owners == UINT32_MAX) return TURBOWASM_OUT_OF_MEMORY;
    lower->async_provider = provider; lower->async_adapter_index = adapter_index;
    ++provider->async_import_owners;
    return TURBOWASM_OK;
}

static turbowasm_status commit_buffer(void *context,
    turbowasm_component_value *values, uint32_t count) {
    turbowasm_component_exec_async_transaction *transaction = context;
    turbowasm_status status;
    (void)values; (void)count;
    status = turbowasm_component_exec_resource_codec_preflight(&transaction->resources);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_component_endpoint_codec_commit(&transaction->endpoints);
    if (status != TURBOWASM_OK) return status;
    /* No callback/allocation between preflight and publication. */
    return turbowasm_component_exec_resource_codec_commit(&transaction->resources);
}

static turbowasm_status rollback_buffer(void *context) {
    turbowasm_component_exec_async_transaction *transaction = context;
    turbowasm_status status, cleanup;
    status = turbowasm_component_endpoint_codec_rollback(&transaction->endpoints);
    cleanup = turbowasm_component_exec_resource_codec_rollback(&transaction->resources);
    return status != TURBOWASM_OK ? status : cleanup;
}

static turbowasm_status commit_values(void *context, turbowasm_component_task *task,
    turbowasm_component_value *values, uint32_t count) {
    (void)task;
    return commit_buffer(context, values, count);
}

static turbowasm_status rollback_values(void *context, turbowasm_component_task *task) {
    (void)task;
    return rollback_buffer(context);
}

static void bind_codec(turbowasm_component_canonical_memory *memory, turbowasm_component_exec_async_transaction *codec,
    turbowasm_component_exec *exec) {
    codec->endpoints.table = &exec->resource_table;
    memory->endpoint_lift = turbowasm_component_endpoint_codec_lift;
    memory->endpoint_lower = turbowasm_component_endpoint_codec_lower;
    memory->endpoint_context = &codec->endpoints;
    turbowasm_component_exec_resource_codec_bind(&codec->resources, exec, memory);
}

static void bind_values(turbowasm_component_canonical_memory *memory,
    turbowasm_component_async_transaction *transaction, turbowasm_component_exec_async_transaction *codec,
    turbowasm_component_exec *exec) {
    bind_codec(memory, codec, exec);
    transaction->commit = commit_values;
    transaction->rollback = rollback_values;
    transaction->context = codec;
}

static turbowasm_status release_buffer(void *context) {
    turbowasm_component_exec_async_transaction *codec = context;
    turbowasm_status status = rollback_buffer(codec);
    --codec->resources.exec->async_buffer_owners;
    turbowasm_rt_free(codec);
    return status;
}

turbowasm_status turbowasm_component_exec_async_buffer_prepare(void *context, turbowasm_component_buffer *buffer) {
    turbowasm_component_exec *exec = context;
    turbowasm_component_exec_async_transaction *codec;
    if (exec == NULL || buffer == NULL || buffer->kind != TURBOWASM_COMPONENT_BUFFER_GUEST ||
        buffer->guest.release != NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (exec->async_buffer_owners >= exec->resource_table.max_entries) return TURBOWASM_OUT_OF_MEMORY;
    codec = turbowasm_rt_calloc(1u, sizeof(*codec));
    if (codec == NULL) return TURBOWASM_OUT_OF_MEMORY;
    bind_codec(&buffer->guest.memory, codec, exec);
    buffer->guest.commit = commit_buffer; buffer->guest.rollback = rollback_buffer;
    buffer->guest.release = release_buffer; buffer->guest.context = codec;
    ++exec->async_buffer_owners;
    return TURBOWASM_OK;
}

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
    turbowasm_component_exec *exec, *provider;
    uint32_t adapter;
    turbowasm_status status;
    uint32_t word;
    if (lower == NULL || lower->exec == NULL || !lower->is_async || caller == NULL ||
        result_count == NULL || trap == NULL || result_capacity < 1u || results == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    exec = lower->exec;
    if (exec->task_domain.active == NULL || exec->task_domain.active->destroying || !exec->may_leave ||
        exec->task_domain.synchronous_depth != 0u)
        return TURBOWASM_TRAPPED;
    provider = lower->async_provider != NULL ? lower->async_provider : exec;
    adapter = lower->async_provider != NULL ? lower->async_adapter_index : lower->local_adapter_index;
    if (provider->async_functions == NULL || adapter >= provider->adapter_count ||
        provider->async_functions[adapter].instance == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = collect(exec);
    if (status != TURBOWASM_OK) return status;
    if (provider->task_domain.count >= provider->task_domain.limit ||
        exec->async_call_count >= exec->task_domain.limit) return TURBOWASM_OUT_OF_MEMORY;
    binding.caller_graph = lower->graph; binding.caller_function_type = lower->function_type;
    binding.caller_domain = &exec->task_domain; binding.callee_domain = &provider->task_domain;
    binding.caller_memory = lower->memory;
    binding.callee = provider->async_functions[adapter];
    frame = turbowasm_rt_calloc(1u, sizeof(*frame));
    if (frame == NULL) return TURBOWASM_OUT_OF_MEMORY;
    /* These contexts survive guest realloc suspension and are never shared with
     * a recursive lower or a different call's pending result conversion. */
    bind_values(&binding.callee.memory, &binding.parameters, &frame->parameters, provider);
    bind_values(&binding.caller_memory, &binding.result, &frame->result, exec);
    if (provider != exec && binding.caller_memory.guest_realloc != NULL) {
        frame->result_realloc = lower->realloc_context;
        frame->result_realloc.progress_task = &frame->call.task;
        binding.caller_memory.realloc_context = &frame->result_realloc;
    }
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
    turbowasm_component_exec_async_call *frame;
    uint32_t local_tasks = 0u;
    if (exec == NULL || !exec->initialized || reason == TURBOWASM_OK || reason == TURBOWASM_YIELDED)
        return TURBOWASM_INVALID_ARGUMENT;
    if (exec->async_driving || exec->task_domain.active != NULL) return TURBOWASM_TRAPPED;
    /* The owner must unwind exported caller tasks before invalidating their
     * subtasks/result regions, including callers waiting through a shared set. */
    for (frame = exec->async_calls; frame != NULL; frame = frame->next)
        if (frame->call.task.domain == &exec->task_domain) ++local_tasks;
    if (exec->task_domain.count != local_tasks) return TURBOWASM_TRAPPED;
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
