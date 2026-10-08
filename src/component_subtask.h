#ifndef TURBOWASM_COMPONENT_SUBTASK_H
#define TURBOWASM_COMPONENT_SUBTASK_H

#include "component_task.h"

/* Lower/move the callee result into the caller's destination. Called inside
 * task.return before resolution. On success consume/clear result; on failure
 * leave any unconsumed value for task.return's normal cleanup. NULL means unit.
 * The result destination stays borrowed until this hook completes. */
typedef turbowasm_status (*turbowasm_component_subtask_lower_fn)(
    void *context, turbowasm_component_value *result);

/* Private, owner-thread-only stable storage. The callee task is separately owned
 * and can continue after resolution. The table/adapter context outlive delivery
 * or failed-owner teardown. No allocation except bounded handle publication. */
typedef struct turbowasm_component_subtask {
    turbowasm_component_waitable waitable;
    turbowasm_component_resource_table *table;
    turbowasm_component_task *callee;
    turbowasm_component_task *task_owner, *parent;
    struct turbowasm_component_subtask *next_child;
    turbowasm_component_task_prepare_fn prepare;
    void *prepare_context;
    turbowasm_component_subtask_lower_fn lower;
    turbowasm_component_event_release_fn release;
    void *context;
    bool published, released;
} turbowasm_component_subtask;

/* Caller-lifetime dependencies; exec retains frame/progress ownership. */
void turbowasm_component_subtask_attach(turbowasm_component_subtask *subtask, turbowasm_component_task *parent);
void turbowasm_component_subtask_detach_children(turbowasm_component_task *parent);
turbowasm_status turbowasm_component_subtask_abort_children(turbowasm_component_task *parent);

/* Creates the callee task and installs the caller boundary atomically. Binding
 * must not already have caller hooks. Empty subtask/task remain empty on failure.
 * lower is required for payload results. release runs once on delivery/teardown
 * and must release all loans even when reporting a cleanup error. */
turbowasm_status turbowasm_component_subtask_create(turbowasm_component_subtask *subtask,
    turbowasm_component_resource_table *caller_table, turbowasm_component_task *callee,
    turbowasm_component_task_domain *callee_domain, const turbowasm_component_task_binding *binding,
    turbowasm_component_subtask_lower_fn lower, turbowasm_component_event_release_fn release,
    void *context);
/* After the initial callee quantum: eager resolution returns 2 without a handle;
 * otherwise register and return phase | (handle << 4). No duplicate STARTED
 * event is queued for a phase already returned in this word. */
turbowasm_status turbowasm_component_subtask_publish(turbowasm_component_subtask *subtask, uint32_t *out_word);

/* Cancellation begin pins the subtask before invoking the callee. Poll waits
 * only for a terminal phase (never consumes STARTED); YIELDED retains the pin.
 * Use waitable_wait_cancel on async BLOCKED or host-wait unwind to release it.
 * Pass the active Core caller for an eager callback quantum to share its budget;
 * NULL is reserved for an external driver choosing unlimited fuel/no interrupt. */
turbowasm_status turbowasm_component_subtask_cancel_begin(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, const turbowasm_host_call *caller);
turbowasm_status turbowasm_component_subtask_cancel_poll(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, uint32_t *out_phase);
turbowasm_status turbowasm_component_subtask_cancel_ready(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, bool *out_ready);
turbowasm_status turbowasm_component_subtask_drop(turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle);
/* Reject attached callees, live waits, or an undelivered successful resolution.
 * Destroy/unwind a failed callee first, then destroy its subtask to release loans.
 * Successful drop only unregisters; this function clears the caller's storage. */
turbowasm_status turbowasm_component_subtask_destroy(turbowasm_component_subtask *subtask);

#endif
