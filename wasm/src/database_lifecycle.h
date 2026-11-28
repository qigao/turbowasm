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
/* Drop cannot silently destroy a native result. The host can inspect and drain
 * an untaken resource on its native owner, then confirm its disposal. Until
 * that proof, drop/destroy return BUSY. No guest call may confirm disposal. */
tw_db_status tw_db_manager_untaken(tw_db_manager *manager,
    uint64_t operation, uintptr_t *out_native);
tw_db_status tw_db_manager_disposed(tw_db_manager *manager, uint64_t operation);
tw_db_status tw_db_manager_drop(tw_db_manager *manager, uint64_t operation);

#endif
