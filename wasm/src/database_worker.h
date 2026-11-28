#ifndef TURBOWASM_DATABASE_WORKER_H
#define TURBOWASM_DATABASE_WORKER_H

#include "database_bridge.h"
#include <cflow/event.h>

typedef struct tw_db_worker { void *impl; } tw_db_worker;

typedef struct tw_db_worker_call {
    tw_db_work work;
    uint64_t deadline_ns;
    bool (*interrupted)(void *context);
    void *interrupt_context;
} tw_db_worker_call;

typedef struct tw_db_worker_result {
    tw_db_outcome outcome;
    uintptr_t native_resource;
    size_t size;
} tw_db_worker_result;

/* Executes on one dedicated CFlow serial lane. The provider's native resource
 * identities are created, used and drained on that same lane. The callback
 * borrows call and result storage until return, and must not enter the bridge,
 * retain guest pointers, or write beyond capacity. It owns the effect/rollback
 * evidence: interruption alone never establishes CANCELLED_NO_EFFECT. */
typedef tw_db_worker_result (*tw_db_worker_execute_fn)(void *context,
    const tw_db_worker_call *call,uint8_t *result,size_t capacity);

typedef struct tw_db_worker_config {
    uint32_t operations;
    size_t result_bytes, host_bytes;
    uint64_t timeout_ns;
    tw_db_worker_execute_fn execute;
    void *context;
} tw_db_worker_config;

/* Private composition. Initialize on the WASM owner before bridge init.
 * The bridge handle's address and provider context must remain stable until
 * all work and native resources drain. Use at least the bridge's operation
 * capacity and at most its result-byte limit. host_bytes bounds the worker's
 * own logical storage; CFlow queue/mailbox allocations have fixed capacities.
 * The two storage budgets must be accounted for separately by the host. */
tw_db_status tw_db_worker_init(tw_db_worker *worker,tw_db_bridge *bridge,
    const tw_db_worker_config *config,tw_db_dispatch *out_dispatch);

/* Borrowed, single-consumer CFlow waitable. Arm a nonblocking owner wake
 * and drive progress on that owner, never run WASM from a producer callback.
 * Completion may arrive before arm. Disarm and retain waker context until
 * worker destruction has joined the producer. */
cflow_waitable tw_db_worker_waitable(tw_db_worker *worker);

/* Transfer at most one drained worker receipt to the bridge. PENDING means
 * no ready record. On rejected completion preserve the slot and stop progress
 * with a sticky error; do not reinterpret an invalid native receipt or retry.
 * The host separately wakes/resumes the exact guest execution if needed. */
tw_db_status tw_db_worker_progress(tw_db_worker *worker);

/* After bridge shutdown reaches DONE and the waitable is disarmed, destroy
 * on the owner. BUSY preserves active worker storage. Successful destroy
 * joins the serial executor before freeing any mailbox/callback state.
 * The control-plane join is not a guest import. */
tw_db_status tw_db_worker_destroy(tw_db_worker *worker);
#endif
