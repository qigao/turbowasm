#ifndef TURBOWASM_COMPONENT_ASYNC_CALL_H
#define TURBOWASM_COMPONENT_ASYNC_CALL_H

#include "component_subtask.h"

/* Commit all staged destination handles atomically. Clear transferred owners'
 * cleanup obligations while retaining caller lender releases. Rollback releases
 * every staged handle even when returning an error. Neither hook suspends; the
 * memory.guest_realloc callback is the only suspending conversion boundary. */
typedef turbowasm_status (*turbowasm_component_async_commit_fn)(void *context,
    turbowasm_component_task *task, turbowasm_component_value *values, uint32_t count);
typedef turbowasm_status (*turbowasm_component_async_rollback_fn)(void *context,
    turbowasm_component_task *task);

typedef struct turbowasm_component_async_transaction {
    turbowasm_component_async_commit_fn commit;
    turbowasm_component_async_rollback_fn rollback;
    void *context;
} turbowasm_component_async_transaction;

typedef struct turbowasm_component_async_call_binding {
    const turbowasm_component_type_graph *caller_graph;
    turbowasm_component_type_id caller_function_type;
    turbowasm_component_task_domain *caller_domain, *callee_domain;
    turbowasm_component_canonical_memory caller_memory;
    turbowasm_component_task_binding callee;
    turbowasm_component_async_transaction parameters, result;
} turbowasm_component_async_call_binding;

/* One private invocation, stable and owner-thread-only. No input memory is read
 * on create: raw carriers are copied, while memories/codec contexts stay borrowed
 * through terminal delivery or failure teardown. Keep the frame alive while its
 * callee continues after task.return, even if its subtask handle was dropped. */
typedef struct turbowasm_component_async_call {
    turbowasm_component_async_call_binding binding;
    turbowasm_component_flat_signature caller_signature;
    turbowasm_component_task task;
    turbowasm_component_subtask subtask;
    turbowasm_component_value *arguments;
    turbowasm_value raw[TURBOWASM_COMPONENT_MAX_FLAT_ASYNC_PARAMS + 1u];
    uint64_t result_address;
    uint32_t argument_count;
    bool started;
} turbowasm_component_async_call;

/* Both function types must be validated async signatures with equal parameter
 * and result semantics. Callee prepare/resolve hooks must be empty. Resource
 * and endpoint destinations require transaction hooks; scalar-only paths do not.
 * A host-entry callee receives call.arguments as retained canonical values:
 * no callee memory codecs or parameter transaction are used. Its stable context
 * can refer to this call; moved arguments must be cleared. Unmoved values and
 * source loans live until terminal delivery or failure teardown, so the callback
 * must not access them after publishing its result/cancellation. Result lowering
 * still uses the caller's codecs, memory and result transaction.
 * Admission failures preserve the empty frame and all caller values. */
turbowasm_status turbowasm_component_async_call_create(turbowasm_component_async_call *call,
    const turbowasm_component_async_call_binding *binding,
    const turbowasm_value *arguments, size_t argument_count);
/* Run the initial quantum once and publish the canonical packed return word.
 * A Core caller shares its remaining fuel/interruption policy; options are only
 * for an external driver and must be NULL when caller is provided. No output is
 * published on failure. The owner must retain/tear down the failed frame. */
turbowasm_status turbowasm_component_async_call_start(turbowasm_component_async_call *call,
    const turbowasm_host_call *caller, const turbowasm_execution_options *options, uint32_t *out_word);
/* Subsequent progress uses task_resume on call.task and the ordinary subtask
 * waitable APIs. Teardown rejects a live waiter or undelivered successful result;
 * otherwise it first unwinds Core, then releases source values and reservations.
 * This private abort operation is not a public cooperative cancellation API. */
turbowasm_status turbowasm_component_async_call_destroy(turbowasm_component_async_call *call);

#endif
