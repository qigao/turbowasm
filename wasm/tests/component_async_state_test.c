#include "component_async_state.h"
#include <tinytest.h>

spec("Component async notification transitions") {
    it("coalesces start and return into one latest-state event") {
        turbowasm_component_subtask_state state = {0};
        turbowasm_component_subtask_phase event = TURBOWASM_COMPONENT_SUBTASK_STARTING;
        check_false(turbowasm_component_subtask_take_event(&state, &event));
        check_true(turbowasm_component_subtask_start(&state));
        check_false(turbowasm_component_subtask_start(&state));
        check_true(turbowasm_component_subtask_resolve(&state, false));
        check_false(state.resolve_delivered);
        check_true(turbowasm_component_subtask_take_event(&state, &event));
        check_equal(event, TURBOWASM_COMPONENT_SUBTASK_RETURNED);
        check_true(state.resolve_delivered);
        check_false(turbowasm_component_subtask_take_event(&state, &event));
        check_false(turbowasm_component_subtask_resolve(&state, false));
        check_false(turbowasm_component_subtask_request_cancel(&state));
    }

    it("marks resolution delivered only when the terminal event is consumed") {
        turbowasm_component_subtask_state state = {0};
        turbowasm_component_subtask_phase event;
        check_true(turbowasm_component_subtask_start(&state));
        check_true(turbowasm_component_subtask_take_event(&state, &event));
        check_equal(event, TURBOWASM_COMPONENT_SUBTASK_STARTED);
        check_false(state.resolve_delivered);
        check_true(turbowasm_component_subtask_request_cancel(&state));
        check_false(turbowasm_component_subtask_request_cancel(&state));
        check_false(state.pending_event);
        check_true(turbowasm_component_subtask_resolve(&state, true));
        check_false(state.resolve_delivered);
        check_true(turbowasm_component_subtask_take_event(&state, &event));
        check_equal(event, TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED);
        check_true(state.resolve_delivered);
    }

    it("distinguishes cancellation before admission from cancellation after start") {
        unsigned started;
        for (started = 0u; started < 2u; ++started) {
            turbowasm_component_subtask_state state = {0};
            turbowasm_component_subtask_phase event;
            check_false(turbowasm_component_subtask_resolve(&state, false));
            check_false(turbowasm_component_subtask_resolve(&state, true));
            check_true(turbowasm_component_subtask_request_cancel(&state));
            if (started)
                check_true(turbowasm_component_subtask_start(&state));
            check_true(turbowasm_component_subtask_resolve(&state, true));
            check_false(turbowasm_component_subtask_resolve(&state, true));
            check_true(turbowasm_component_subtask_take_event(&state, &event));
            check_equal(event, started ? TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_RETURNED
                : TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_STARTED);
        }
    }

    it("preserves a returned result when cancellation races with return") {
        unsigned cancel_first;
        for (cancel_first = 0u; cancel_first < 2u; ++cancel_first) {
            turbowasm_component_subtask_state state = {0};
            turbowasm_component_subtask_phase event;
            check_true(turbowasm_component_subtask_start(&state));
            if (cancel_first)
                check_true(turbowasm_component_subtask_request_cancel(&state));
            check_true(turbowasm_component_subtask_resolve(&state, false));
            if (!cancel_first) {
                check_true(turbowasm_component_subtask_request_cancel(&state));
                check_false(state.cancellation_requested);
            }
            check_true(turbowasm_component_subtask_take_event(&state, &event));
            check_equal(event, TURBOWASM_COMPONENT_SUBTASK_RETURNED);
        }
    }

    it("classifies completed stream and future races at event delivery") {
        static const unsigned orders[6][3] = {
            {0u, 1u, 2u}, {0u, 2u, 1u}, {1u, 0u, 2u},
            {1u, 2u, 0u}, {2u, 0u, 1u}, {2u, 1u, 0u}
        };
        unsigned future, order, step;
        for (future = 0u; future < 2u; ++future) {
            for (order = 0u; order < 6u; ++order) {
                turbowasm_component_endpoint_state state = {0};
                uint32_t event = UINT32_MAX;
                state.future = future != 0u;
                check_true(turbowasm_component_endpoint_begin_copy(&state));
                for (step = 0u; step < 3u; ++step) {
                    switch (orders[order][step]) {
                        case 0u:
                            check_true(turbowasm_component_endpoint_notify(&state, 1u)); break;
                        case 1u:
                            check_true(turbowasm_component_endpoint_request_cancel(&state, true)); break;
                        default:
                            check_true(turbowasm_component_endpoint_peer_dropped(&state)); break;
                    }
                    check_not_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_DONE);
                }
                check_true(turbowasm_component_endpoint_take_event(&state, &event));
                check_equal(event, future ? TURBOWASM_COMPONENT_COPY_COMPLETED :
                    (1u << 4u) | TURBOWASM_COMPONENT_COPY_DROPPED);
                check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_DONE);
                check_false(turbowasm_component_endpoint_take_event(&state, &event));
                check_false(turbowasm_component_endpoint_begin_copy(&state));
            }
        }
    }

    it("keeps partial stream progress while cancellation wins over completion") {
        turbowasm_component_endpoint_state state = {0};
        uint32_t event;
        check_true(turbowasm_component_endpoint_begin_copy(&state));
        check_true(turbowasm_component_endpoint_notify(&state, 7u));
        check_true(turbowasm_component_endpoint_request_cancel(&state, false));
        check_false(turbowasm_component_endpoint_notify(&state, 0u));
        check_true(turbowasm_component_endpoint_take_event(&state, &event));
        check_equal(event, (7u << 4u) | TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_IDLE);
        check_true(turbowasm_component_endpoint_begin_copy(&state));
        check_true(turbowasm_component_endpoint_notify(&state, 2u));
        check_true(turbowasm_component_endpoint_take_event(&state, &event));
        check_equal(event, 2u << 4u);
    }

    it("waits for host cancellation acknowledgement before releasing a copy") {
        unsigned future;
        for (future = 0u; future < 2u; ++future) {
            turbowasm_component_endpoint_state state = {0};
            uint32_t event = UINT32_MAX;
            state.future = future != 0u;
            check_true(turbowasm_component_endpoint_begin_copy(&state));
            check_true(turbowasm_component_endpoint_request_cancel(&state, true));
            check_false(turbowasm_component_endpoint_request_cancel(&state, true));
            check_false(turbowasm_component_endpoint_take_event(&state, &event));
            check_equal(event, UINT32_MAX);
            check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_CANCELLING);
            check_false(turbowasm_component_endpoint_begin_copy(&state));
            check_true(turbowasm_component_endpoint_notify(&state, 0u));
            check_true(turbowasm_component_endpoint_take_event(&state, &event));
            check_equal(event, TURBOWASM_COMPONENT_COPY_CANCELLED);
            check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_IDLE);
        }
    }

    it("delivers peer closure from idle and retains it when a copy begins") {
        unsigned future, begin;
        for (future = 0u; future < 2u; ++future) {
            for (begin = 0u; begin < 2u; ++begin) {
                turbowasm_component_endpoint_state state = {0};
                uint32_t event;
                state.future = future != 0u;
                check_true(turbowasm_component_endpoint_peer_dropped(&state));
                check_false(turbowasm_component_endpoint_peer_dropped(&state));
                check_false(turbowasm_component_endpoint_notify(&state, 1u));
                if (begin)
                    check_true(turbowasm_component_endpoint_begin_copy(&state));
                check_true(turbowasm_component_endpoint_take_event(&state, &event));
                check_equal(event, TURBOWASM_COMPONENT_COPY_DROPPED);
                check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_DONE);
            }
        }
    }

    it("bounds cumulative progress and coalesces one pending notification") {
        turbowasm_component_endpoint_state state = {0};
        uint32_t event;
        check_false(turbowasm_component_endpoint_notify(&state, 0u));
        check_false(turbowasm_component_endpoint_request_cancel(&state, false));
        check_true(turbowasm_component_endpoint_begin_copy(&state));
        check_false(turbowasm_component_endpoint_begin_copy(&state));
        check_true(turbowasm_component_endpoint_notify(&state, 1u));
        check_true(turbowasm_component_endpoint_notify(&state, 9u));
        check_false(turbowasm_component_endpoint_notify(&state, 8u));
        check_true(turbowasm_component_endpoint_notify(&state, TURBOWASM_COMPONENT_COPY_MAX_LENGTH));
        check_false(turbowasm_component_endpoint_notify(&state, TURBOWASM_COMPONENT_COPY_MAX_LENGTH + 1u));
        check_false(turbowasm_component_endpoint_take_event(&state, NULL));
        check_true(state.pending_event);
        check_true(turbowasm_component_endpoint_take_event(&state, &event));
        check_equal(event, (uint32_t)TURBOWASM_COMPONENT_COPY_MAX_LENGTH << 4u);
        check_false(turbowasm_component_endpoint_take_event(&state, &event));
    }

    it("completes a future once and ignores later peer close notifications") {
        turbowasm_component_endpoint_state state = {0};
        uint32_t event;
        state.future = true;
        check_true(turbowasm_component_endpoint_begin_copy(&state));
        check_false(turbowasm_component_endpoint_notify(&state, 0u));
        check_false(turbowasm_component_endpoint_notify(&state, 2u));
        check_true(turbowasm_component_endpoint_notify(&state, 1u));
        check_true(turbowasm_component_endpoint_take_event(&state, &event));
        check_equal(event, TURBOWASM_COMPONENT_COPY_COMPLETED);
        check_equal(state.phase, TURBOWASM_COMPONENT_ENDPOINT_DONE);
        check_true(turbowasm_component_endpoint_peer_dropped(&state));
        check_false(state.pending_event);
        check_false(turbowasm_component_endpoint_begin_copy(&state));
        check_false(turbowasm_component_endpoint_notify(&state, 1u));
    }

    it("keeps simultaneous task and endpoint notifications independent") {
        turbowasm_component_subtask_state first = {0}, second = {0};
        turbowasm_component_endpoint_state reader = {0}, writer = {0};
        turbowasm_component_subtask_phase task_event;
        uint32_t copy_event;
        check_true(turbowasm_component_subtask_start(&first));
        check_true(turbowasm_component_subtask_request_cancel(&second));
        check_true(turbowasm_component_endpoint_begin_copy(&reader));
        check_true(turbowasm_component_endpoint_begin_copy(&writer));
        check_true(turbowasm_component_endpoint_notify(&reader, 4u));
        check_true(turbowasm_component_endpoint_request_cancel(&writer, false));
        check_true(turbowasm_component_subtask_resolve(&second, true));
        check_true(turbowasm_component_subtask_take_event(&second, &task_event));
        check_equal(task_event, TURBOWASM_COMPONENT_SUBTASK_CANCELLED_BEFORE_STARTED);
        check_true(first.pending_event);
        check_false(first.resolve_delivered);
        check_true(turbowasm_component_endpoint_take_event(&writer, &copy_event));
        check_equal(copy_event, TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_true(reader.pending_event);
        check_equal(reader.progress, 4u);
    }
}
