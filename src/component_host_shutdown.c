#include "component_api_internal.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"

bool turbowasm_component_host_activity_enter(turbowasm_component_instance_public_impl *instance,
    bool admission) {
    if (instance == NULL || instance->shutdown_complete || instance->shutdown_driving || instance->host_activity == UINT32_MAX ||
        (admission && instance->admission_closed)) return false;
    ++instance->host_activity;
    return true;
}

void turbowasm_component_host_activity_leave(turbowasm_component_instance_public_impl *instance) {
    --instance->host_activity;
}

void turbowasm_component_host_register(turbowasm_component_instance_public_impl *instance,
    turbowasm_component_host_registration *registration, void *context,
    bool (*busy)(const void *), turbowasm_status (*cancel)(void *)) {
    registration->context = context; registration->busy = busy; registration->cancel = cancel;
    registration->next = instance->host_owners; registration->previous = &instance->host_owners;
    if (registration->next != NULL) registration->next->previous = &registration->next;
    instance->host_owners = registration;
}

void turbowasm_component_host_unregister(turbowasm_component_host_registration *registration) {
    if (registration->previous == NULL) return;
    *registration->previous = registration->next;
    if (registration->next != NULL) registration->next->previous = registration->previous;
    registration->previous = NULL; registration->next = NULL;
}

turbowasm_status turbowasm_component_instance_request_shutdown_private(
    turbowasm_component_instance_public_impl *instance) {
    turbowasm_component_host_registration *registration;
    turbowasm_status status = TURBOWASM_OK;
    turbowasm_component_task_domain *domain;
    if (instance == NULL || !instance->exec.initialized || instance->exec.task_domain.table == NULL ||
        instance->shutdown_driving || instance->host_activity != 0u || instance->exec.async_driving)
        return TURBOWASM_INVALID_ARGUMENT;
    if (instance->shutdown_complete) return instance->shutdown_status;
    if (instance->shutdown != NULL) return TURBOWASM_OK;
    domain = &instance->exec.task_domain;
    /* A fuel-suspended callback retains exclusive admission without executing.
     * Cancellation only sets its request; its driver still owns the Core exit. */
    if (domain->active != NULL || domain->auxiliary != NULL ||
        domain->synchronous_depth != 0u) return TURBOWASM_INVALID_ARGUMENT;
    for (registration = instance->host_owners; registration != NULL; registration = registration->next)
        if (registration->busy != NULL && registration->busy(registration->context)) return TURBOWASM_INVALID_ARGUMENT;
    instance->admission_closed = true;
    instance->shutdown_driving = true;
    /* Owner operations cannot mutate registrations while this finite walk runs.
     * Hooks retain their bodies; pending events remain for the original owner. */
    for (registration = instance->host_owners; registration != NULL; registration = registration->next) {
        turbowasm_status cancellation = registration->cancel != NULL
            ? registration->cancel(registration->context) : TURBOWASM_OK;
        if (status == TURBOWASM_OK) status = cancellation;
    }
    instance->shutdown_driving = false;
    return status;
}

typedef struct turbowasm_component_shutdown_drain {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_task task;
    turbowasm_component_type unit;
    turbowasm_component_type_graph graph;
    bool entered;
} turbowasm_component_shutdown_drain;

static void cleanup_status(turbowasm_component_instance_public_impl *instance, turbowasm_status status) {
    if (instance->shutdown_status == TURBOWASM_OK) instance->shutdown_status = status;
}

static bool external_owners(const turbowasm_component_instance_public_impl *instance) {
    const turbowasm_component_exec *exec = &instance->exec;
    return instance->host_owners != NULL || instance->host_transfer_count != 0u || instance->resource_count != 0u ||
        exec->async_call_count != 0u || exec->async_resource_owners != 0u ||
        exec->async_import_owners != 0u;
}

static bool task_quiescent(const turbowasm_component_task_domain *domain) {
    return domain->count == 0u && domain->active == NULL && domain->exclusive == NULL &&
        domain->auxiliary == NULL && domain->synchronous_depth == 0u;
}

static turbowasm_status drop_resource(void *context, uint64_t identity, turbowasm_value rep) {
    turbowasm_component_instance_public_impl *instance = context;
    /* The active internal task supplies the existing retained Core control. */
    return turbowasm_component_exec_resource_release(&instance->exec, identity, rep);
}

static void drain_handles(turbowasm_component_instance_public_impl *instance) {
    turbowasm_component_resource_table *table = &instance->exec.resource_table;
    uint32_t slot, capacity = table->capacity;
    for (slot = 0u; slot < capacity; ++slot) {
        turbowasm_component_resource_handle handle;
        turbowasm_component_handle_kind kind;
        turbowasm_component_endpoint *endpoint;
        void *object;
        turbowasm_status status;
        if (!turbowasm_component_handle_at(table, slot, &handle, &kind, &object)) continue;
        if (kind == TURBOWASM_COMPONENT_HANDLE_RESOURCE) {
            const turbowasm_component_resource_entry *entry = &table->entries[slot];
            uint64_t identity = entry->resource_identity;
            if (!entry->owned || entry->lend_count != 0u || entry->borrow_scope != NULL) continue;
            /* No entry pointer survives a destructor, which can grow the table.
             * Removal commits before guest execution, including suspension. */
            status = turbowasm_component_resource_drop(table, handle, identity, drop_resource, instance);
            cleanup_status(instance, status);
            if (status == TURBOWASM_INTERRUPTED) break;
        } else if (kind >= TURBOWASM_COMPONENT_HANDLE_STREAM_READ && kind <= TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE) {
            endpoint = turbowasm_component_endpoint_get(table, handle, kind);
            if (endpoint == NULL || endpoint->waitable.sync_waiter || endpoint->waitable.delivering ||
                endpoint->lower_scope != NULL || endpoint->value_owner != NULL) continue;
            if (endpoint->operation != NULL) {
                turbowasm_component_event event;
                if (endpoint->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING) {
                    status = turbowasm_component_endpoint_cancel(endpoint);
                    if (status != TURBOWASM_OK) continue;
                }
                status = turbowasm_component_endpoint_take(endpoint, &event);
                if (endpoint->operation != NULL) continue;
                cleanup_status(instance, status);
            }
            /* A borrowed endpoint object remains valid, with its local handle
             * detached. Pair storage is collected only after this finite pass. */
            (void)turbowasm_component_endpoint_close(endpoint);
        }
    }
    /* Membership and active waits keep sets pending. Independently owned
     * subtasks/reservations remain with their actual release authority. */
    for (slot = 0u; slot < capacity; ++slot) {
        turbowasm_component_resource_handle handle;
        turbowasm_component_handle_kind kind;
        void *object;
        if (turbowasm_component_handle_at(table, slot, &handle, &kind, &object) &&
            kind == TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET)
            (void)turbowasm_component_task_set_drop(&instance->exec.task_domain, handle);
    }
}

static turbowasm_status drain_entry(void *context, turbowasm_component_task *task, turbowasm_host_call *call) {
    turbowasm_component_shutdown_drain *drain = context;
    (void)call;
    drain->entered = true;
    drain_handles(drain->instance);
    return turbowasm_component_task_return(task->domain, NULL);
}

static turbowasm_instance *core_instance(turbowasm_component_exec *exec) {
    uint32_t index;
    for (index = 0u; index < exec->core_instance_count; ++index)
        if (turbowasm_instance_module(&exec->core_instances[index]) != NULL) return &exec->core_instances[index];
    return NULL;
}

static turbowasm_status drain_core(turbowasm_component_exec *exec, turbowasm_instance **out) {
    turbowasm_instance *core = core_instance(exec);
    uint32_t slot;
    if (core != NULL) { *out = core; return TURBOWASM_OK; }
    /* A Core-free consumer may still use a defining Core provider. The existing
     * resource identity view owns this borrowed provider-lifetime contract. */
    for (slot = 0u; slot < exec->resource_table.capacity; ++slot) {
        const turbowasm_component_resource_entry *entry = &exec->resource_table.entries[slot];
        turbowasm_component_exec *provider = exec;
        uint64_t identity = entry->resource_identity;
        uint32_t depth;
        if (!entry->occupied || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE || !entry->owned ||
            entry->lend_count != 0u || entry->borrow_scope != NULL) continue;
        for (depth = 0u; depth < TURBOWASM_COMPONENT_TASK_MAX_DEPENDENCY_DEPTH; ++depth) {
            const turbowasm_component_resource_identity *resolved =
                turbowasm_component_exec_resource_identity(provider, identity);
            if (resolved == NULL || resolved->provider == NULL) break;
            provider = resolved->provider; identity = resolved->provider_declaration;
            core = core_instance(provider);
            if (core != NULL) { *out = core; return TURBOWASM_OK; }
        }
        if (depth == TURBOWASM_COMPONENT_TASK_MAX_DEPENDENCY_DEPTH) return TURBOWASM_TRAPPED;
    }
    *out = NULL; return TURBOWASM_OK;
}

static turbowasm_status create_drain(turbowasm_component_instance_public_impl *instance, turbowasm_instance *core) {
    turbowasm_component_shutdown_drain *drain;
    turbowasm_component_task_binding binding = {0};
    turbowasm_status status;
    if (!turbowasm_component_instance_public_impl_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    drain = turbowasm_rt_calloc(1u, sizeof(*drain));
    if (drain == NULL) {
        turbowasm_component_instance_public_impl_release(instance); return TURBOWASM_OUT_OF_MEMORY;
    }
    drain->instance = instance;
    drain->unit.kind = TURBOWASM_COMPONENT_TYPE_FUNCTION; drain->unit.as.function.is_async = true;
    drain->graph.types = &drain->unit; drain->graph.count = 1u;
    binding.instance = core; binding.graph = &drain->graph;
    binding.host_entry = drain_entry; binding.host_context = drain; binding.host_import = true;
    status = turbowasm_component_task_create(&drain->task, &instance->exec.task_domain, &binding);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(drain); turbowasm_component_instance_public_impl_release(instance); return status;
    }
    instance->shutdown = drain;
    return TURBOWASM_OK;
}

static bool drained(turbowasm_component_instance_public_impl *instance) {
    return !external_owners(instance) && task_quiescent(&instance->exec.task_domain) &&
        instance->exec.resource_table.live_count == 0u && instance->exec.task_domain.pairs == NULL &&
        instance->exec.task_domain.sets == NULL && instance->exec.async_buffer_owners == 0u && !instance->exec.async_driving &&
        instance->ref_count <= 2u; /* Caller plus this poll's temporary reference. */
}

turbowasm_status turbowasm_component_instance_poll_shutdown_private(
    turbowasm_component_instance_public_impl *instance, const turbowasm_execution_options *options) {
    turbowasm_component_host_registration *registration;
    turbowasm_component_shutdown_drain *drain;
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_YIELDED, cleanup;
    bool entered;
    if (instance == NULL || !instance->admission_closed || !instance->exec.initialized ||
        instance->shutdown_driving || instance->host_activity != 0u || instance->exec.task_domain.active != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (instance->shutdown_complete) return instance->shutdown_status;
    for (registration = instance->host_owners; registration != NULL; registration = registration->next)
        if (registration->busy != NULL && registration->busy(registration->context)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_instance_public_impl_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(&instance->component->binary.config);
    instance->shutdown_driving = true;
    if (instance->shutdown == NULL) {
        turbowasm_instance *core;
        if (external_owners(instance) || !task_quiescent(&instance->exec.task_domain) || instance->exec.async_driving)
            goto done;
        if (instance->exec.resource_table.live_count == 0u) goto audit;
        status = drain_core(&instance->exec, &core);
        if (status != TURBOWASM_OK) goto done;
        if (core == NULL) { drain_handles(instance); goto audit; }
        status = create_drain(instance, core);
        if (status != TURBOWASM_OK) goto done;
    }
    drain = instance->shutdown;
    status = turbowasm_component_task_resume(&drain->task, options);
    if (drain->task.state < TURBOWASM_EXECUTION_COMPLETED) { status = TURBOWASM_YIELDED; goto done; }
    if (drain->entered) cleanup_status(instance, status);
    cleanup = turbowasm_component_task_destroy(&drain->task);
    if (drain->task.domain != NULL) { status = TURBOWASM_YIELDED; goto done; }
    if (drain->entered) cleanup_status(instance, cleanup);
    else if (status == TURBOWASM_OK) status = cleanup;
    entered = drain->entered;
    instance->shutdown = NULL;
    turbowasm_rt_free(drain);
    turbowasm_component_instance_public_impl_release(instance);
    /* A failed startup has not consumed a handle or cleanup obligation. */
    if (!entered && status != TURBOWASM_OK) goto done;
audit:
    turbowasm_component_endpoint_domain_collect(&instance->exec.task_domain);
    if (drained(instance)) {
        instance->shutdown_complete = true; status = instance->shutdown_status;
    } else status = TURBOWASM_YIELDED;
done:
    instance->shutdown_driving = false;
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}
