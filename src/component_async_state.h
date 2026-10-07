#ifndef TURBOWASM_COMPONENT_ASYNC_STATE_H
#define TURBOWASM_COMPONENT_ASYNC_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Private Canonical ABI notification states, not a scheduler or handle table.
 * Embed in the owning task/endpoint. The owner enforces thread affinity, handle
 * generation, waitable membership and memory/resource lifetime. All functions
 * are allocation-free; false leaves state and output unchanged. */
typedef enum turbowasm_component_subtask_phase {
    TURBOWASM_COMPONENT_SUBTASK_STARTING = 0,
    TURBOWASM_COMPONENT_SUBTASK_STARTED = 1,
    TURBOWASM_COMPONENT_SUBTASK_RETURNED = 2,
    TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_STARTED = 3,
    TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED = 4
} turbowasm_component_subtask_phase;

/* Zero initialization starts a subtask. It cannot be dropped until its terminal
 * event has been delivered and the enclosing owner has released its loans. */
typedef struct turbowasm_component_subtask_state {
    turbowasm_component_subtask_phase phase;
    bool cancellation_requested;
    bool pending_event;
    bool resolve_delivered;
} turbowasm_component_subtask_state;

bool turbowasm_component_subtask_start(turbowasm_component_subtask_state *state);
bool turbowasm_component_subtask_request_cancel(turbowasm_component_subtask_state *state);
bool turbowasm_component_subtask_resolve(turbowasm_component_subtask_state *state,
    bool cancelled);
/* Return the latest phase, consuming one notification. For a terminal event the
 * owner must release loans before guest execution resumes. No loan is released
 * by a STARTED event. */
bool turbowasm_component_subtask_take_event(turbowasm_component_subtask_state *state,
    turbowasm_component_subtask_phase *out_phase);

typedef enum turbowasm_component_endpoint_phase {
    TURBOWASM_COMPONENT_ENDPOINT_IDLE = 0,
    TURBOWASM_COMPONENT_ENDPOINT_COPYING,
    TURBOWASM_COMPONENT_ENDPOINT_CANCELLING,
    TURBOWASM_COMPONENT_ENDPOINT_DONE
} turbowasm_component_endpoint_phase;

enum {
    TURBOWASM_COMPONENT_COPY_COMPLETED = 0u,
    TURBOWASM_COMPONENT_COPY_DROPPED = 1u,
    TURBOWASM_COMPONENT_COPY_CANCELLED = 2u,
    TURBOWASM_COMPONENT_COPY_MAX_LENGTH = 0x0fffffffu
};

/* Zero initialization is an idle stream end. Set future=true once, before
 * first use, for a future end. progress counts elements, not bytes. */
typedef struct turbowasm_component_endpoint_state {
    turbowasm_component_endpoint_phase phase;
    uint32_t progress;
    bool future;
    bool peer_dropped;
    bool pending_event;
} turbowasm_component_endpoint_state;

bool turbowasm_component_endpoint_begin_copy(turbowasm_component_endpoint_state *state);
/* Coalesce cumulative progress before delivery. The transfer owner has already
 * committed this many elements. Futures can publish only zero or one element. */
bool turbowasm_component_endpoint_notify(turbowasm_component_endpoint_state *state,
    uint32_t progress);
/* defer_ack is only for a host operation still owning its buffer. It must later
 * call notify when cancellation/completion is acknowledged. A pending progress
 * event is retained even when cancellation is requested. */
bool turbowasm_component_endpoint_request_cancel(turbowasm_component_endpoint_state *state,
    bool defer_ack);
bool turbowasm_component_endpoint_peer_dropped(turbowasm_component_endpoint_state *state);
/* Compute result precedence at delivery. Streams pack progress in bits 4..31;
 * futures return only the result code. On success the owner releases the pending
 * buffer before guest resumption. This primitive never accesses the buffer. */
bool turbowasm_component_endpoint_take_event(turbowasm_component_endpoint_state *state,
    uint32_t *out_result);

#endif
