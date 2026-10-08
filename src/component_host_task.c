#include "component_api_internal.h"
#include "runtime_alloc.h"
#include <string.h>

typedef struct component_host_task_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_budget *budget;
    turbowasm_component_host_arguments arguments;
    turbowasm_component_host_result result;
    turbowasm_component_value canonical;
    turbowasm_component_task task;
    turbowasm_component_exec_resource_codec resources;
    turbowasm_component_exec_realloc_context realloc;
    turbowasm_component_host_registration registration;
    bool result_loaded, delivered, driving;
} component_host_task_impl;

static bool shutdown_busy(const void *context) {
    const component_host_task_impl *impl = context;
    return impl->driving;
}
static turbowasm_status shutdown_cancel(void *context) {
    component_host_task_impl *impl = context;
    turbowasm_status status;
    if (impl->task.state >= TURBOWASM_EXECUTION_COMPLETED || impl->task.cancellation_requested ||
        impl->task.phase == TURBOWASM_COMPONENT_TASK_RETURNED || impl->task.phase == TURBOWASM_COMPONENT_TASK_CANCELLED)
        return TURBOWASM_OK;
    impl->driving = true;
    status = turbowasm_component_task_request_cancel(&impl->task);
    impl->driving = false;
    return status;
}

static turbowasm_status rollback_parameters(component_host_task_impl *impl, turbowasm_status status) {
    turbowasm_status cleanup = turbowasm_component_exec_resource_codec_rollback(&impl->resources);
    return status != TURBOWASM_OK ? status : cleanup;
}

static turbowasm_status prepare_parameters(void *context, turbowasm_component_task *task,
    turbowasm_value *out, size_t capacity, size_t *out_count) {
    component_host_task_impl *impl = context;
    const turbowasm_component_task_binding *binding = &task->binding;
    const turbowasm_component_type *type = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    const turbowasm_component_value *values = turbowasm_component_host_arguments_values(&impl->arguments, NULL);
    const turbowasm_component_canonical_memory *memory = &binding->memory;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t cursor = 0u, i;
    if (capacity < task->signature.param_count) return TURBOWASM_TYPE_MISMATCH;
    if (task->signature.params_indirect) {
        turbowasm_component_layout layout;
        uint64_t address = 0u;
        if (memory->guest_realloc == NULL) return TURBOWASM_UNSUPPORTED;
        status = turbowasm_component_canonical_parameter_layout(binding->graph, binding->function_type,
            memory->pointer_type, &layout);
        if (status == TURBOWASM_OK)
            status = memory->guest_realloc(memory->realloc_context, 0u, 0u, layout.alignment, layout.size, &address);
        if (status == TURBOWASM_OK)
            status = turbowasm_component_canonical_lower_parameters(binding->graph, binding->function_type,
                memory, address, values);
        if (status == TURBOWASM_OK) {
            if (memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I32 && address > UINT32_MAX) status = TURBOWASM_TRAPPED;
            else {
                out[0].kind = memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64 ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
                if (out[0].kind == TURBOWASM_VALUE_I64) out[0].as.i64 = (int64_t)address;
                else out[0].as.i32 = (int32_t)address;
                cursor = 1u;
            }
        }
    } else for (i = 0u; i < type->as.function.param_count; ++i) {
        uint32_t count = 0u;
        status = turbowasm_component_canonical_lower_flat_value(binding->graph, type->as.function.params[i],
            memory, &values[i], out + cursor, (uint32_t)(capacity - cursor), &count);
        if (status != TURBOWASM_OK) break;
        cursor += count;
    }
    if (status == TURBOWASM_OK && cursor != task->signature.param_count) status = TURBOWASM_TYPE_MISMATCH;
    if (status == TURBOWASM_OK) status = turbowasm_component_exec_resource_codec_commit(&impl->resources);
    if (status == TURBOWASM_OK) status = turbowasm_component_host_arguments_published(&impl->arguments);
    if (status != TURBOWASM_OK) return rollback_parameters(impl, status);
    *out_count = cursor;
    return TURBOWASM_OK;
}

static turbowasm_status export_supported(const turbowasm_component_task_binding *binding) {
    const turbowasm_component_type *type = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    uint32_t i;
    for (i = 0u; ; ++i) {
        uint32_t features;
        bool result = i == type->as.function.param_count;
        turbowasm_component_type_ref ref;
        if (result && !type->as.function.has_result) break;
        ref = result ? type->as.function.result : type->as.function.params[i];
        if (!turbowasm_component_value_type_features(binding->graph, ref, &features)) return TURBOWASM_UNSUPPORTED;
        if (result) break;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_task_create(turbowasm_component_host_task *owner,
    turbowasm_component_instance_public_impl *instance, turbowasm_name name,
    const turbowasm_component_host_value *arguments, size_t count, bool move,
    turbowasm_component_host_budget *budget) {
    const turbowasm_component_task_binding *binding;
    component_host_task_impl *impl;
    turbowasm_component_task_binding target;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    if (owner == NULL || owner->impl != NULL || instance == NULL || !instance->exec.initialized ||
        instance->admission_closed || instance->shutdown_driving ||
        (name.size != 0u && name.bytes == NULL) || (count != 0u && arguments == NULL) ||
        !turbowasm_component_host_budget_valid(instance, budget))
        return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_exec_async_export(&instance->exec, name.bytes, name.size, &binding);
    if (status != TURBOWASM_OK) return status;
    status = export_supported(binding);
    if (status != TURBOWASM_OK) return status;
    if (instance->exec.task_domain.count >= instance->exec.task_domain.limit ||
        sizeof(*impl) > budget->limit - budget->used) return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_component_instance_public_impl_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(instance, true)) {
        turbowasm_component_instance_public_impl_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    budget->used += sizeof(*impl);
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    impl->instance = instance; impl->budget = budget;
    target = *binding;
    turbowasm_component_exec_resource_codec_bind(&impl->resources, &instance->exec, &target.memory);
    impl->resources.borrow_scope = &impl->task;
    if (target.memory.guest_realloc != NULL) {
        if (target.memory.realloc_context == NULL) { status = TURBOWASM_INVALID_ARGUMENT; goto fail; }
        impl->realloc = *(turbowasm_component_exec_realloc_context *)target.memory.realloc_context;
        impl->realloc.progress_task = &impl->task;
        target.memory.realloc_context = &impl->realloc;
    }
    status = turbowasm_component_host_arguments_prepare(&impl->arguments, instance,
        target.graph, target.function_type, arguments, count, move, true, budget);
    if (status != TURBOWASM_OK) goto fail;
    target.prepare = prepare_parameters; target.prepare_context = impl;
    status = turbowasm_component_task_create(&impl->task, &instance->exec.task_domain, &target);
    if (status != TURBOWASM_OK) goto fail;
    status = turbowasm_component_host_arguments_commit(&impl->arguments);
    if (status != TURBOWASM_OK) goto fail;
    owner->impl = impl;
    turbowasm_component_host_register(instance, &impl->registration, impl, shutdown_busy, shutdown_cancel);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    return TURBOWASM_OK;
fail:
    if (impl != NULL) {
        (void)turbowasm_component_task_destroy(&impl->task);
        (void)turbowasm_component_host_arguments_destroy(&impl->arguments);
        turbowasm_rt_free(impl);
    }
    budget->used -= sizeof(*impl);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}

const turbowasm_component_task *turbowasm_component_host_task_view(const turbowasm_component_host_task *owner) {
    const component_host_task_impl *impl = owner != NULL ? owner->impl : NULL;
    return impl != NULL ? &impl->task : NULL;
}

turbowasm_status turbowasm_component_host_task_resume(turbowasm_component_host_task *owner,
    const turbowasm_execution_options *options) {
    component_host_task_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_task_resume(&impl->task, options);
    impl->driving = false;
    return status;
}

turbowasm_status turbowasm_component_host_task_request_cancel(turbowasm_component_host_task *owner) {
    component_host_task_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_task_request_cancel(&impl->task);
    impl->driving = false;
    return status;
}

turbowasm_status turbowasm_component_host_task_take_result(turbowasm_component_host_task *owner,
    turbowasm_component_host_value *out, size_t *count) {
    component_host_task_impl *impl = owner != NULL ? owner->impl : NULL;
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving || impl->delivered || count == NULL ||
        (out != NULL && (int)out->kind != 0)) return TURBOWASM_INVALID_ARGUMENT;
    if (impl->task.state < TURBOWASM_EXECUTION_COMPLETED) return TURBOWASM_YIELDED;
    if (impl->task.status != TURBOWASM_OK) return impl->task.status;
    if (impl->task.phase == TURBOWASM_COMPONENT_TASK_CANCELLED) return TURBOWASM_INTERRUPTED;
    type = turbowasm_component_type_graph_get(impl->task.binding.graph, impl->task.binding.function_type);
    if (type->as.function.has_result && out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    if (!impl->result_loaded) {
        status = turbowasm_component_task_take_result(&impl->task, &impl->canonical);
        if (status != TURBOWASM_OK) goto done;
        impl->result_loaded = true;
    }
    if (type->as.function.has_result && impl->result.impl == NULL) {
        status = turbowasm_component_host_result_prepare(&impl->result, impl->instance,
            impl->task.binding.graph, type->as.function.result, &impl->canonical, impl->budget);
        if (status != TURBOWASM_OK) goto done;
    }
    status = turbowasm_component_host_arguments_destroy(&impl->arguments);
    if (status != TURBOWASM_OK) goto done;
    status = type->as.function.has_result ? turbowasm_component_host_result_take(&impl->result, out) : TURBOWASM_OK;
    if (status == TURBOWASM_OK) { *count = type->as.function.has_result ? 1u : 0u; impl->delivered = true; }
done:
    impl->driving = false;
    return status;
}

turbowasm_status turbowasm_component_host_task_destroy(turbowasm_component_host_task *owner) {
    component_host_task_impl *impl;
    turbowasm_component_instance_public_impl *instance;
    turbowasm_status status, cleanup;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl = owner->impl;
    if (impl == NULL) return TURBOWASM_OK;
    if (impl->driving || impl->instance->shutdown_driving || impl->task.state < TURBOWASM_EXECUTION_COMPLETED)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = impl->task.status;
    cleanup = turbowasm_component_task_destroy(&impl->task);
    if (impl->task.domain != NULL) {
        impl->driving = false; turbowasm_component_host_activity_leave(impl->instance); return cleanup;
    }
    if (status == TURBOWASM_OK) status = cleanup;
    cleanup = rollback_parameters(impl, TURBOWASM_OK);
    if (status == TURBOWASM_OK) status = cleanup;
    cleanup = turbowasm_component_host_arguments_destroy(&impl->arguments);
    if (impl->arguments.impl != NULL) {
        impl->driving = false; turbowasm_component_host_activity_leave(impl->instance); return cleanup;
    }
    if (status == TURBOWASM_OK) status = cleanup;
    cleanup = turbowasm_component_host_result_destroy(&impl->result);
    if (status == TURBOWASM_OK) status = cleanup;
    cleanup = turbowasm_component_value_destroy(&impl->canonical);
    if (status == TURBOWASM_OK) status = cleanup;
    instance = impl->instance;
    impl->budget->used -= sizeof(*impl); owner->impl = NULL;
    turbowasm_component_host_unregister(&impl->registration);
    turbowasm_rt_free(impl);
    turbowasm_component_host_activity_leave(instance);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}
