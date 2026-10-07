#ifndef TURBOWASM_COMPONENT_TASK_H
#define TURBOWASM_COMPONENT_TASK_H

#include "component_canonical.h"
#include "component_waitable.h"
#include <turbowasm/execution.h>

typedef struct turbowasm_component_task turbowasm_component_task;

/* Owner-thread-only domain. Live caller-owned stable tasks are bounded by limit;
 * no worker or queue is created. The table and may_leave flag outlive the domain. */
typedef struct turbowasm_component_task_domain {
    turbowasm_component_resource_table *table;
    bool *may_leave;
    turbowasm_component_task *active, *exclusive;
    uint32_t count, limit, backpressure;
} turbowasm_component_task_domain;

/* Called exactly once after backpressure clears, with the task active. Fill
 * already-lowered Core arguments; the runner copies them before returning.
 * The adapter owns admission/rollback and outstanding borrowed-handle accounting.
 * It must not destroy its task/domain or retain the output array. */
typedef turbowasm_status (*turbowasm_component_task_prepare_fn)(
    void *context, turbowasm_component_task *task, turbowasm_value *arguments,
    size_t capacity, size_t *out_count);

typedef struct turbowasm_component_task_binding {
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id function_type;
    turbowasm_instance *instance;
    uint32_t function_index;
    turbowasm_instance *callback_instance; /* NULL selects stackful async */
    uint32_t callback_index;
    turbowasm_component_canonical_memory memory;
    turbowasm_component_task_prepare_fn prepare;
    void *prepare_context;
} turbowasm_component_task_binding;

typedef enum turbowasm_component_task_phase {
    TURBOWASM_COMPONENT_TASK_INITIAL = 0,
    TURBOWASM_COMPONENT_TASK_STARTED,
    TURBOWASM_COMPONENT_TASK_RETURNED,
    TURBOWASM_COMPONENT_TASK_CANCELLED
} turbowasm_component_task_phase;

struct turbowasm_component_task {
    turbowasm_component_task_domain *domain;
    turbowasm_component_task_binding binding;
    turbowasm_component_flat_signature signature;
    turbowasm_execution core;
    turbowasm_instance *core_instance;
    turbowasm_execution_state state;
    turbowasm_status status;
    turbowasm_trap trap;
    turbowasm_component_task_phase phase;
    turbowasm_component_value result;
    uint32_t borrowed_handles;
    turbowasm_component_resource_handle waiting_set;
    bool between_callbacks, cancellation_requested, cancellation_delivered;
    bool result_taken, destroying;
};

bool turbowasm_component_task_domain_init(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_table *table, bool *may_leave, uint32_t limit);
turbowasm_status turbowasm_component_task_domain_destroy(turbowasm_component_task_domain *domain);
/* The canonical count is bounded to 0..65535; overflow/underflow trap unchanged. */
turbowasm_status turbowasm_component_task_backpressure(turbowasm_component_task_domain *domain, bool increment);

/* Zero-initialized stable task, borrowed immutable binding/context/instances.
 * No argument preparation or guest execution occurs on create. Capacity and
 * signature failures preserve the empty task. */
turbowasm_status turbowasm_component_task_create(turbowasm_component_task *task,
    turbowasm_component_task_domain *domain, const turbowasm_component_task_binding *binding);
/* One entry/callback quantum, or continuation of a suspended Core call. WAIT
 * polls existing events and retains its pin while empty. Cancellation delivery
 * has priority over pending events. Core fuel/host waits are never replayed. */
turbowasm_status turbowasm_component_task_resume(turbowasm_component_task *task,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_task_request_cancel(turbowasm_component_task *task);
/* Called at a canonical cancellable wait point in the active task. */
bool turbowasm_component_task_deliver_cancel(turbowasm_component_task_domain *domain);
/* Guest task.cancel: requires the active task, delivered cancellation, no loans
 * and may_leave. Return remains permitted after cancellation was requested. */
turbowasm_status turbowasm_component_task_cancel(turbowasm_component_task_domain *domain);
/* Move an admitted canonical result; caller retains it on failure. Unit uses
 * NULL. This does not end the Core execution. */
turbowasm_status turbowasm_component_task_return(turbowasm_component_task_domain *domain,
    turbowasm_component_value *result);
/* Canonical task.return adapter: compare result type and memory/encoding with
 * the active lift before consuming any handles; lift Core parameters using the
 * shared canonical codecs. memory may be NULL for memory-free options. */
turbowasm_status turbowasm_component_task_return_flat(turbowasm_component_task_domain *domain,
    const turbowasm_component_type_graph *graph, bool has_result,
    turbowasm_component_type_ref result_type, const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_task_take_result(turbowasm_component_task *task,
    turbowasm_component_value *out);
/* Reject running/reentrant destruction. Unwind Core first, release set pin and
 * result, unregister the task. Primary cleanup error is returned after teardown. */
turbowasm_status turbowasm_component_task_destroy(turbowasm_component_task *task);

#endif
