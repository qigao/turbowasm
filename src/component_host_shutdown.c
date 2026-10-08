#include "component_api_internal.h"

bool turbowasm_component_host_activity_enter(turbowasm_component_instance_public_impl *instance,
    bool admission) {
    if (instance == NULL || instance->shutdown_driving || instance->host_activity == UINT32_MAX ||
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
    domain = &instance->exec.task_domain;
    /* A fuel-suspended callback retains exclusive admission without executing.
     * Cancellation only sets its request; its driver still owns the Core exit. */
    if (domain->active != NULL || domain->auxiliary != NULL ||
        domain->synchronous_depth != 0u) return TURBOWASM_INVALID_ARGUMENT;
    for (registration = instance->host_owners; registration != NULL; registration = registration->next)
        if (registration->busy(registration->context)) return TURBOWASM_INVALID_ARGUMENT;
    instance->admission_closed = true;
    instance->shutdown_driving = true;
    /* Owner operations cannot mutate registrations while this finite walk runs.
     * Hooks retain their bodies; pending events remain for the original owner. */
    for (registration = instance->host_owners; registration != NULL; registration = registration->next) {
        turbowasm_status cancellation = registration->cancel(registration->context);
        if (status == TURBOWASM_OK) status = cancellation;
    }
    instance->shutdown_driving = false;
    return status;
}
