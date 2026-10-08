#include "component_subtask.h"
#include <string.h>

static bool resolved(const turbowasm_component_subtask *subtask) {
    return subtask->waitable.state.subtask.phase >= TURBOWASM_COMPONENT_SUBTASK_RETURNED;
}

static void detach(turbowasm_component_subtask *subtask) {
    turbowasm_component_subtask **link;
    if (subtask->parent == NULL) return;
    link = &subtask->parent->children;
    while (*link != NULL && *link != subtask) link = &(*link)->next_child;
    if (*link == subtask) *link = subtask->next_child;
    subtask->parent = NULL; subtask->next_child = NULL;
}

void turbowasm_component_subtask_attach(turbowasm_component_subtask *subtask, turbowasm_component_task *parent) {
    subtask->parent = parent; subtask->next_child = parent->children;
    parent->children = subtask;
    subtask->task_owner->dependency_depth = parent->dependency_depth + 1u;
}

void turbowasm_component_subtask_detach_children(turbowasm_component_task *parent) {
    while (parent->children != NULL) detach(parent->children);
}

turbowasm_status turbowasm_component_subtask_abort_children(turbowasm_component_task *parent) {
    turbowasm_status status = TURBOWASM_OK;
    while (parent->children != NULL) {
        turbowasm_component_subtask *child = parent->children;
        turbowasm_status cleanup;
        if (child->waitable.sync_waiter || child->waitable.delivering) return TURBOWASM_TRAPPED;
        if (child->waitable.failure == TURBOWASM_OK) child->waitable.failure = TURBOWASM_INTERRUPTED;
        cleanup = turbowasm_component_task_destroy(child->task_owner);
        if (status == TURBOWASM_OK) status = cleanup;
        if (child->task_owner->domain != NULL) return cleanup != TURBOWASM_OK ? cleanup : TURBOWASM_TRAPPED;
        cleanup = turbowasm_component_subtask_destroy(child);
        if (status == TURBOWASM_OK) status = cleanup;
        if (child->table != NULL) return cleanup != TURBOWASM_OK ? cleanup : TURBOWASM_TRAPPED;
    }
    return status;
}

static turbowasm_status release_loans(void *context) {
    turbowasm_component_subtask *subtask = context;
    turbowasm_status status = TURBOWASM_OK;
    if (subtask->released) return TURBOWASM_TRAPPED;
    detach(subtask);
    subtask->released = true;
    if (subtask->release != NULL) status = subtask->release(subtask->context);
    if (status != TURBOWASM_OK) subtask->waitable.failure = status;
    return status;
}

static turbowasm_component_subtask *get_subtask(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable *waitable = turbowasm_component_handle_object(
        table, handle, TURBOWASM_COMPONENT_HANDLE_SUBTASK);
    turbowasm_component_subtask *subtask;
    if (waitable == NULL || waitable->table != table || waitable->handle != handle ||
        waitable->release_pending != release_loans || waitable->release_context == NULL) return NULL;
    subtask = waitable->release_context;
    return &subtask->waitable == waitable ? subtask : NULL;
}

static turbowasm_status prepare_callee(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count) {
    turbowasm_component_subtask *subtask = context;
    if (!turbowasm_component_subtask_start(&subtask->waitable.state.subtask)) return TURBOWASM_TRAPPED;
    /* The wrapper is used once. Detach this reference before resolution could
     * let the caller drop its subtask while the callee continues running. */
    task->binding.prepare = NULL; task->binding.prepare_context = NULL;
    if (subtask->prepare != NULL)
        return subtask->prepare(subtask->prepare_context, task, arguments, capacity, out_count);
    *out_count = 0;
    return TURBOWASM_OK;
}

static turbowasm_status resolve_callee(void *context, turbowasm_component_value *result, bool cancelled) {
    turbowasm_component_subtask *subtask = context;
    turbowasm_component_subtask_state next = subtask->waitable.state.subtask;
    turbowasm_status status;
    if (subtask->waitable.failure != TURBOWASM_OK) return subtask->waitable.failure;
    if (!turbowasm_component_subtask_resolve(&next, cancelled)) return TURBOWASM_TRAPPED;
    if (!cancelled && subtask->lower != NULL) {
        status = subtask->lower(subtask->context, result);
        if (status != TURBOWASM_OK) { subtask->waitable.failure = status; return status; }
        if (result != NULL && result->kind != 0) {
            subtask->waitable.failure = TURBOWASM_TRAPPED; return TURBOWASM_TRAPPED;
        }
    }
    subtask->waitable.state.subtask = next;
    if (subtask->callee->phase == TURBOWASM_COMPONENT_TASK_INITIAL) {
        subtask->callee->binding.prepare = NULL;
        subtask->callee->binding.prepare_context = NULL;
    }
    subtask->callee = NULL;
    return TURBOWASM_OK;
}

static void abandon_callee(void *context, turbowasm_status status) {
    turbowasm_component_subtask *subtask = context;
    if (subtask->waitable.failure == TURBOWASM_OK) subtask->waitable.failure = status;
    subtask->callee->binding.prepare = NULL;
    subtask->callee->binding.prepare_context = NULL;
    subtask->callee = NULL;
}

turbowasm_status turbowasm_component_subtask_create(turbowasm_component_subtask *subtask,
    turbowasm_component_resource_table *caller_table, turbowasm_component_task *callee,
    turbowasm_component_task_domain *callee_domain, const turbowasm_component_task_binding *binding,
    turbowasm_component_subtask_lower_fn lower, turbowasm_component_event_release_fn release,
    void *context) {
    turbowasm_component_task_binding adapted;
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (subtask == NULL || subtask->table != NULL || caller_table == NULL || caller_table->max_entries == 0 ||
        binding == NULL || binding->resolve != NULL || binding->abandon != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    type = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION) return TURBOWASM_TYPE_MISMATCH;
    if ((type->as.function.has_result && lower == NULL) ||
        (type->as.function.param_count != 0 && binding->prepare == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    adapted = *binding;
    adapted.prepare = prepare_callee; adapted.prepare_context = subtask;
    adapted.resolve = resolve_callee; adapted.abandon = abandon_callee; adapted.caller_context = subtask;
    status = turbowasm_component_task_create(callee, callee_domain, &adapted);
    if (status != TURBOWASM_OK) return status;
    memset(subtask, 0, sizeof(*subtask));
    subtask->table = caller_table; subtask->callee = callee;
    subtask->task_owner = callee;
    subtask->prepare = binding->prepare; subtask->prepare_context = binding->prepare_context;
    subtask->lower = lower; subtask->release = release; subtask->context = context;
    subtask->waitable.release_pending = release_loans; subtask->waitable.release_context = subtask;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_subtask_publish(turbowasm_component_subtask *subtask, uint32_t *out_word) {
    turbowasm_status status;
    if (subtask == NULL || subtask->table == NULL || out_word == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (subtask->published || subtask->waitable.delivering) return TURBOWASM_TRAPPED;
    if (subtask->waitable.failure != TURBOWASM_OK) return subtask->waitable.failure;
    if (resolved(subtask)) {
        turbowasm_component_subtask_phase phase;
        if (!turbowasm_component_subtask_take_event(&subtask->waitable.state.subtask, &phase)) return TURBOWASM_TRAPPED;
        subtask->published = true; subtask->waitable.delivering = true;
        status = release_loans(subtask);
        subtask->waitable.delivering = false;
        if (status != TURBOWASM_OK) return status;
        *out_word = TURBOWASM_COMPONENT_SUBTASK_RETURNED;
    } else {
        status = turbowasm_component_waitable_register(subtask->table, TURBOWASM_COMPONENT_HANDLE_SUBTASK, &subtask->waitable);
        if (status != TURBOWASM_OK) return status;
        subtask->published = true;
        subtask->waitable.state.subtask.pending_event = false;
        *out_word = ((uint32_t)subtask->waitable.state.subtask.phase) | (subtask->waitable.handle << 4u);
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_subtask_cancel_begin(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, const turbowasm_host_call *caller) {
    turbowasm_component_subtask *subtask = get_subtask(table, handle);
    turbowasm_component_task *callee;
    turbowasm_status status;
    if (subtask == NULL || subtask->waitable.state.subtask.resolve_delivered ||
        subtask->waitable.state.subtask.cancellation_requested) return TURBOWASM_TRAPPED;
    if (subtask->waitable.failure != TURBOWASM_OK) return subtask->waitable.failure;
    if (!resolved(subtask) && (subtask->callee == NULL || subtask->callee->resolving ||
        subtask->callee->destroying || subtask->callee->cancellation_requested ||
        subtask->callee->state >= TURBOWASM_EXECUTION_COMPLETED)) return TURBOWASM_TRAPPED;
    status = turbowasm_component_waitable_wait_begin(table, handle);
    if (status != TURBOWASM_OK) return status;
    if (resolved(subtask)) return TURBOWASM_OK;
    if (!turbowasm_component_subtask_request_cancel(&subtask->waitable.state.subtask)) status = TURBOWASM_TRAPPED;
    else {
        callee = subtask->callee;
        status = turbowasm_component_task_request_cancel(callee);
        /* A ready callback cancellation point executes eagerly. Other Core waits
         * retain their real completion owner; cancellation cannot complete I/O. */
        if (status == TURBOWASM_OK && subtask->callee != NULL && callee->between_callbacks &&
            callee->domain->active == NULL && callee->domain->exclusive == NULL) {
            status = caller != NULL ? turbowasm_component_task_resume_from_host(callee, caller)
                                   : turbowasm_component_task_resume(callee, NULL);
            if (status == TURBOWASM_YIELDED) status = TURBOWASM_OK;
        }
    }
    if (status != TURBOWASM_OK) (void)turbowasm_component_waitable_wait_cancel(table, handle);
    return status;
}

turbowasm_status turbowasm_component_subtask_cancel_ready(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, bool *out_ready) {
    turbowasm_component_subtask *subtask = get_subtask(table, handle);
    if (out_ready == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (subtask == NULL || !subtask->waitable.sync_waiter || subtask->waitable.delivering) return TURBOWASM_TRAPPED;
    *out_ready = resolved(subtask) || subtask->waitable.failure != TURBOWASM_OK;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_subtask_cancel_poll(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, uint32_t *out_phase) {
    turbowasm_component_event event;
    bool ready;
    turbowasm_status status;
    if (out_phase == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_subtask_cancel_ready(table, handle, &ready);
    if (status != TURBOWASM_OK) return status;
    if (!ready) return TURBOWASM_YIELDED;
    status = turbowasm_component_waitable_wait_end(table, handle, &event);
    if (status == TURBOWASM_OK) *out_phase = event.payload;
    return status;
}

turbowasm_status turbowasm_component_subtask_drop(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    if (get_subtask(table, handle) == NULL) return TURBOWASM_TRAPPED;
    return turbowasm_component_waitable_drop(table, handle);
}

turbowasm_status turbowasm_component_subtask_destroy(turbowasm_component_subtask *subtask) {
    turbowasm_status status = TURBOWASM_OK;
    if (subtask == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (subtask->table == NULL) return TURBOWASM_OK;
    if (subtask->callee != NULL || subtask->waitable.sync_waiter || subtask->waitable.delivering ||
        (subtask->waitable.failure == TURBOWASM_OK && !subtask->waitable.state.subtask.resolve_delivered))
        return TURBOWASM_TRAPPED;
    if (subtask->waitable.handle != 0) {
        void *object;
        status = turbowasm_component_handle_remove(subtask->table, subtask->waitable.handle,
            TURBOWASM_COMPONENT_HANDLE_SUBTASK, &object);
        if (status != TURBOWASM_OK) return status;
    }
    subtask->waitable.delivering = true;
    if (!subtask->released) status = release_loans(subtask);
    memset(subtask, 0, sizeof(*subtask));
    return status;
}
