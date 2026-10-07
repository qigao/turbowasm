#include "component_task_builtin.h"
#include "component_subtask.h"
#include "instance_internal.h"
#include <string.h>

static turbowasm_value_kind flat_kind(turbowasm_component_flat_type type) {
    static const turbowasm_value_kind kinds[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I64, TURBOWASM_VALUE_F32, TURBOWASM_VALUE_F64
    };
    return kinds[type];
}

turbowasm_status turbowasm_component_task_builtin_bind(turbowasm_component_task_builtin *binding,
    turbowasm_component_task_domain *domain, const turbowasm_component_type_graph *graph,
    const turbowasm_component_async_builtin *definition,
    const turbowasm_component_canonical_memory *resolved_memory) {
    turbowasm_component_flat_signature signature;
    turbowasm_component_canonical_memory memory = {0};
    turbowasm_status status;
    uint32_t i;
    if (binding == NULL || binding->domain != NULL || domain == NULL || domain->table == NULL ||
        graph == NULL || definition == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    switch (definition->kind) {
        case TURBOWASM_COMPONENT_TASK_RETURN: case TURBOWASM_COMPONENT_TASK_CANCEL:
        case TURBOWASM_COMPONENT_CONTEXT_GET: case TURBOWASM_COMPONENT_CONTEXT_SET:
        case TURBOWASM_COMPONENT_BACKPRESSURE_INC: case TURBOWASM_COMPONENT_BACKPRESSURE_DEC:
        case TURBOWASM_COMPONENT_THREAD_YIELD: case TURBOWASM_COMPONENT_WAITABLE_SET_NEW:
        case TURBOWASM_COMPONENT_WAITABLE_SET_DROP: case TURBOWASM_COMPONENT_WAITABLE_SET_WAIT:
        case TURBOWASM_COMPONENT_WAITABLE_SET_POLL: case TURBOWASM_COMPONENT_WAITABLE_JOIN:
        case TURBOWASM_COMPONENT_SUBTASK_CANCEL: case TURBOWASM_COMPONENT_SUBTASK_DROP:
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }
    if (definition->has_memory) {
        turbowasm_memory_desc desc;
        if (resolved_memory == NULL || resolved_memory->instance == NULL ||
            !turbowasm_module_memory_at(turbowasm_instance_module(resolved_memory->instance),
                resolved_memory->memory_index, &desc))
            return TURBOWASM_INVALID_ARGUMENT;
        memory = *resolved_memory;
        if (memory.pointer_type != (desc.memory64 ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32))
            return TURBOWASM_TYPE_MISMATCH;
    } else if (resolved_memory != NULL && resolved_memory->instance != NULL) {
        return TURBOWASM_INVALID_ARGUMENT;
    }
    memory.string_encoding = definition->string_encoding;
    status = turbowasm_component_async_builtin_signature(graph, definition, memory.pointer_type, &signature);
    if (status != TURBOWASM_OK) return status;
    memset(binding, 0, sizeof(*binding));
    binding->domain = domain; binding->graph = graph; binding->definition = *definition; binding->memory = memory;
    for (i = 0; i < signature.param_count; ++i) binding->params[i] = flat_kind(signature.params[i]);
    for (i = 0; i < signature.result_count; ++i) binding->results[i] = flat_kind(signature.results[i]);
    binding->type.params = binding->params; binding->type.param_count = signature.param_count;
    binding->type.results = binding->results; binding->type.result_count = signature.result_count;
    return TURBOWASM_OK;
}

static turbowasm_status suspend_builtin(turbowasm_component_task *task, turbowasm_host_call *call,
    int kind, turbowasm_component_resource_handle set) {
    turbowasm_host_wait wait = {0};
    int completion;
    turbowasm_status status;
    if (!turbowasm_host_call_can_wait(call) || task->builtin_wait != TURBOWASM_COMPONENT_TASK_WAIT_NONE)
        return TURBOWASM_TRAPPED;
    task->builtin_wait = kind;
    task->builtin_wait_set = set;
    status = turbowasm_host_call_wait(call, (uintptr_t)task, &wait, &completion);
    task->builtin_wait = TURBOWASM_COMPONENT_TASK_WAIT_NONE; task->builtin_wait_set = 0;
    if (status != TURBOWASM_OK) return status;
    return completion == 0 ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}

static turbowasm_status set_event(turbowasm_component_task_builtin *binding, turbowasm_host_call *call,
    const turbowasm_value *arguments, turbowasm_value *result) {
    turbowasm_component_resource_handle set = (uint32_t)arguments[0].as.i32;
    turbowasm_component_event event;
    turbowasm_status status, cleanup;
    bool waiting = binding->definition.kind == TURBOWASM_COMPONENT_WAITABLE_SET_WAIT;
    uint64_t address = binding->memory.pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? (uint64_t)arguments[1].as.i64 : (uint32_t)arguments[1].as.i32;
    if (waiting) {
        status = turbowasm_component_waitable_set_wait_acquire(binding->domain->table, set);
        if (status != TURBOWASM_OK) return status;
    }
    for (;;) {
        status = turbowasm_component_waitable_set_poll(binding->domain->table, set, &event);
        if (status != TURBOWASM_OK || !waiting || event.code != TURBOWASM_COMPONENT_EVENT_NONE) break;
        status = suspend_builtin(binding->domain->active, call, TURBOWASM_COMPONENT_TASK_WAIT_SET, set);
        if (status != TURBOWASM_OK) break;
    }
    if (waiting) {
        cleanup = turbowasm_component_waitable_set_wait_release(binding->domain->table, set);
        if (status == TURBOWASM_OK) status = cleanup;
    }
    if (status != TURBOWASM_OK) return status;
    /* Canonical unpack_event consumes the event first, then performs two stores
     * in order. A second-store trap may leave the first payload visible. */
    {
        turbowasm_component_type_ref u32 = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32);
        turbowasm_component_value value = {0};
        value.kind = TURBOWASM_COMPONENT_TYPE_U32; value.as.u32 = event.handle;
        status = turbowasm_component_canonical_lower_value(binding->graph, u32, &binding->memory, address, &value);
        if (status != TURBOWASM_OK) return status;
        if (address > UINT64_MAX - 4u) return TURBOWASM_TRAPPED;
        value.as.u32 = event.payload;
        status = turbowasm_component_canonical_lower_value(binding->graph, u32, &binding->memory, address + 4u, &value);
        if (status != TURBOWASM_OK) return status;
    }
    result->kind = TURBOWASM_VALUE_I32; result->as.i32 = (int32_t)event.code;
    return TURBOWASM_OK;
}

static turbowasm_status cancel_subtask(turbowasm_component_task_builtin *binding,
    turbowasm_host_call *call, turbowasm_component_resource_handle handle, turbowasm_value *result) {
    turbowasm_status status, cleanup;
    uint32_t phase;
    status = turbowasm_component_subtask_cancel_begin(binding->domain->table, handle, call);
    if (status != TURBOWASM_OK) return status;
    for (;;) {
        status = turbowasm_component_subtask_cancel_poll(binding->domain->table, handle, &phase);
        if (status != TURBOWASM_YIELDED) break; /* poll releases the pin on terminal/error */
        if (binding->definition.is_async) {
            status = turbowasm_component_waitable_wait_cancel(binding->domain->table, handle);
            phase = UINT32_MAX;
            break;
        }
        status = suspend_builtin(binding->domain->active, call, TURBOWASM_COMPONENT_TASK_WAIT_SUBTASK, handle);
        if (status != TURBOWASM_OK) {
            cleanup = turbowasm_component_waitable_wait_cancel(binding->domain->table, handle);
            (void)cleanup; /* preserve the wait/unwind failure */
            break;
        }
    }
    if (status == TURBOWASM_OK) { result->kind = TURBOWASM_VALUE_I32; result->as.i32 = (int32_t)phase; }
    return status;
}

turbowasm_status turbowasm_component_task_builtin_invoke(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t result_capacity, size_t *result_count, turbowasm_trap *trap) {
    turbowasm_component_task_builtin *binding = context;
    turbowasm_component_task *task;
    turbowasm_value result = {0};
    turbowasm_status status = TURBOWASM_OK;
    size_t i;
    if (binding == NULL || binding->domain == NULL || result_count == NULL || trap == NULL ||
        argument_count != binding->type.param_count || (argument_count != 0u && arguments == NULL) ||
        result_capacity < binding->type.result_count || (binding->type.result_count != 0u && results == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0; i < argument_count; ++i)
        if (arguments[i].kind != binding->params[i]) return TURBOWASM_TYPE_MISMATCH;
    task = binding->domain->active;
    if (task == NULL || task->destroying) return TURBOWASM_TRAPPED;
    if (binding->definition.kind != TURBOWASM_COMPONENT_CONTEXT_GET &&
        binding->definition.kind != TURBOWASM_COMPONENT_CONTEXT_SET &&
        binding->definition.kind != TURBOWASM_COMPONENT_BACKPRESSURE_INC &&
        binding->definition.kind != TURBOWASM_COMPONENT_BACKPRESSURE_DEC && !*binding->domain->may_leave)
        return TURBOWASM_TRAPPED;
    result.kind = TURBOWASM_VALUE_I32;
    switch (binding->definition.kind) {
        case TURBOWASM_COMPONENT_TASK_RETURN:
            status = turbowasm_component_task_return_flat(binding->domain, binding->graph,
                binding->definition.has_result, binding->definition.result, &binding->memory, arguments, argument_count);
            break;
        case TURBOWASM_COMPONENT_TASK_CANCEL:
            status = turbowasm_component_task_cancel(binding->domain); break;
        case TURBOWASM_COMPONENT_SUBTASK_CANCEL:
            status = cancel_subtask(binding, call, (uint32_t)arguments[0].as.i32, &result); break;
        case TURBOWASM_COMPONENT_SUBTASK_DROP:
            status = turbowasm_component_subtask_drop(binding->domain->table, (uint32_t)arguments[0].as.i32); break;
        case TURBOWASM_COMPONENT_CONTEXT_GET:
            result.kind = binding->results[0];
            if (result.kind == TURBOWASM_VALUE_I64) result.as.i64 = (int64_t)task->context_storage[binding->definition.context_index];
            else result.as.i32 = (int32_t)task->context_storage[binding->definition.context_index];
            break;
        case TURBOWASM_COMPONENT_CONTEXT_SET:
            task->context_storage[binding->definition.context_index] = arguments[0].kind == TURBOWASM_VALUE_I64
                ? (uint64_t)arguments[0].as.i64 : (uint32_t)arguments[0].as.i32;
            break;
        case TURBOWASM_COMPONENT_BACKPRESSURE_INC:
        case TURBOWASM_COMPONENT_BACKPRESSURE_DEC:
            status = turbowasm_component_task_backpressure(binding->domain,
                binding->definition.kind == TURBOWASM_COMPONENT_BACKPRESSURE_INC); break;
        case TURBOWASM_COMPONENT_WAITABLE_SET_NEW: {
            turbowasm_component_resource_handle handle;
            status = turbowasm_component_task_set_new(binding->domain, &handle);
            if (status == TURBOWASM_OK) result.as.i32 = (int32_t)handle;
            break;
        }
        case TURBOWASM_COMPONENT_WAITABLE_SET_DROP:
            status = turbowasm_component_task_set_drop(binding->domain, (uint32_t)arguments[0].as.i32); break;
        case TURBOWASM_COMPONENT_WAITABLE_JOIN:
            status = turbowasm_component_waitable_join(binding->domain->table,
                (uint32_t)arguments[0].as.i32, (uint32_t)arguments[1].as.i32); break;
        case TURBOWASM_COMPONENT_WAITABLE_SET_WAIT:
        case TURBOWASM_COMPONENT_WAITABLE_SET_POLL:
            status = set_event(binding, call, arguments, &result); break;
        case TURBOWASM_COMPONENT_THREAD_YIELD:
            status = suspend_builtin(task, call, TURBOWASM_COMPONENT_TASK_WAIT_YIELD, 0); break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }
    if (status != TURBOWASM_OK) {
        if (status == TURBOWASM_TRAPPED) *trap = TURBOWASM_TRAP_UNREACHABLE;
        return status;
    }
    if (binding->type.result_count != 0u) results[0] = result;
    *result_count = binding->type.result_count; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
