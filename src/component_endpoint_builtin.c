#include "component_endpoint_builtin.h"
#include "component_endpoint.h"
#include "runtime_alloc.h"
#include <string.h>

typedef struct turbowasm_component_task_owned_pair {
    turbowasm_component_endpoint reader, writer;
    struct turbowasm_component_task_owned_pair *next;
} turbowasm_component_task_owned_pair;

bool turbowasm_component_endpoint_builtin_kind(turbowasm_component_async_builtin_kind kind, bool *future) {
    if ((kind >= TURBOWASM_COMPONENT_STREAM_NEW && kind <= TURBOWASM_COMPONENT_STREAM_DROP_WRITABLE) ||
        kind == TURBOWASM_COMPONENT_STREAM_FORWARD) { *future = false; return true; }
    if ((kind >= TURBOWASM_COMPONENT_FUTURE_NEW && kind <= TURBOWASM_COMPONENT_FUTURE_DROP_WRITABLE) ||
        kind == TURBOWASM_COMPONENT_FUTURE_FORWARD) { *future = true; return true; }
    return false;
}

void turbowasm_component_endpoint_domain_collect(turbowasm_component_task_domain *domain) {
    turbowasm_component_task_owned_pair **link = &domain->pairs;
    while (*link != NULL) {
        turbowasm_component_task_owned_pair *pair = *link;
        if (pair->reader.closed && pair->writer.closed) {
            *link = pair->next; --domain->pair_count; turbowasm_rt_free(pair);
        } else link = &pair->next;
    }
}

turbowasm_status turbowasm_component_endpoint_domain_pair_open(
    turbowasm_component_task_domain *domain, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type_id, bool guest_handles,
    turbowasm_component_endpoint **reader, turbowasm_component_endpoint **writer) {
    turbowasm_component_task_owned_pair *pair;
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (domain == NULL || domain->table == NULL || domain->table->max_entries == 0u ||
        reader == NULL || writer == NULL || reader == writer || *reader != NULL || *writer != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    type = turbowasm_component_type_graph_get(graph, type_id);
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_STREAM && type->kind != TURBOWASM_COMPONENT_TYPE_FUTURE))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_type_graph_validate(graph)) return TURBOWASM_TYPE_MISMATCH;
    turbowasm_component_endpoint_domain_collect(domain);
    /* Moving ends does not release stable storage. Bound that retained storage
     * independently of currently occupied handle slots. */
    if (domain->pair_count >= domain->table->max_entries) return TURBOWASM_OUT_OF_MEMORY;
    pair = turbowasm_rt_calloc(1, sizeof(*pair));
    if (pair == NULL) return TURBOWASM_OUT_OF_MEMORY;
    status = turbowasm_component_endpoint_pair_open(graph, type_id,
        guest_handles ? domain->table : NULL, guest_handles ? domain->table : NULL, &pair->reader, &pair->writer);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(pair); return status; }
    pair->next = domain->pairs; domain->pairs = pair; ++domain->pair_count;
    *reader = &pair->reader; *writer = &pair->writer;
    return TURBOWASM_OK;
}

static turbowasm_status new_pair(turbowasm_component_task_builtin *binding, turbowasm_value *result) {
    turbowasm_component_endpoint *reader = NULL, *writer = NULL;
    turbowasm_status status = turbowasm_component_endpoint_domain_pair_open(binding->domain,
        binding->graph, binding->definition.type_index, true, &reader, &writer);
    if (status != TURBOWASM_OK) return status;
    result->kind = TURBOWASM_VALUE_I64;
    result->as.i64 = (int64_t)((uint64_t)reader->waitable.handle | ((uint64_t)writer->waitable.handle << 32));
    return TURBOWASM_OK;
}

static turbowasm_component_endpoint *get_endpoint(turbowasm_component_task_builtin *binding,
    uint32_t handle, bool future, bool readable) {
    turbowasm_component_handle_kind kind = future
        ? (readable ? TURBOWASM_COMPONENT_HANDLE_FUTURE_READ : TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE)
        : (readable ? TURBOWASM_COMPONENT_HANDLE_STREAM_READ : TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE);
    turbowasm_component_endpoint *endpoint = turbowasm_component_endpoint_get(binding->domain->table, handle, kind);
    if (endpoint == NULL || !turbowasm_component_value_type_equal(binding->graph,
        turbowasm_component_type_ref_indexed(binding->definition.type_index), endpoint->graph,
        turbowasm_component_type_ref_indexed(endpoint->type))) return NULL;
    return endpoint;
}

turbowasm_status turbowasm_component_endpoint_builtin_ready(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle handle, bool *ready, bool *can_unwind) {
    turbowasm_component_endpoint *endpoint = turbowasm_component_endpoint_get(domain->table, handle,
        turbowasm_component_handle_kind_get(domain->table, handle));
    if (endpoint == NULL || !endpoint->waitable.sync_waiter) return TURBOWASM_TRAPPED;
    *can_unwind = !endpoint->waitable.delivering;
    *ready = *can_unwind && endpoint->waitable.state.endpoint.pending_event;
    return TURBOWASM_OK;
}

static turbowasm_status receive_event(turbowasm_component_task_builtin *binding, turbowasm_host_call *call,
    turbowasm_component_endpoint *endpoint, bool pinned, uint32_t *payload) {
    turbowasm_component_event event;
    turbowasm_status status;
    for (;;) {
        status = pinned ? turbowasm_component_waitable_wait_end(binding->domain->table, endpoint->waitable.handle, &event)
                        : turbowasm_component_endpoint_take(endpoint, &event);
        if (status != TURBOWASM_YIELDED) break;
        if (!pinned) { *payload = UINT32_MAX; return TURBOWASM_OK; }
        status = turbowasm_component_task_builtin_suspend(binding->domain->active, call,
            TURBOWASM_COMPONENT_TASK_WAIT_ENDPOINT, endpoint->waitable.handle);
        if (status != TURBOWASM_OK) {
            /* Only local copies reach this boundary: cancellation acknowledges
             * immediately. Return the region before this Core frame unwinds. */
            (void)turbowasm_component_waitable_wait_cancel(binding->domain->table, endpoint->waitable.handle);
            if (endpoint->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                (void)turbowasm_component_endpoint_cancel(endpoint);
            (void)turbowasm_component_endpoint_take(endpoint, &event);
            return status;
        }
    }
    if (status == TURBOWASM_OK) *payload = event.payload;
    return status;
}

static uint64_t pointer_value(const turbowasm_component_task_builtin *binding, const turbowasm_value *value) {
    return binding->memory.pointer_type == TURBOWASM_COMPONENT_POINTER_I64 ? (uint64_t)value->as.i64 : (uint32_t)value->as.i32;
}

static turbowasm_status copy_endpoint(turbowasm_component_task_builtin *binding, turbowasm_host_call *call,
    const turbowasm_value *arguments, turbowasm_component_endpoint *endpoint, bool future, turbowasm_value *result) {
    turbowasm_component_buffer buffer = {0};
    turbowasm_status status;
    uint32_t payload;
    uint64_t length = future ? 1 : pointer_value(binding, &arguments[2]);
    bool synchronous = !binding->definition.is_async;
    if (length > TURBOWASM_COMPONENT_COPY_MAX_LENGTH ||
        endpoint->operation != NULL || endpoint->guest_buffer.leased ||
        (synchronous && !turbowasm_host_call_can_wait(call))) return TURBOWASM_TRAPPED;
    buffer.kind = TURBOWASM_COMPONENT_BUFFER_GUEST; buffer.length = (uint32_t)length;
    buffer.guest.graph = binding->graph;
    buffer.guest.type = turbowasm_component_type_graph_get(binding->graph, binding->definition.type_index)->as.async_value.payload;
    buffer.guest.memory = binding->memory; buffer.guest.address = pointer_value(binding, &arguments[1]);
    buffer.guest.commit = binding->buffer_commit; buffer.guest.rollback = binding->buffer_rollback;
    buffer.guest.context = binding->buffer_context;
    if (binding->buffer_prepare != NULL && length != 0u) {
        status = binding->buffer_prepare(binding->buffer_prepare_context, &buffer);
        if (status != TURBOWASM_OK) return status;
    }
    status = turbowasm_component_endpoint_submit_guest(endpoint, &buffer, synchronous, binding->domain->active);
    if (status != TURBOWASM_OK) {
        if (buffer.guest.release != NULL) (void)buffer.guest.release(buffer.guest.context);
        return status == TURBOWASM_INVALID_ARGUMENT ? TURBOWASM_TRAPPED : status;
    }
    status = receive_event(binding, call, endpoint, synchronous, &payload);
    if (status == TURBOWASM_OK) {
        result->kind = binding->results[0];
        if (result->kind == TURBOWASM_VALUE_I64) result->as.i64 = (int64_t)(uint64_t)payload;
        else result->as.i32 = (int32_t)payload;
    }
    return status;
}

turbowasm_status turbowasm_component_endpoint_builtin_invoke(turbowasm_component_task_builtin *binding,
    turbowasm_host_call *call, const turbowasm_value *arguments, turbowasm_value *result) {
    turbowasm_component_async_builtin_kind kind = binding->definition.kind;
    turbowasm_component_endpoint *endpoint;
    turbowasm_status status;
    bool future, readable, copy, cancel;
    if (!turbowasm_component_endpoint_builtin_kind(kind, &future)) return TURBOWASM_UNSUPPORTED;
    if (kind == TURBOWASM_COMPONENT_STREAM_NEW || kind == TURBOWASM_COMPONENT_FUTURE_NEW) return new_pair(binding, result);
    readable = kind == TURBOWASM_COMPONENT_STREAM_READ || kind == TURBOWASM_COMPONENT_FUTURE_READ ||
        kind == TURBOWASM_COMPONENT_STREAM_CANCEL_READ || kind == TURBOWASM_COMPONENT_FUTURE_CANCEL_READ ||
        kind == TURBOWASM_COMPONENT_STREAM_DROP_READABLE || kind == TURBOWASM_COMPONENT_FUTURE_DROP_READABLE ||
        kind == TURBOWASM_COMPONENT_STREAM_FORWARD || kind == TURBOWASM_COMPONENT_FUTURE_FORWARD;
    endpoint = get_endpoint(binding, (uint32_t)arguments[0].as.i32, future, readable);
    if (endpoint == NULL) return TURBOWASM_TRAPPED;
    copy = kind == TURBOWASM_COMPONENT_STREAM_READ || kind == TURBOWASM_COMPONENT_FUTURE_READ ||
        kind == TURBOWASM_COMPONENT_STREAM_WRITE || kind == TURBOWASM_COMPONENT_FUTURE_WRITE;
    if (copy) return copy_endpoint(binding, call, arguments, endpoint, future, result);
    cancel = kind == TURBOWASM_COMPONENT_STREAM_CANCEL_READ || kind == TURBOWASM_COMPONENT_FUTURE_CANCEL_READ ||
        kind == TURBOWASM_COMPONENT_STREAM_CANCEL_WRITE || kind == TURBOWASM_COMPONENT_FUTURE_CANCEL_WRITE;
    if (cancel) {
        uint32_t payload;
        if (!binding->definition.is_async && endpoint->waitable.set_handle != 0) return TURBOWASM_TRAPPED;
        status = turbowasm_component_endpoint_cancel(endpoint);
        if (status != TURBOWASM_OK) return status;
        /* The local copy engine always acknowledges without external I/O. */
        status = receive_event(binding, call, endpoint, false, &payload);
        if (status == TURBOWASM_OK) { result->kind = TURBOWASM_VALUE_I32; result->as.i32 = (int32_t)payload; }
        return status;
    }
    if (kind == TURBOWASM_COMPONENT_STREAM_FORWARD || kind == TURBOWASM_COMPONENT_FUTURE_FORWARD) {
        turbowasm_component_endpoint *writer = get_endpoint(binding, (uint32_t)arguments[1].as.i32, future, false);
        if (writer == NULL) return TURBOWASM_TRAPPED;
        status = turbowasm_component_endpoint_forward_from_task(endpoint, writer, binding->domain->active);
    } else {
        if (future && !readable && endpoint->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_DONE)
            return TURBOWASM_TRAPPED;
        status = turbowasm_component_endpoint_close(endpoint);
    }
    turbowasm_component_endpoint_domain_collect(binding->domain);
    return status;
}
