#include "component_endpoint.h"

#include <string.h>

static turbowasm_status release_buffer(void *context) {
    turbowasm_component_endpoint *endpoint = context;
    if (endpoint->operation != NULL)
        endpoint->operation->leased = false;
    endpoint->operation = NULL;
    endpoint->available = NULL;
    return TURBOWASM_OK;
}

static turbowasm_component_handle_kind endpoint_kind(const turbowasm_component_endpoint *endpoint) {
    if (endpoint->waitable.state.endpoint.future)
        return endpoint->readable ? TURBOWASM_COMPONENT_HANDLE_FUTURE_READ : TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE;
    return endpoint->readable ? TURBOWASM_COMPONENT_HANDLE_STREAM_READ : TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE;
}

static void initialize_endpoint(turbowasm_component_endpoint *endpoint,
    const turbowasm_component_type_graph *graph, const turbowasm_component_type *type,
    bool readable, turbowasm_component_endpoint *peer) {
    endpoint->graph = graph;
    endpoint->payload = type->as.async_value.payload;
    endpoint->has_payload = type->as.async_value.has_payload;
    endpoint->readable = readable;
    endpoint->initialized = true;
    endpoint->peer = peer;
    endpoint->waitable.state.endpoint.future = type->kind == TURBOWASM_COMPONENT_TYPE_FUTURE;
    endpoint->waitable.release_pending = release_buffer;
    endpoint->waitable.release_context = endpoint;
}

turbowasm_status turbowasm_component_endpoint_pair_open(
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type_id,
    turbowasm_component_resource_table *reader_table,
    turbowasm_component_resource_table *writer_table,
    turbowasm_component_endpoint *reader, turbowasm_component_endpoint *writer) {
    const turbowasm_component_type *type = turbowasm_component_type_graph_get(graph, type_id);
    turbowasm_status status = TURBOWASM_OK;
    if (reader == NULL || writer == NULL || reader == writer ||
        reader->initialized || writer->initialized ||
        reader->waitable.table != NULL || writer->waitable.table != NULL ||
        type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_STREAM &&
                         type->kind != TURBOWASM_COMPONENT_TYPE_FUTURE))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_type_graph_validate(graph))
        return TURBOWASM_TYPE_MISMATCH;
    initialize_endpoint(reader, graph, type, true, writer);
    initialize_endpoint(writer, graph, type, false, reader);
    if (reader_table != NULL)
        status = turbowasm_component_waitable_register(reader_table, endpoint_kind(reader), &reader->waitable);
    if (status == TURBOWASM_OK && writer_table != NULL)
        status = turbowasm_component_waitable_register(writer_table, endpoint_kind(writer), &writer->waitable);
    if (status != TURBOWASM_OK) {
        if (reader->waitable.table != NULL) {
            turbowasm_status rollback = turbowasm_component_waitable_drop(reader_table, reader->waitable.handle);
            if (rollback != TURBOWASM_OK) return rollback;
        }
        memset(reader, 0, sizeof(*reader));
        memset(writer, 0, sizeof(*writer));
    }
    return status;
}

static bool live(const turbowasm_component_endpoint *endpoint) {
    return endpoint != NULL && endpoint->initialized && !endpoint->closed;
}

static bool numeric_or_unit(const turbowasm_component_endpoint *endpoint) {
    const turbowasm_component_type *type;
    turbowasm_component_type_kind kind;
    if (!endpoint->has_payload)
        return true;
    if (endpoint->payload.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        kind = endpoint->payload.as.inline_type;
    else {
        type = turbowasm_component_type_graph_get(endpoint->graph, endpoint->payload.as.indexed);
        if (type == NULL) return false;
        kind = type->kind;
    }
    return kind >= TURBOWASM_COMPONENT_TYPE_S8 && kind <= TURBOWASM_COMPONENT_TYPE_F64;
}

static turbowasm_status validate_buffer(const turbowasm_component_endpoint *endpoint,
    const turbowasm_component_host_buffer *buffer) {
    uint32_t i;
    if (buffer == NULL || buffer->leased || buffer->length > TURBOWASM_COMPONENT_COPY_MAX_LENGTH ||
        (endpoint->waitable.state.endpoint.future && buffer->length != 1u))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!endpoint->has_payload || buffer->length == 0u)
        return TURBOWASM_OK;
    if (buffer->values == NULL || (size_t)buffer->length > SIZE_MAX / sizeof(*buffer->values))
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < buffer->length; ++i) {
        if (endpoint->readable) {
            if (buffer->values[i].kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || buffer->values[i].release != NULL)
                return TURBOWASM_INVALID_ARGUMENT;
        } else {
            turbowasm_status status = turbowasm_component_canonical_validate_value(
                endpoint->graph, endpoint->payload, &buffer->values[i]);
            if (status != TURBOWASM_OK) return status;
        }
    }
    return TURBOWASM_OK;
}

static bool overlaps(const turbowasm_component_host_buffer *a,
    const turbowasm_component_host_buffer *b) {
    uintptr_t first = (uintptr_t)a->values, second = (uintptr_t)b->values;
    size_t first_size = (size_t)a->length * sizeof(*a->values);
    size_t second_size = (size_t)b->length * sizeof(*b->values);
    if (first_size == 0u || second_size == 0u) return false;
    return first <= second ? second - first < first_size : first - second < second_size;
}

turbowasm_status turbowasm_component_endpoint_submit(
    turbowasm_component_endpoint *endpoint, turbowasm_component_host_buffer *buffer) {
    turbowasm_component_endpoint *peer;
    turbowasm_component_host_buffer *other;
    turbowasm_component_endpoint_state next_self, next_peer = {0};
    turbowasm_status status;
    uint32_t remaining = 0u, count = 0u;
    bool self_available = false, other_finished = false;
    if (!live(endpoint) || endpoint->operation != NULL ||
        endpoint->waitable.delivering || endpoint->waitable.sync_waiter ||
        endpoint->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE)
        return TURBOWASM_INVALID_ARGUMENT;
    status = validate_buffer(endpoint, buffer);
    if (status != TURBOWASM_OK) return status;
    peer = endpoint->peer;
    other = peer != NULL ? peer->available : NULL;
    if (other != NULL && endpoint->has_payload && overlaps(buffer, other))
        return TURBOWASM_INVALID_ARGUMENT;
    if (other != NULL && buffer->length != 0u && other->length > other->progress &&
        endpoint->waitable.table != NULL && endpoint->waitable.table == peer->waitable.table &&
        !numeric_or_unit(endpoint))
        return TURBOWASM_TRAPPED;

    /* Preflight all state transitions before moving values. The commit below
     * has no callback, allocation or other failure point. */
    next_self = endpoint->waitable.state.endpoint;
    if (!turbowasm_component_endpoint_begin_copy(&next_self))
        return TURBOWASM_TRAPPED;
    if (peer != NULL) next_peer = peer->waitable.state.endpoint;
    if (peer == NULL) {
        if (!next_self.peer_dropped || !next_self.pending_event)
            return TURBOWASM_TRAPPED;
    } else if (other == NULL) {
        self_available = true;
    } else {
        if (other->progress > other->length || !other->leased)
            return TURBOWASM_TRAPPED;
        remaining = other->length - other->progress;
        if (buffer->length != 0u && remaining != 0u) {
            count = buffer->length < remaining ? buffer->length : remaining;
            if (!turbowasm_component_endpoint_notify(&next_self, count) ||
                !turbowasm_component_endpoint_notify(&next_peer, other->progress + count))
                return TURBOWASM_TRAPPED;
            other_finished = count == remaining;
        } else if (buffer->length != 0u || (endpoint->readable && remaining == 0u)) {
            if (!turbowasm_component_endpoint_notify(&next_peer, 0u))
                return TURBOWASM_TRAPPED;
            other_finished = true;
            self_available = true;
        } else if (!turbowasm_component_endpoint_notify(&next_self, 0u)) {
            return TURBOWASM_TRAPPED;
        }
    }
    buffer->progress = 0u;
    buffer->leased = true;
    endpoint->operation = buffer;
    if (count != 0u) {
        uint32_t i;
        turbowasm_component_host_buffer *source = endpoint->readable ? other : buffer;
        turbowasm_component_host_buffer *destination = endpoint->readable ? buffer : other;
        if (endpoint->has_payload) {
            for (i = 0u; i < count; ++i) {
                turbowasm_component_value *from = &source->values[source->progress + i];
                destination->values[destination->progress + i] = *from;
                memset(from, 0, sizeof(*from));
            }
        }
        buffer->progress = count;
        other->progress += count;
    }
    endpoint->available = self_available ? buffer : NULL;
    endpoint->waitable.state.endpoint = next_self;
    if (peer != NULL) {
        peer->waitable.state.endpoint = next_peer;
        if (other_finished) peer->available = NULL;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_endpoint_take(
    turbowasm_component_endpoint *endpoint, turbowasm_component_event *out_event) {
    turbowasm_component_event event = {0};
    if (!live(endpoint) || out_event == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint->waitable.table != NULL)
        return turbowasm_component_waitable_take(endpoint->waitable.table,
            endpoint->waitable.handle, out_event);
    if (!endpoint->waitable.state.endpoint.pending_event)
        return TURBOWASM_YIELDED;
    if (!turbowasm_component_endpoint_take_event(&endpoint->waitable.state.endpoint, &event.payload))
        return TURBOWASM_TRAPPED;
    if (endpoint->waitable.state.endpoint.future)
        event.code = endpoint->readable ? TURBOWASM_COMPONENT_EVENT_FUTURE_READ : TURBOWASM_COMPONENT_EVENT_FUTURE_WRITE;
    else
        event.code = endpoint->readable ? TURBOWASM_COMPONENT_EVENT_STREAM_READ : TURBOWASM_COMPONENT_EVENT_STREAM_WRITE;
    if (release_buffer(endpoint) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;
    *out_event = event;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_endpoint_cancel(turbowasm_component_endpoint *endpoint) {
    if (!live(endpoint) || endpoint->waitable.delivering || endpoint->waitable.sync_waiter ||
        !turbowasm_component_endpoint_request_cancel(&endpoint->waitable.state.endpoint, false))
        return TURBOWASM_TRAPPED;
    endpoint->available = NULL;
    return TURBOWASM_OK;
}

static bool movable_readable(const turbowasm_component_endpoint *endpoint) {
    return live(endpoint) && endpoint->readable && endpoint->operation == NULL &&
        endpoint->available == NULL && !endpoint->waitable.delivering &&
        !endpoint->waitable.sync_waiter && endpoint->waitable.set_handle == 0u &&
        endpoint->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_IDLE;
}

turbowasm_status turbowasm_component_endpoint_detach_readable(
    turbowasm_component_endpoint *endpoint) {
    if (!movable_readable(endpoint) || endpoint->waitable.table == NULL)
        return TURBOWASM_TRAPPED;
    return turbowasm_component_waitable_drop(endpoint->waitable.table, endpoint->waitable.handle);
}

turbowasm_status turbowasm_component_endpoint_attach_readable(
    turbowasm_component_endpoint *endpoint,
    turbowasm_component_resource_table *table) {
    if (!movable_readable(endpoint) || endpoint->waitable.table != NULL || table == NULL)
        return TURBOWASM_TRAPPED;
    return turbowasm_component_waitable_register(table, endpoint_kind(endpoint), &endpoint->waitable);
}

turbowasm_status turbowasm_component_endpoint_close(turbowasm_component_endpoint *endpoint) {
    turbowasm_component_endpoint *peer;
    turbowasm_component_endpoint_state peer_state = {0};
    turbowasm_status status;
    if (!live(endpoint)) return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint->operation != NULL || endpoint->waitable.sync_waiter || endpoint->waitable.delivering ||
        (endpoint->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE &&
         endpoint->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_DONE))
        return TURBOWASM_TRAPPED;
    peer = endpoint->peer;
    if (peer != NULL) {
        peer_state = peer->waitable.state.endpoint;
        if (peer->peer != endpoint || !turbowasm_component_endpoint_peer_dropped(&peer_state))
            return TURBOWASM_TRAPPED;
    }
    if (endpoint->waitable.table != NULL) {
        status = turbowasm_component_waitable_drop(endpoint->waitable.table, endpoint->waitable.handle);
        if (status != TURBOWASM_OK) return status;
    }
    endpoint->peer = NULL;
    endpoint->closed = true;
    if (peer != NULL) {
        peer->peer = NULL;
        peer->available = NULL;
        peer->waitable.state.endpoint = peer_state;
    }
    return TURBOWASM_OK;
}
