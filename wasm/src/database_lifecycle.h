#ifndef TURBOWASM_DATABASE_LIFECYCLE_H
#define TURBOWASM_DATABASE_LIFECYCLE_H

#include "database_wire.h"

/* Private owner-thread core, independent of database drivers and guest
 * registration. No callback or scheduler is created here. Provider progress
 * must be delivered on this owner, never on a native completion thread. */
typedef struct tw_db_manager { void *impl; } tw_db_manager;

typedef struct tw_db_capacity {
    uint32_t connections, transactions, cursors, operations;
    size_t result_bytes, host_bytes;
} tw_db_capacity;

typedef enum tw_db_operation_state {
    TW_DB_ADMITTED = 1, TW_DB_RUNNING = 2,
    TW_DB_CANCEL_REQUESTED = 3, TW_DB_TERMINAL = 4
} tw_db_operation_state;

typedef struct tw_db_work {
    uint64_t operation;
    tw_db_request request;
    /* Private provider identity, never serialized to the guest. */
    uintptr_t native_owner;
    tw_db_kind native_kind;
} tw_db_work;

typedef struct tw_db_receipt {
    tw_db_outcome outcome;
    size_t size;
} tw_db_receipt;

/* Copies validated policy and preallocates all tables and byte storage.
 * host_bytes accounts for the logical allocations, including policy/table
 * metadata and every operation's full request and result reservation.
 * Zero resource capacities deny those resources. No provider effects. */
tw_db_status tw_db_manager_init(tw_db_manager *manager,
    const tw_db_policy *policy, const tw_db_capacity *capacity);
void tw_db_manager_stop(tw_db_manager *manager);
tw_db_status tw_db_manager_destroy(tw_db_manager *manager);

/* Input is a host-owned snapshot, not guest memory. Admits one operation and
 * reserves its prospective resource before start. IDs are non-reused in this
 * manager's lifetime; all handles are scoped to this manager. Caller-instance
 * authentication and owner lifetime are required by the later import bridge. */
tw_db_status tw_db_manager_submit(tw_db_manager *manager,
    uint64_t owner, uint32_t opcode, const uint8_t *bytes, size_t size,
    uint64_t *out_operation);
tw_db_status tw_db_manager_start(tw_db_manager *manager,
    uint64_t operation, tw_db_work *out);
tw_db_status tw_db_manager_poll(tw_db_manager *manager,
    uint64_t operation, tw_db_operation_state *out);

/* The returned work borrows operation storage until terminal completion.
 * work.operation is the native dispatch identity (normally the guest token;
 * discard uses a distinct one). CLOSE consumes an owner only after drain;
 * closing a transaction rolls back known-active work, never replays commit.
 * On a failed close the provider must retain a valid cleanup identity.
 * A valid terminal completion proves the provider no longer borrows input or
 * result buffers. Successful OPEN/BEGIN/QUERY transfer exactly one new native
 * resource; successful COMMIT/ROLLBACK/CLOSE consume their native owner.
 * Other completions must pass native_resource=0 and drain any unpublished
 * native resource. UNKNOWN quarantines the owner and its ancestors. Invalid
 * completions publish nothing; the provider still owns its cleanup obligation. */
tw_db_status tw_db_manager_complete(tw_db_manager *manager,
    uint64_t operation, tw_db_outcome outcome, uintptr_t native_resource,
    const uint8_t *bytes, size_t size);

/* Before start, cancel completes with CANCELLED_NO_EFFECT. After start, signal
 * the provider exactly once when out_notify is true; do not imply rollback.
 * The owner and buffers remain pinned until a valid terminal completion. */
tw_db_status tw_db_manager_cancel(tw_db_manager *manager,
    uint64_t operation, bool *out_notify);
tw_db_status tw_db_manager_receipt(tw_db_manager *manager,
    uint64_t operation, tw_db_receipt *out);
tw_db_status tw_db_manager_read(tw_db_manager *manager,
    uint64_t operation, size_t offset, uint8_t *dst, size_t capacity,
    size_t *out_copied);
tw_db_status tw_db_manager_take(tw_db_manager *manager,
    uint64_t operation, uint64_t *out_resource);
/* Discard a terminal operation's untaken resource by dispatching a separate
 * CLOSE with a fresh native identity in the same reserved operation slot.
 * Once begun, the original result is relinquished and take is forbidden.
 * Receipt/poll describe cleanup. Pending discard cannot be cancelled;
 * failed/unknown discard retains the native identity for explicit host recovery.
 * No native operation is issued here: the owner dispatches the returned work. */
tw_db_status tw_db_manager_begin_discard(tw_db_manager *manager,
    uint64_t operation, tw_db_work *out);
/* Host-only drain proof for an untaken resource, never accessible to the
 * guest. In-flight discard cannot be disposed. */
tw_db_status tw_db_manager_untaken(tw_db_manager *manager,
    uint64_t operation, uintptr_t *out_native);
tw_db_status tw_db_manager_disposed(tw_db_manager *manager, uint64_t operation);
tw_db_status tw_db_manager_drop(tw_db_manager *manager, uint64_t operation);
/* Guest drop admission. If disposal is newly admitted, out_work receives
 * a nonzero native dispatch identity and PENDING; otherwise its identity is
 * zero. A repeat drop observes pending cleanup or releases its terminal
 * success. Failure is retained as a receipt, not automatically retried. */
tw_db_status tw_db_manager_request_drop(tw_db_manager *manager,
    uint64_t operation,tw_db_work *out_work);

/* One bounded action per shutdown step, after stop and guest detach.
 * WAIT requires owner-driven provider progress; BLOCKED is sticky until
 * explicit host recovery/teardown, never an automatic retry. RETIRED reports
 * the receipt that is discarded; callers must record uncertain outcomes.
 * DISPATCH and CANCEL are delivered once on the owner. */
typedef enum tw_db_shutdown_kind {
    TW_DB_SHUTDOWN_DONE=1, TW_DB_SHUTDOWN_WAIT, TW_DB_SHUTDOWN_RETIRED,
    TW_DB_SHUTDOWN_CANCEL, TW_DB_SHUTDOWN_DISPATCH, TW_DB_SHUTDOWN_BLOCKED
} tw_db_shutdown_kind;
typedef struct tw_db_shutdown_action {
    tw_db_shutdown_kind kind;
    uint64_t operation;
    tw_db_receipt receipt;
    tw_db_work work;
} tw_db_shutdown_action;
tw_db_status tw_db_manager_shutdown_next(tw_db_manager *manager,tw_db_shutdown_action *out);
/* Trusted host only: confirms the native owner of a BLOCKED automatic CLOSE
 * has been explicitly drained outside this manager. Never invokes a provider
 * or retries the failed close. The failed receipt remains for retirement. */
tw_db_status tw_db_manager_confirm_shutdown_close(tw_db_manager *manager,uint64_t operation);

#endif
