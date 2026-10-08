#ifndef TURBOWASM_COMPONENT_TASK_H
#define TURBOWASM_COMPONENT_TASK_H

#include "component_canonical.h"
#include "component_waitable.h"
#include <turbowasm/execution.h>

typedef struct turbowasm_component_task turbowasm_component_task;
enum { TURBOWASM_COMPONENT_TASK_MAX_DEPENDENCY_DEPTH = 256u };

/* Owner-thread-only domain. Live caller-owned stable tasks are bounded by limit;
 * no worker or queue is created. The table and may_leave flag outlive the domain. */
typedef struct turbowasm_component_task_domain {
    turbowasm_component_resource_table *table;
    bool *may_leave;
    turbowasm_component_task *active, *exclusive;
    /* Auxiliary calls retain context isolation across suspension. */
    turbowasm_component_task *auxiliary;
    struct turbowasm_component_task_owned_set *sets;
    struct turbowasm_component_task_owned_pair *pairs;
    uint32_t pair_count;
    uint32_t count, limit, backpressure;
    uint32_t synchronous_depth;
} turbowasm_component_task_domain;

/* Called exactly once after backpressure clears, on the retained Core coroutine
 * with the task active. Fill already-lowered Core arguments; the runner copies
 * them after success. Guest realloc may use that execution's control and suspend;
 * all temporary owners must survive and clean up when the callback unwinds.
 * Set task->trap when returning a specific Core trap from argument conversion.
 * The adapter owns admission/rollback and outstanding borrowed-handle accounting.
 * It must not destroy its task/domain or retain the output array. */
typedef turbowasm_status (*turbowasm_component_task_prepare_fn)(
    void *context, turbowasm_component_task *task, turbowasm_value *arguments,
    size_t capacity, size_t *out_count);

/* Private caller boundary. Resolve moves result on success (NULL for unit or
 * cancellation), before the task commits its phase. Failure leaves any remaining
 * value with the caller. Abandon is nonblocking, runs no guest code, and detaches
 * an unresolved caller on failure/unwind. Supply both hooks or neither. Context
 * stays stable until exactly one successful resolve or abandon; no recursive
 * task return/cancel/destruction is permitted within these hooks. */
typedef turbowasm_status (*turbowasm_component_task_resolve_fn)(
    void *context, turbowasm_component_value *result, bool cancelled);
typedef void (*turbowasm_component_task_abandon_fn)(void *context, turbowasm_status status);

/* Internal stackful host task. Runs once after prepare, with this task active.
 * It may use host_wait and nested Core calls, then explicitly return/cancel the
 * task. Context remains borrowed until execution ends; no raw YIELDED return. */
typedef turbowasm_status (*turbowasm_component_task_host_entry_fn)(void *context,
    turbowasm_component_task *task, turbowasm_host_call *call);

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
    turbowasm_component_task_resolve_fn resolve;
    turbowasm_component_task_abandon_fn abandon;
    void *caller_context;
    turbowasm_component_task_host_entry_fn host_entry;
    void *host_context;
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
    struct turbowasm_component_subtask *children;
    uint32_t dependency_depth;
    turbowasm_component_resource_handle waiting_set;
    /* Builtin host-wait continuation, separate from callback WAIT. */
    enum { TURBOWASM_COMPONENT_TASK_WAIT_NONE, TURBOWASM_COMPONENT_TASK_WAIT_YIELD,
           TURBOWASM_COMPONENT_TASK_WAIT_SET, TURBOWASM_COMPONENT_TASK_WAIT_SUBTASK,
           TURBOWASM_COMPONENT_TASK_WAIT_ENDPOINT } builtin_wait;
    turbowasm_component_resource_handle builtin_wait_set;
    uint64_t context_storage[2];
    bool between_callbacks, cancellation_requested, cancellation_delivered;
    bool result_taken, destroying, resolving;
};

bool turbowasm_component_task_domain_init(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_table *table, bool *may_leave, uint32_t limit);
turbowasm_status turbowasm_component_task_domain_destroy(turbowasm_component_task_domain *domain);
/* The canonical count is bounded to 0..65535; overflow/underflow trap unchanged. */
turbowasm_status turbowasm_component_task_backpressure(turbowasm_component_task_domain *domain, bool increment);
/* Guest-created set storage belongs to the domain and shares table quota.
 * Drop also accepts externally owned registered sets, without freeing them.
 * Domain destruction requires tasks gone and owned sets empty/unpinned. */
turbowasm_status turbowasm_component_task_set_new(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle *out);
turbowasm_status turbowasm_component_task_set_drop(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle handle);

/* Validate the immutable lift/callback ABI without requiring invocation hooks.
 * Used by instantiation before any argument owner/task exists. */
turbowasm_status turbowasm_component_task_binding_validate(
    const turbowasm_component_task_binding *binding, turbowasm_component_flat_signature *out);
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
/* Nonblocking initial/callback quantum between guest calls, sharing the active host
 * caller's remaining fuel and interrupt policy (unlimited for an uncontrolled
 * one-shot invocation). A previously suspended Core
 * call must be resumed by its driver; this entry rejects such continuations. */
turbowasm_status turbowasm_component_task_resume_from_host(turbowasm_component_task *task,
    const turbowasm_host_call *caller);
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
/* Reject running/reentrant destruction, except an already exited empty sibling
 * that requires no cleanup callbacks. Unwind Core first, release set pin and
 * result, unregister the task. Borrowing tasks first abort dependent children;
 * blocked cleanup retains the task and caller loans for a later retry. Primary
 * cleanup error is returned after successful teardown. */
turbowasm_status turbowasm_component_task_destroy(turbowasm_component_task *task);

#endif
