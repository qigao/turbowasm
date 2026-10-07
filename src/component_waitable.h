#ifndef TURBOWASM_COMPONENT_WAITABLE_H
#define TURBOWASM_COMPONENT_WAITABLE_H

#include "component_async_state.h"
#include "component_resource.h"

typedef enum turbowasm_component_event_code {
    TURBOWASM_COMPONENT_EVENT_NONE = 0,
    TURBOWASM_COMPONENT_EVENT_SUBTASK = 1,
    TURBOWASM_COMPONENT_EVENT_STREAM_READ = 2,
    TURBOWASM_COMPONENT_EVENT_STREAM_WRITE = 3,
    TURBOWASM_COMPONENT_EVENT_FUTURE_READ = 4,
    TURBOWASM_COMPONENT_EVENT_FUTURE_WRITE = 5,
    TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED = 6
} turbowasm_component_event_code;

typedef struct turbowasm_component_event {
    turbowasm_component_event_code code;
    turbowasm_component_resource_handle handle;
    uint32_t payload;
} turbowasm_component_event;

/* Event cleanup is nonblocking and must not run guest code. It releases the
 * endpoint buffer borrow, or all loans for a terminal subtask event, returning
 * the first error. It may grow the handle table. The
 * current waitable cannot join/drop/wait/take recursively during this callback.
 * Failure consumes the terminal notification and leaves event output untouched;
 * the owner must propagate it as a trap rather than replay cleanup. */
typedef turbowasm_status (*turbowasm_component_event_release_fn)(void *context);

typedef struct turbowasm_component_waitable {
    turbowasm_component_resource_table *table;
    turbowasm_component_resource_handle handle;
    turbowasm_component_resource_handle set_handle;
    bool sync_waiter;
    /* Exclusive callback guard during event cleanup and endpoint conversion. */
    bool delivering;
    union {
        turbowasm_component_subtask_state subtask;
        turbowasm_component_endpoint_state endpoint;
    } state;
    turbowasm_component_event_release_fn release_pending;
    void *release_context;
} turbowasm_component_waitable;

typedef struct turbowasm_component_waitable_set {
    turbowasm_component_resource_table *table;
    turbowasm_component_resource_handle handle;
    uint32_t wait_count;
    uint32_t next_slot;
} turbowasm_component_waitable_set;

/* Objects are zero-initialized, caller-owned and stable until drop succeeds.
 * The table owns only handle slots. Drop all registrations before table/object
 * destruction. Registration failure transfers nothing. No operation allocates
 * except registration, which uses the existing bounded table growth. */
turbowasm_status turbowasm_component_waitable_register(
    turbowasm_component_resource_table *table,
    turbowasm_component_handle_kind kind, turbowasm_component_waitable *waitable);
turbowasm_status turbowasm_component_waitable_set_register(
    turbowasm_component_resource_table *table, turbowasm_component_waitable_set *set);
turbowasm_status turbowasm_component_waitable_join(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_resource_handle set_handle);
turbowasm_status turbowasm_component_waitable_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle);
turbowasm_status turbowasm_component_waitable_set_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle);

/* Poll returns OK and a NONE/0/0 tuple if the set has no pending event.
 * Direct take returns YIELDED without changing output if no event is pending.
 * Slot scanning rotates after delivery to avoid a perpetually ready low slot
 * starving other ready members. Invalid/stale/wrong-kind handles trap. */
turbowasm_status turbowasm_component_waitable_set_poll(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle set_handle,
    turbowasm_component_event *out_event);
/* Non-consuming readiness query for the Core continuation driver. */
turbowasm_status turbowasm_component_waitable_set_ready(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle set_handle,
    bool *out_ready);
turbowasm_status turbowasm_component_waitable_take(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, turbowasm_component_event *out_event);

/* A suspended set wait pins its set until release, including cancellation.
 * An individual synchronous wait is exclusive and may only start outside a set.
 * wait_end keeps the pin on YIELDED; wait_cancel releases it without stealing
 * an event. The scheduler/owning coroutine performs actual suspension. */
turbowasm_status turbowasm_component_waitable_set_wait_acquire(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle);
turbowasm_status turbowasm_component_waitable_set_wait_release(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle);
turbowasm_status turbowasm_component_waitable_wait_begin(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle);
turbowasm_status turbowasm_component_waitable_wait_end(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    turbowasm_component_event *out_event);
turbowasm_status turbowasm_component_waitable_wait_cancel(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle);

#endif
