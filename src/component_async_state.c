#include "component_async_state.h"

#include <stddef.h>

static bool subtask_live(const turbowasm_component_subtask_state *state) {
    return state != NULL &&
        (state->phase == TURBOWASM_COMPONENT_SUBTASK_STARTING ||
         state->phase == TURBOWASM_COMPONENT_SUBTASK_STARTED);
}

bool turbowasm_component_subtask_start(turbowasm_component_subtask_state *state) {
    if (!subtask_live(state) || state->phase != TURBOWASM_COMPONENT_SUBTASK_STARTING)
        return false;
    state->phase = TURBOWASM_COMPONENT_SUBTASK_STARTED;
    state->pending_event = true;
    return true;
}

bool turbowasm_component_subtask_request_cancel(turbowasm_component_subtask_state *state) {
    if (state == NULL || state->resolve_delivered || state->cancellation_requested)
        return false;
    /* Resolving can race with cancellation admission. Preserve an undelivered
     * terminal event without sending a redundant cancellation to the callee. */
    if (subtask_live(state))
        state->cancellation_requested = true;
    return state->phase >= TURBOWASM_COMPONENT_SUBTASK_STARTING &&
           state->phase <= TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED;
}

bool turbowasm_component_subtask_resolve(turbowasm_component_subtask_state *state,
    bool cancelled) {
    if (!subtask_live(state) ||
        (cancelled && !state->cancellation_requested) ||
        (!cancelled && state->phase != TURBOWASM_COMPONENT_SUBTASK_STARTED))
        return false;
    if (cancelled)
        state->phase = state->phase == TURBOWASM_COMPONENT_SUBTASK_STARTING
            ? TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_STARTED
            : TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED;
    else
        state->phase = TURBOWASM_COMPONENT_SUBTASK_RETURNED;
    state->pending_event = true;
    return true;
}

bool turbowasm_component_subtask_take_event(turbowasm_component_subtask_state *state,
    turbowasm_component_subtask_phase *out_phase) {
    if (state == NULL || out_phase == NULL || !state->pending_event ||
        state->resolve_delivered || state->phase < TURBOWASM_COMPONENT_SUBTASK_STARTED ||
        state->phase > TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED)
        return false;
    state->pending_event = false;
    if (!subtask_live(state))
        state->resolve_delivered = true;
    *out_phase = state->phase;
    return true;
}

bool turbowasm_component_endpoint_begin_copy(turbowasm_component_endpoint_state *state) {
    if (state == NULL || state->phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE ||
        (state->pending_event && !state->peer_dropped))
        return false;
    state->phase = TURBOWASM_COMPONENT_ENDPOINT_COPYING;
    state->progress = 0u;
    return true;
}

bool turbowasm_component_endpoint_notify(turbowasm_component_endpoint_state *state,
    uint32_t progress) {
    if (state == NULL ||
        (state->phase != TURBOWASM_COMPONENT_ENDPOINT_COPYING &&
         state->phase != TURBOWASM_COMPONENT_ENDPOINT_CANCELLING &&
         !(state->phase == TURBOWASM_COMPONENT_ENDPOINT_IDLE && state->peer_dropped)) ||
        progress > TURBOWASM_COMPONENT_COPY_MAX_LENGTH ||
        (state->phase == TURBOWASM_COMPONENT_ENDPOINT_IDLE && progress != 0u) ||
        (state->future && (progress > 1u ||
            (progress == 0u && !state->peer_dropped &&
             state->phase != TURBOWASM_COMPONENT_ENDPOINT_CANCELLING))) ||
        (state->pending_event && progress < state->progress))
        return false;
    state->progress = progress;
    state->pending_event = true;
    return true;
}

bool turbowasm_component_endpoint_request_cancel(turbowasm_component_endpoint_state *state,
    bool defer_ack) {
    if (state == NULL || state->phase != TURBOWASM_COMPONENT_ENDPOINT_COPYING)
        return false;
    state->phase = TURBOWASM_COMPONENT_ENDPOINT_CANCELLING;
    if (!state->pending_event && !defer_ack) {
        state->progress = 0u;
        state->pending_event = true;
    }
    return true;
}

bool turbowasm_component_endpoint_peer_dropped(turbowasm_component_endpoint_state *state) {
    if (state == NULL || state->peer_dropped ||
        state->phase > TURBOWASM_COMPONENT_ENDPOINT_DONE)
        return false;
    state->peer_dropped = true;
    if (state->phase != TURBOWASM_COMPONENT_ENDPOINT_DONE && !state->pending_event) {
        state->progress = 0u;
        state->pending_event = true;
    }
    return true;
}

bool turbowasm_component_endpoint_take_event(turbowasm_component_endpoint_state *state,
    uint32_t *out_result) {
    uint32_t result;
    turbowasm_component_endpoint_phase next_phase;
    if (state == NULL || out_result == NULL || !state->pending_event ||
        state->phase == TURBOWASM_COMPONENT_ENDPOINT_DONE)
        return false;
    if (state->future && state->progress == 1u) {
        result = TURBOWASM_COMPONENT_COPY_COMPLETED;
        next_phase = TURBOWASM_COMPONENT_ENDPOINT_DONE;
    } else if (state->peer_dropped) {
        result = TURBOWASM_COMPONENT_COPY_DROPPED;
        next_phase = TURBOWASM_COMPONENT_ENDPOINT_DONE;
    } else if (state->phase == TURBOWASM_COMPONENT_ENDPOINT_CANCELLING) {
        result = TURBOWASM_COMPONENT_COPY_CANCELLED;
        next_phase = TURBOWASM_COMPONENT_ENDPOINT_IDLE;
    } else if (!state->future && state->phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING) {
        result = TURBOWASM_COMPONENT_COPY_COMPLETED;
        next_phase = TURBOWASM_COMPONENT_ENDPOINT_IDLE;
    } else {
        return false;
    }
    if (!state->future)
        result |= state->progress << 4u;
    state->phase = next_phase;
    state->pending_event = false;
    *out_result = result;
    return true;
}
