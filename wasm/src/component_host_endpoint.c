#include "component_host_endpoint_internal.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"

static bool shutdown_busy(const void *context) {
    const component_host_endpoint_impl *impl = context;
    /* Committed, deferred input is an idle cancellation root. Preparing or
     * retiring it still excludes shutdown reentry; its former carrier is frozen. */
    bool driving = impl->driving && !impl->deferred_move;
    return driving || impl->endpoint->waitable.delivering || impl->endpoint->waitable.sync_waiter;
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

static void release_body(turbowasm_component_host_endpoint *owner, component_host_endpoint_impl *impl,
    bool activity) {
    turbowasm_component_instance_public_impl *instance = impl->instance;
    impl->budget->used -= sizeof(*impl); owner->impl = NULL;
    turbowasm_component_host_unregister(&impl->registration);
    turbowasm_rt_free(impl);
    turbowasm_component_endpoint_domain_collect(&instance->exec.task_domain);
    if (activity) turbowasm_component_host_activity_leave(instance);
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
    release_body(owner, impl, true);
    return TURBOWASM_OK;
}

static void begin_host_move_cleanup(void *context) {
    component_host_endpoint_impl *impl = context;
    impl->deferred_move = false;
    /* Close can release the creation instance and invalidate the pair storage.
     * Establish receiver exclusion before any allocator cleanup can reenter. */
    impl->move_cleanup_activity = turbowasm_component_host_activity_enter(impl->instance, false);
}

static void finish_host_move(void *context, bool transferred) {
    component_host_endpoint_impl *impl = context;
    if (!transferred) {
        impl->driving = false;
        turbowasm_component_host_activity_leave(impl->instance);
    } else {
        turbowasm_component_host_endpoint retired = {impl};
        /* Cleanup must also finish under a preexisting shutdown/activity guard.
         * In that case enter rejects and the existing guard excludes reentry. */
        turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&impl->instance->exec.binary->config);
        /* Canonical cleanup already closed, took or published the reader. */
        release_body(&retired, impl, impl->move_cleanup_activity);
        turbowasm_runtime_scope_leave(scope);
    }
}

turbowasm_status turbowasm_component_host_endpoint_move_prepare(
    turbowasm_component_host_endpoint *owner, turbowasm_component_value *out,
    const bool *admitted) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    turbowasm_component_value prepared = {0};
    if (out == NULL || out->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || out->release != NULL ||
        impl == NULL || impl->driving || admitted == NULL || *admitted ||
        impl->instance->shutdown_driving || !impl->endpoint->readable ||
        !turbowasm_component_endpoint_domain_pair_retained(impl->endpoint)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, true)) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    scope = turbowasm_runtime_scope_enter(&impl->instance->exec.binary->config);
    status = turbowasm_component_endpoint_into_value(impl->endpoint, &prepared);
    if (status == TURBOWASM_OK) {
        /* The fresh canonical record cannot already have another host hook. */
        status = turbowasm_component_endpoint_value_adopt(&prepared, admitted,
            begin_host_move_cleanup, finish_host_move, impl);
        if (status != TURBOWASM_OK) {
            turbowasm_component_endpoint *restored;
            (void)turbowasm_component_endpoint_take_value(&prepared, &restored);
        }
    }
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK) {
        impl->driving = false;
        turbowasm_component_host_activity_leave(impl->instance);
    }
    else *out = prepared;
    return status;
}

turbowasm_status turbowasm_component_host_endpoint_move_commit(
    turbowasm_component_host_endpoint *owner, const turbowasm_component_value *value) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    const turbowasm_component_endpoint *endpoint = turbowasm_component_endpoint_value_get(value);
    const turbowasm_component_endpoint_value_owner *record = endpoint != NULL ? endpoint->value_owner : NULL;
    if (impl == NULL || !impl->driving || endpoint != impl->endpoint || record == NULL ||
        record->host_finish != finish_host_move || record->host_context != impl ||
        record->host_admitted == NULL || !*record->host_admitted) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_component_host_endpoint_move_publish(owner);
    return TURBOWASM_OK;
}

bool turbowasm_component_host_endpoint_move_ready(const turbowasm_component_host_endpoint *owner,
    const turbowasm_component_value *value, const bool *admitted) {
    const component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    const turbowasm_component_endpoint *end = turbowasm_component_endpoint_value_get(value);
    const turbowasm_component_endpoint_value_owner *record = end != NULL ? end->value_owner : NULL;
    return impl != NULL && impl->driving && !impl->deferred_move && end == impl->endpoint &&
        record != NULL && !record->publishing && record->host_finish == finish_host_move &&
        record->host_context == impl && record->host_admitted == admitted && admitted != NULL && !*admitted;
}

void turbowasm_component_host_endpoint_move_publish(turbowasm_component_host_endpoint *owner) {
    component_host_endpoint_impl *impl = owner->impl;
    owner->impl = NULL; impl->deferred_move = true;
    turbowasm_component_host_activity_leave(impl->instance);
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

bool turbowasm_component_host_endpoint_destroy_ready(const turbowasm_component_host_endpoint *owner) {
    const component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    const turbowasm_component_endpoint *end;
    turbowasm_component_endpoint_state peer_state;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving ||
        impl->instance->host_activity == UINT32_MAX) return false;
    end = impl->endpoint;
    if (!end->initialized || end->closed || end->operation != NULL || end->lower_scope != NULL ||
        end->value_owner != NULL || end->waitable.table != NULL || end->waitable.sync_waiter || end->waitable.delivering ||
        (end->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE &&
         end->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_DONE)) return false;
    if (end->peer == NULL) return true;
    peer_state = end->peer->waitable.state.endpoint;
    return end->peer->peer == end && turbowasm_component_endpoint_peer_dropped(&peer_state);
}

turbowasm_status turbowasm_component_host_endpoint_destroy_locked(turbowasm_component_host_endpoint *owner) {
    component_host_endpoint_impl *impl;
    turbowasm_status status;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl = owner->impl;
    if (impl == NULL) return TURBOWASM_OK;
    if (!impl->driving || impl->deferred_move || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) {
        impl->driving = false; return TURBOWASM_INVALID_ARGUMENT;
    }
    status = turbowasm_component_endpoint_close(impl->endpoint);
    if (status != TURBOWASM_OK) {
        impl->driving = false; turbowasm_component_host_activity_leave(impl->instance); return status;
    }
    release_body(owner, impl, true);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_endpoint_destroy(turbowasm_component_host_endpoint *owner) {
    component_host_endpoint_impl *impl = owner != NULL ? owner->impl : NULL;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (impl == NULL) return TURBOWASM_OK;
    if (impl->driving || impl->instance->shutdown_driving) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    return turbowasm_component_host_endpoint_destroy_locked(owner);
}
