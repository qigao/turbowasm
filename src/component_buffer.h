#ifndef TURBOWASM_COMPONENT_BUFFER_H
#define TURBOWASM_COMPONENT_BUFFER_H

#include "component_canonical.h"

typedef enum turbowasm_component_buffer_kind {
    TURBOWASM_COMPONENT_BUFFER_HOST = 0,
    TURBOWASM_COMPONENT_BUFFER_GUEST
} turbowasm_component_buffer_kind;

/* A successful commit transfers every lowered ownership obligation, leaving
 * the old values safe to destroy. Failure leaves reservations rollbackable.
 * Rollback releases every reservation, returning the first error. These hooks
 * share the canonical callbacks' context/lifetime and must not publish partial
 * results. They are required when lowering resources or endpoint values. */
typedef turbowasm_status (*turbowasm_component_buffer_commit_fn)(void *context,
    turbowasm_component_value *values, uint32_t count);
typedef turbowasm_status (*turbowasm_component_buffer_rollback_fn)(void *context);
typedef turbowasm_status (*turbowasm_component_buffer_release_fn)(void *context);

/* Caller-owned stable storage, lent from endpoint submit through event/error
 * delivery. Host values are uniquely owned and read destinations start empty.
 * Guest memory uses checked offsets; instances, graphs and callback contexts
 * outlive the borrow. Unit/zero-length operations ignore values/guest options.
 * Do not mutate leased host cells or this descriptor. */
typedef struct turbowasm_component_buffer {
    turbowasm_component_value *values;
    uint32_t length;
    uint32_t progress;
    bool leased;
    turbowasm_component_buffer_kind kind;
    struct {
        const turbowasm_component_type_graph *graph;
        turbowasm_component_type_ref type;
        turbowasm_component_canonical_memory memory;
        uint64_t address;
        turbowasm_component_buffer_commit_fn commit;
        turbowasm_component_buffer_rollback_fn rollback;
        /* Optional owned context for submit_guest only. Successful admission
         * transfers this obligation to the endpoint; failed admission leaves it
         * with the caller. Runs once on delivery, never during conversion;
         * cleanup must not suspend. */
        turbowasm_component_buffer_release_fn release;
        void *context;
    } guest;
} turbowasm_component_buffer;

/* Called before admission. Readable endpoint means a destination buffer.
 * Returns before side effects on malformed ranges/types or missing callbacks. */
turbowasm_status turbowasm_component_buffer_validate(const turbowasm_component_buffer *buffer,
    const turbowasm_component_type_graph *graph, bool has_payload,
    turbowasm_component_type_ref payload, bool destination);

/* Copy one positive, prevalidated rendezvous batch with both endpoints guarded.
 * Eagerly lifts all guest source elements before writing any destination. Updates
 * each buffer's progress after that side's read/write succeeds. A trap can
 * consume guest source owners; callers must terminate both endpoint operations.
 * Lower failure rolls back destination ownership before temporary cleanup. */
turbowasm_status turbowasm_component_buffer_copy(turbowasm_component_buffer *source,
    turbowasm_component_buffer *destination, bool has_payload, uint32_t count);

#endif
