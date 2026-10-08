#include "component_host_endpoint_internal.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"

static bool shutdown_busy(const void *context) {
    const component_host_endpoint_impl *impl = context;
    return impl->driving || impl->endpoint->waitable.delivering || impl->endpoint->waitable.sync_waiter;
}
static turbowasm_status shutdown_cancel(void *context) {
    component_host_endpoint_impl *impl = context;
    if (impl->endpoint->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_COPYING)
        return TURBOWASM_OK;
    return turbowasm_component_endpoint_cancel(impl->endpoint);
}
void turbowasm_component_host_endpoint_register(component_host_endpoint_impl *impl) {
    turbowasm_component_host_register(impl->instance, &impl->registration, impl, shutdown_busy, shutdown_cancel);
}

static void release_body(turbowasm_component_host_endpoint *owner, component_host_endpoint_impl *impl) {
    turbowasm_component_instance_public_impl *instance = impl->instance;
    impl->budget->used -= sizeof(*impl); owner->impl = NULL;
    turbowasm_component_host_unregister(&impl->registration);
    turbowasm_rt_free(impl);
    turbowasm_component_endpoint_domain_collect(&instance->exec.task_domain);
    turbowasm_component_host_activity_leave(instance);
    turbowasm_component_instance_public_impl_release(instance);
}

turbowasm_status turbowasm_component_host_endpoint_pair_create(
    turbowasm_component_host_endpoint *reader, turbowasm_component_host_endpoint *writer,
    turbowasm_component_instance_public_impl *instance, uint32_t type,
    turbowasm_component_host_budget *budget) {
    component_host_endpoint_impl *read = NULL, *write = NULL;
    turbowasm_component_endpoint *read_end = NULL, *write_end = NULL;
    const turbowasm_component_type *definition;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    if (reader == NULL || writer == NULL || reader == writer || reader->impl != NULL || writer->impl != NULL ||
        instance == NULL || !instance->exec.initialized || !turbowasm_component_host_budget_valid(instance, budget) ||
        instance->admission_closed || instance->shutdown_driving ||
        instance->exec.task_domain.pair_owner != instance || instance->exec.task_domain.pair_retain == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    definition = turbowasm_component_type_graph_get(&instance->exec.binary->type_graph, type);
    if (definition == NULL || (definition->kind != TURBOWASM_COMPONENT_TYPE_FUTURE &&
        definition->kind != TURBOWASM_COMPONENT_TYPE_STREAM)) return TURBOWASM_TYPE_MISMATCH;
    turbowasm_component_endpoint_domain_collect(&instance->exec.task_domain);
    if (instance->exec.task_domain.pair_count >= instance->exec.resource_table.max_entries ||
        sizeof(*read) > (budget->limit - budget->used) / 2u) return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_component_instance_public_impl_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_instance_public_impl_retain(instance)) {
        turbowasm_component_instance_public_impl_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    if (!turbowasm_component_host_activity_enter(instance, true)) {
        turbowasm_component_instance_public_impl_release(instance);
        turbowasm_component_instance_public_impl_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    budget->used += 2u * sizeof(*read);
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    read = turbowasm_rt_calloc(1u, sizeof(*read));
    if (read == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    write = turbowasm_rt_calloc(1u, sizeof(*write));
    if (write == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    status = turbowasm_component_endpoint_domain_pair_open(&instance->exec.task_domain,
        &instance->exec.binary->type_graph, type, false, &read_end, &write_end);
    if (status != TURBOWASM_OK) goto fail;
    read->instance = write->instance = instance; read->budget = write->budget = budget;
    read->endpoint = read_end; write->endpoint = write_end;
    reader->impl = read; writer->impl = write;
    turbowasm_component_host_endpoint_register(read); turbowasm_component_host_endpoint_register(write);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    return TURBOWASM_OK;
fail:
    turbowasm_rt_free(read); turbowasm_rt_free(write);
    budget->used -= 2u * sizeof(*read);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    turbowasm_component_instance_public_impl_release(instance);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}

const turbowasm_component_endpoint *turbowasm_component_host_endpoint_view(
    const turbowasm_component_host_endpoint *owner) {
    const component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    return impl != NULL && !impl->driving ? impl->endpoint : NULL;
}

turbowasm_status turbowasm_component_host_endpoint_submit(turbowasm_component_host_endpoint *owner,
    turbowasm_component_buffer *buffer, turbowasm_component_task *driver) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->admission_closed || impl->instance->shutdown_driving)
        return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    scope = turbowasm_runtime_scope_enter(&impl->instance->exec.binary->config);
    status = turbowasm_component_endpoint_submit_from_task(impl->endpoint, buffer, driver);
    turbowasm_runtime_scope_leave(scope);
    impl->driving = false;
    return status;
}
turbowasm_status turbowasm_component_host_endpoint_take(turbowasm_component_host_endpoint *owner,
    turbowasm_component_event *event) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_endpoint_take(impl->endpoint, event);
    impl->driving = false;
    return status;
}
turbowasm_status turbowasm_component_host_endpoint_cancel(turbowasm_component_host_endpoint *owner) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_endpoint_cancel(impl->endpoint);
    impl->driving = false;
    return status;
}

turbowasm_status turbowasm_component_host_endpoint_into_value(
    turbowasm_component_host_endpoint *owner, turbowasm_component_value *out) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving || !impl->endpoint->readable)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    scope = turbowasm_runtime_scope_enter(&impl->instance->exec.binary->config);
    status = turbowasm_component_endpoint_into_value(impl->endpoint, out);
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK) {
        impl->driving = false; turbowasm_component_host_activity_leave(impl->instance); return status;
    }
    release_body(owner, impl);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_endpoint_from_value(
    turbowasm_component_host_endpoint *owner, turbowasm_component_instance_public_impl *instance,
    turbowasm_component_value *source, turbowasm_component_host_budget *budget) {
    const turbowasm_component_endpoint *end;
    component_host_endpoint_impl *impl;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    if (owner == NULL || owner->impl != NULL || instance == NULL || !instance->exec.initialized ||
        instance->shutdown_driving || !turbowasm_component_host_budget_valid(instance, budget))
        return TURBOWASM_INVALID_ARGUMENT;
    end = turbowasm_component_endpoint_value_get(source);
    if (end == NULL || end->lower_scope != NULL || !turbowasm_component_endpoint_domain_pair_retained(end))
        return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_canonical_validate_value(end->graph,
        turbowasm_component_type_ref_indexed(end->type), source);
    if (status != TURBOWASM_OK) return status;
    if (sizeof(*impl) > budget->limit - budget->used) return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_component_instance_public_impl_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(instance, false)) {
        turbowasm_component_instance_public_impl_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    budget->used += sizeof(*impl);
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    status = turbowasm_component_endpoint_take_value(source, &impl->endpoint);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(impl); goto fail; }
    impl->instance = instance; impl->budget = budget; owner->impl = impl;
    turbowasm_component_host_endpoint_register(impl);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    return TURBOWASM_OK;
fail:
    budget->used -= sizeof(*impl);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}

turbowasm_status turbowasm_component_host_endpoint_destroy(turbowasm_component_host_endpoint *owner) {
    component_host_endpoint_impl *impl;
    turbowasm_status status;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl = owner->impl;
    if (impl == NULL) return TURBOWASM_OK;
    if (impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_endpoint_close(impl->endpoint);
    if (status != TURBOWASM_OK) {
        impl->driving = false; turbowasm_component_host_activity_leave(impl->instance); return status;
    }
    release_body(owner, impl);
    return TURBOWASM_OK;
}
