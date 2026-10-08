#include "component_async_call.h"
#include "runtime_alloc.h"
#include <string.h>

static turbowasm_value_kind flat_kind(turbowasm_component_flat_type type) {
    static const turbowasm_value_kind kinds[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I64, TURBOWASM_VALUE_F32, TURBOWASM_VALUE_F64
    };
    return kinds[type];
}

static uint64_t pointer_value(const turbowasm_component_canonical_memory *memory, const turbowasm_value *value) {
    return memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? (uint64_t)value->as.i64 : (uint32_t)value->as.i32;
}

static turbowasm_status memory_valid(const turbowasm_component_canonical_memory *memory, bool required) {
    turbowasm_memory_desc desc;
    if (memory->pointer_type != TURBOWASM_COMPONENT_POINTER_I32 && memory->pointer_type != TURBOWASM_COMPONENT_POINTER_I64)
        return TURBOWASM_INVALID_ARGUMENT;
    if ((unsigned)memory->string_encoding > TURBOWASM_COMPONENT_STRING_LATIN1_UTF16) return TURBOWASM_INVALID_ARGUMENT;
    if (memory->instance == NULL) return required ? TURBOWASM_INVALID_ARGUMENT : TURBOWASM_OK;
    if (!turbowasm_module_memory_at(turbowasm_instance_module(memory->instance), memory->memory_index, &desc))
        return TURBOWASM_INVALID_ARGUMENT;
    return desc.memory64 == (memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64)
        ? TURBOWASM_OK : TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status transfer_valid(uint32_t features,
    const turbowasm_component_canonical_memory *source, const turbowasm_component_canonical_memory *destination,
    const turbowasm_component_async_transaction *transaction, bool source_host, bool destination_host) {
    if ((transaction->commit == NULL) != (transaction->rollback == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    if (destination_host && transaction->commit != NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (((features & TURBOWASM_COMPONENT_VALUE_RESOURCES) != 0u &&
         ((!source_host && source->resource_lift == NULL) || (!destination_host && destination->resource_lower == NULL))) ||
        ((features & TURBOWASM_COMPONENT_VALUE_ENDPOINTS) != 0u &&
         ((!source_host && source->endpoint_lift == NULL) || (!destination_host && destination->endpoint_lower == NULL))) ||
        ((features & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u &&
         ((!source_host && source->instance == NULL) ||
          (!destination_host && (destination->instance == NULL || destination->guest_realloc == NULL)))) ||
        (!destination_host && (features & (TURBOWASM_COMPONENT_VALUE_RESOURCES | TURBOWASM_COMPONENT_VALUE_ENDPOINTS)) != 0u &&
         transaction->commit == NULL)) return TURBOWASM_UNSUPPORTED;
    return TURBOWASM_OK;
}

static turbowasm_status release_arguments(void *context) {
    turbowasm_component_async_call *call = context;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t i;
    if (call->arguments == NULL) return TURBOWASM_OK;
    for (i = 0; i < call->argument_count; ++i) {
        turbowasm_status cleanup = turbowasm_component_value_destroy(&call->arguments[i]);
        if (status == TURBOWASM_OK) status = cleanup;
    }
    turbowasm_rt_free(call->arguments); call->arguments = NULL;
    return status;
}

static turbowasm_status prepare_arguments(void *context, turbowasm_component_task *task,
    turbowasm_value *out, size_t capacity, size_t *out_count) {
    turbowasm_component_async_call *call = context;
    const turbowasm_component_async_call_binding *binding = &call->binding;
    const turbowasm_component_type *source = turbowasm_component_type_graph_get(binding->caller_graph, binding->caller_function_type);
    const turbowasm_component_type *target = turbowasm_component_type_graph_get(binding->callee.graph, binding->callee.function_type);
    const turbowasm_component_canonical_memory *memory = &binding->callee.memory;
    turbowasm_status status;
    uint32_t i, cursor = 0;
    uint64_t address;
    if (capacity < task->signature.param_count) return TURBOWASM_TYPE_MISMATCH;
    if (call->argument_count != 0) {
        if ((size_t)call->argument_count > SIZE_MAX / sizeof(*call->arguments)) return TURBOWASM_OUT_OF_MEMORY;
        call->arguments = turbowasm_rt_calloc(call->argument_count, sizeof(*call->arguments));
        if (call->arguments == NULL) return TURBOWASM_OUT_OF_MEMORY;
    }
    if (call->caller_signature.params_indirect) {
        status = turbowasm_component_canonical_lift_parameters(binding->caller_graph, binding->caller_function_type,
            &binding->caller_memory, pointer_value(&binding->caller_memory, &call->raw[0]), call->arguments);
        if (status != TURBOWASM_OK) return status;
    } else for (i = 0; i < call->argument_count; ++i) {
        turbowasm_component_flat_type_list flat;
        status = turbowasm_component_canonical_flatten_type(binding->caller_graph, source->as.function.params[i],
            binding->caller_memory.pointer_type, &flat);
        if (status != TURBOWASM_OK) return status;
        status = turbowasm_component_canonical_lift_flat_value(binding->caller_graph, source->as.function.params[i],
            &binding->caller_memory, call->raw + cursor, flat.count, &call->arguments[i]);
        if (status != TURBOWASM_OK) return status;
        cursor += flat.count;
    }
    /* The host consumes these retained canonical values directly. No callee
     * memory/handles exist, and source loans last through terminal delivery. */
    if (binding->callee.host_entry != NULL) { *out_count = 0; return TURBOWASM_OK; }
    cursor = 0;
    if (task->signature.params_indirect) {
        turbowasm_component_layout layout;
        status = turbowasm_component_canonical_parameter_layout(binding->callee.graph, binding->callee.function_type,
            memory->pointer_type, &layout);
        if (status != TURBOWASM_OK) return status;
        status = memory->guest_realloc(memory->realloc_context, 0, 0, layout.alignment, layout.size, &address);
        if (status != TURBOWASM_OK) return status;
        status = turbowasm_component_canonical_lower_parameters(binding->callee.graph, binding->callee.function_type,
            memory, address, call->arguments);
        if (status == TURBOWASM_OK) {
            if (memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I32 && address > UINT32_MAX)
                status = TURBOWASM_TRAPPED;
            else {
                out[0].kind = memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64 ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
                if (out[0].kind == TURBOWASM_VALUE_I64) out[0].as.i64 = (int64_t)address;
                else out[0].as.i32 = (int32_t)address;
                cursor = 1;
            }
        }
    } else {
        status = TURBOWASM_OK;
        for (i = 0; i < call->argument_count; ++i) {
            uint32_t count = 0;
            status = turbowasm_component_canonical_lower_flat_value(binding->callee.graph, target->as.function.params[i],
                memory, &call->arguments[i], out + cursor, (uint32_t)(capacity - cursor), &count);
            if (status != TURBOWASM_OK) break;
            cursor += count;
        }
    }
    if (status == TURBOWASM_OK && cursor != task->signature.param_count) status = TURBOWASM_TYPE_MISMATCH;
    if (status == TURBOWASM_OK && binding->parameters.commit != NULL)
        status = binding->parameters.commit(binding->parameters.context, task, call->arguments, call->argument_count);
    if (status != TURBOWASM_OK) {
        if (binding->parameters.rollback != NULL) (void)binding->parameters.rollback(binding->parameters.context, task);
        return status;
    }
    *out_count = cursor;
    return TURBOWASM_OK;
}

static turbowasm_status lower_result(void *context, turbowasm_component_value *value) {
    turbowasm_component_async_call *call = context;
    const turbowasm_component_async_call_binding *binding = &call->binding;
    const turbowasm_component_type *function = turbowasm_component_type_graph_get(binding->caller_graph, binding->caller_function_type);
    turbowasm_status status;
    if (!function->as.function.has_result) return value == NULL ? TURBOWASM_OK : TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_canonical_lower_value(binding->caller_graph, function->as.function.result,
        &binding->caller_memory, call->result_address, value);
    if (status == TURBOWASM_OK && binding->result.commit != NULL)
        status = binding->result.commit(binding->result.context, &call->task, value, 1);
    if (status != TURBOWASM_OK) {
        if (binding->result.rollback != NULL) (void)binding->result.rollback(binding->result.context, &call->task);
        return status;
    }
    return turbowasm_component_value_destroy(value);
}

turbowasm_status turbowasm_component_async_call_create(turbowasm_component_async_call *call,
    const turbowasm_component_async_call_binding *binding, const turbowasm_value *arguments, size_t argument_count) {
    const turbowasm_component_type *source, *target;
    turbowasm_component_flat_signature signature, target_signature;
    turbowasm_component_task_binding callee;
    turbowasm_status status;
    uint32_t i, parameters = 0, result = 0;
    bool host;
    if (call == NULL || call->binding.callee_domain != NULL || binding == NULL ||
        binding->caller_domain == NULL || binding->caller_domain->table == NULL ||
        binding->caller_domain->may_leave == NULL || binding->callee_domain == NULL ||
        binding->callee.prepare != NULL || binding->callee.resolve != NULL || binding->callee.abandon != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    host = binding->callee.host_entry != NULL;
    if (!*binding->caller_domain->may_leave) return TURBOWASM_TRAPPED;
    source = turbowasm_component_type_graph_get(binding->caller_graph, binding->caller_function_type);
    target = turbowasm_component_type_graph_get(binding->callee.graph, binding->callee.function_type);
    if (source == NULL || target == NULL || source->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        target->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION || !source->as.function.is_async || !target->as.function.is_async ||
        source->as.function.param_count != target->as.function.param_count ||
        source->as.function.has_result != target->as.function.has_result) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0; i < source->as.function.param_count; ++i) {
        uint32_t features;
        if (!turbowasm_component_value_type_equal(binding->caller_graph, source->as.function.params[i],
                binding->callee.graph, target->as.function.params[i]) ||
            !turbowasm_component_transfer_type_features(binding->caller_graph, source->as.function.params[i], &features))
            return TURBOWASM_TYPE_MISMATCH;
        parameters |= features;
    }
    if (source->as.function.has_result &&
        (!turbowasm_component_value_type_equal(binding->caller_graph, source->as.function.result,
             binding->callee.graph, target->as.function.result) ||
         !turbowasm_component_transfer_type_features(binding->caller_graph, source->as.function.result, &result)))
        return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_canonical_flatten_function_abi(binding->caller_graph, binding->caller_function_type,
        binding->caller_memory.pointer_type, TURBOWASM_COMPONENT_CANONICAL_LOWER, TURBOWASM_COMPONENT_ABI_ASYNC, &signature);
    if (status != TURBOWASM_OK) return status;
    status = memory_valid(&binding->caller_memory, signature.params_indirect || source->as.function.has_result ||
        (parameters & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u);
    if (status != TURBOWASM_OK) return status;
    status = memory_valid(&binding->callee.memory, false);
    if (status != TURBOWASM_OK) return status;
    status = transfer_valid(parameters, &binding->caller_memory, &binding->callee.memory, &binding->parameters, false, host);
    if (status != TURBOWASM_OK) return status;
    status = transfer_valid(result, &binding->callee.memory, &binding->caller_memory, &binding->result, host, false);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_component_canonical_flatten_function_abi(binding->callee.graph, binding->callee.function_type,
        binding->callee.memory.pointer_type, TURBOWASM_COMPONENT_CANONICAL_LIFT,
        binding->callee.callback_instance ? TURBOWASM_COMPONENT_ABI_ASYNC_CALLBACK : TURBOWASM_COMPONENT_ABI_ASYNC, &target_signature);
    if (status != TURBOWASM_OK) return status;
    if (!host && target_signature.params_indirect && (binding->callee.memory.instance == NULL || binding->callee.memory.guest_realloc == NULL))
        return TURBOWASM_UNSUPPORTED;
    if (argument_count != signature.param_count || argument_count > sizeof(call->raw) / sizeof(call->raw[0]) ||
        (argument_count != 0 && arguments == NULL)) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0; i < argument_count; ++i) if (arguments[i].kind != flat_kind(signature.params[i])) return TURBOWASM_TYPE_MISMATCH;
    call->binding = *binding; call->caller_signature = signature; call->argument_count = source->as.function.param_count;
    if (argument_count != 0) memcpy(call->raw, arguments, argument_count * sizeof(*arguments));
    if (source->as.function.has_result) call->result_address = pointer_value(&binding->caller_memory, &arguments[argument_count - 1]);
    if (binding->caller_domain->active != NULL && binding->caller_domain->active->destroying) {
        memset(call, 0, sizeof(*call));
        return TURBOWASM_TRAPPED;
    }
    if (binding->caller_domain->active != NULL &&
        binding->caller_domain->active->dependency_depth >= TURBOWASM_COMPONENT_TASK_MAX_DEPENDENCY_DEPTH) {
        memset(call, 0, sizeof(*call));
        return TURBOWASM_OUT_OF_MEMORY;
    }
    callee = binding->callee; callee.prepare = prepare_arguments; callee.prepare_context = call;
    status = turbowasm_component_subtask_create(&call->subtask, binding->caller_domain->table, &call->task,
        binding->callee_domain, &callee, lower_result, release_arguments, call);
    if (status != TURBOWASM_OK) memset(call, 0, sizeof(*call));
    else if (binding->caller_domain->active != NULL)
        turbowasm_component_subtask_attach(&call->subtask, binding->caller_domain->active);
    return status;
}

turbowasm_status turbowasm_component_async_call_start(turbowasm_component_async_call *call,
    const turbowasm_host_call *caller, const turbowasm_execution_options *options, uint32_t *out_word) {
    turbowasm_status status;
    if (call == NULL || call->binding.callee_domain == NULL || out_word == NULL || (caller != NULL && options != NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (call->started || !*call->binding.caller_domain->may_leave) return TURBOWASM_TRAPPED;
    call->started = true;
    status = caller != NULL ? turbowasm_component_task_resume_from_host(&call->task, caller)
                           : turbowasm_component_task_resume(&call->task, options);
    if (status != TURBOWASM_OK && status != TURBOWASM_YIELDED) return status;
    return turbowasm_component_subtask_publish(&call->subtask, out_word);
}

turbowasm_status turbowasm_component_async_call_destroy(turbowasm_component_async_call *call) {
    turbowasm_status status, cleanup;
    if (call == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (call->binding.callee_domain == NULL) return TURBOWASM_OK;
    if (call->subtask.table != NULL && (call->subtask.waitable.sync_waiter || call->subtask.waitable.delivering ||
        (call->subtask.waitable.failure == TURBOWASM_OK && call->subtask.callee == NULL &&
         !call->subtask.waitable.state.subtask.resolve_delivered))) return TURBOWASM_TRAPPED;
    status = turbowasm_component_task_destroy(&call->task);
    if (call->task.domain != NULL) return status;
    cleanup = turbowasm_component_subtask_destroy(&call->subtask);
    if (status == TURBOWASM_OK) status = cleanup;
    if (call->subtask.table != NULL) return status;
    memset(call, 0, sizeof(*call));
    return status;
}
