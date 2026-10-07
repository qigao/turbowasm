#include "component_endpoint.h"
#include "runtime_alloc.h"

#include <string.h>

static turbowasm_status release_buffer(void *context) {
    turbowasm_component_endpoint *endpoint = context;
    if (endpoint->operation != NULL)
        endpoint->operation->leased = false;
    endpoint->operation = NULL;
    endpoint->available = NULL;
    return endpoint->failure;
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
    reader->type = writer->type = type_id;
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
    return endpoint != NULL && endpoint->initialized && !endpoint->closed && endpoint->value_owner == NULL;
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
    const turbowasm_component_buffer *buffer) {
    if (buffer == NULL || buffer->leased || buffer->length > TURBOWASM_COMPONENT_COPY_MAX_LENGTH ||
        (endpoint->waitable.state.endpoint.future && buffer->length != 1u))
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_buffer_validate(buffer, endpoint->graph,
        endpoint->has_payload, endpoint->payload, endpoint->readable);
}

static bool overlaps(const turbowasm_component_buffer *a,
    const turbowasm_component_buffer *b) {
    if (a->kind != TURBOWASM_COMPONENT_BUFFER_HOST || b->kind != TURBOWASM_COMPONENT_BUFFER_HOST)
        return false;
    uintptr_t first = (uintptr_t)a->values, second = (uintptr_t)b->values;
    size_t first_size = (size_t)a->length * sizeof(*a->values);
    size_t second_size = (size_t)b->length * sizeof(*b->values);
    if (first_size == 0u || second_size == 0u) return false;
    return first <= second ? second - first < first_size : first - second < second_size;
}

/* Both operations are admitted and COPYING before any conversion callback. */
static turbowasm_status transfer_buffers(turbowasm_component_endpoint *reader,
    turbowasm_component_endpoint *writer, uint32_t count) {
    turbowasm_status status;
    reader->waitable.delivering = writer->waitable.delivering = true;
    status = turbowasm_component_buffer_copy(writer->operation, reader->operation, reader->has_payload, count);
    reader->waitable.delivering = writer->waitable.delivering = false;
    if (status != TURBOWASM_OK) {
        reader->failure = writer->failure = status;
        reader->available = writer->available = NULL;
        if (!turbowasm_component_endpoint_request_cancel(&reader->waitable.state.endpoint, false) ||
            !turbowasm_component_endpoint_request_cancel(&writer->waitable.state.endpoint, false))
            return TURBOWASM_TRAPPED;
    }
    return status;
}

turbowasm_status turbowasm_component_endpoint_submit(
    turbowasm_component_endpoint *endpoint, turbowasm_component_buffer *buffer) {
    turbowasm_component_endpoint *peer;
    turbowasm_component_buffer *other;
    turbowasm_component_endpoint_state next_self, next_peer = {0};
    turbowasm_status status;
    uint32_t remaining = 0u, count = 0u;
    bool self_available = false, other_finished = false;
    if (endpoint != NULL && endpoint->failure != TURBOWASM_OK) return endpoint->failure;
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

    /* Preflight notification transitions before lending the new buffer. Data
     * conversion can still trap; guarded error delivery returns both borrows. */
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
        endpoint->waitable.state.endpoint.phase = TURBOWASM_COMPONENT_ENDPOINT_COPYING;
        status = transfer_buffers(endpoint->readable ? endpoint : peer,
            endpoint->readable ? peer : endpoint, count);
        if (status != TURBOWASM_OK) return TURBOWASM_OK;
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
    turbowasm_status status;
    if (!live(endpoint) || out_event == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint->waitable.delivering || endpoint->waitable.sync_waiter)
        return TURBOWASM_TRAPPED;
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
    status = release_buffer(endpoint);
    if (status != TURBOWASM_OK) return status;
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

static bool movable_idle(const turbowasm_component_endpoint *endpoint) {
    return live(endpoint) && endpoint->failure == TURBOWASM_OK && endpoint->operation == NULL &&
        endpoint->available == NULL && !endpoint->waitable.delivering &&
        !endpoint->waitable.sync_waiter && endpoint->waitable.set_handle == 0u &&
        endpoint->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_IDLE;
}

static bool movable_readable(const turbowasm_component_endpoint *endpoint) {
    return movable_idle(endpoint) && endpoint->readable;
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

static turbowasm_status release_endpoint_value(void *context) {
    turbowasm_component_endpoint_value_owner *owner = context;
    turbowasm_component_endpoint *endpoint;
    turbowasm_status status = TURBOWASM_OK;
    if (owner == NULL) return TURBOWASM_TRAPPED;
    endpoint = owner->endpoint;
    if (endpoint != NULL) {
        if (endpoint->value_owner != owner || endpoint->lower_scope != NULL)
            return TURBOWASM_TRAPPED;
        endpoint->value_owner = NULL;
        status = turbowasm_component_endpoint_close(endpoint);
    }
    turbowasm_rt_free(owner);
    return status;
}

static void bind_endpoint_value(turbowasm_component_endpoint *endpoint,
    turbowasm_component_endpoint_value_owner *owner, turbowasm_component_value *out) {
    turbowasm_component_value value = {0};
    value.kind = endpoint->waitable.state.endpoint.future
        ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM;
    value.as.endpoint.owner = owner;
    value.as.endpoint.graph = endpoint->graph;
    value.as.endpoint.type = turbowasm_component_type_ref_indexed(endpoint->type);
    value.release = release_endpoint_value;
    value.release_context = owner;
    owner->endpoint = endpoint;
    endpoint->value_owner = owner;
    *out = value;
}

turbowasm_status turbowasm_component_endpoint_into_value(
    turbowasm_component_endpoint *endpoint, turbowasm_component_value *out) {
    turbowasm_component_endpoint_value_owner *owner;
    if (out == NULL || out->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || out->release != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!movable_readable(endpoint) || endpoint->waitable.table != NULL)
        return TURBOWASM_TRAPPED;
    owner = turbowasm_rt_malloc(sizeof(*owner));
    if (owner == NULL) return TURBOWASM_OUT_OF_MEMORY;
    bind_endpoint_value(endpoint, owner, out);
    return TURBOWASM_OK;
}

static turbowasm_component_endpoint *owned_endpoint(const turbowasm_component_value *value) {
    turbowasm_component_endpoint *endpoint;
    if (value == NULL ||
        (value->kind != TURBOWASM_COMPONENT_TYPE_FUTURE && value->kind != TURBOWASM_COMPONENT_TYPE_STREAM) ||
        value->release != release_endpoint_value || value->as.endpoint.owner == NULL ||
        value->release_context != value->as.endpoint.owner)
        return NULL;
    endpoint = value->as.endpoint.owner->endpoint;
    if (endpoint == NULL || endpoint->value_owner != value->as.endpoint.owner ||
        !endpoint->initialized || endpoint->closed || !endpoint->readable ||
        (value->kind == TURBOWASM_COMPONENT_TYPE_FUTURE) != endpoint->waitable.state.endpoint.future)
        return NULL;
    return endpoint;
}

turbowasm_status turbowasm_component_endpoint_take_value(
    turbowasm_component_value *value, turbowasm_component_endpoint **out) {
    turbowasm_component_endpoint *endpoint = owned_endpoint(value);
    if (out == NULL || endpoint == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint->lower_scope != NULL)
        return TURBOWASM_TRAPPED;
    turbowasm_rt_free(endpoint->value_owner);
    memset(value, 0, sizeof(*value));
    endpoint->value_owner = NULL;
    *out = endpoint;
    return TURBOWASM_OK;
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

static bool forwardable(const turbowasm_component_endpoint *endpoint) {
    const turbowasm_component_endpoint *peer;
    if (!movable_idle(endpoint) || endpoint->lower_scope != NULL)
        return false;
    if (endpoint->waitable.table != NULL &&
        turbowasm_component_handle_object(endpoint->waitable.table, endpoint->waitable.handle,
            endpoint_kind(endpoint)) != &endpoint->waitable)
        return false;
    peer = endpoint->peer;
    return peer == NULL || (peer->peer == endpoint && !peer->closed && !peer->waitable.delivering);
}

turbowasm_status turbowasm_component_endpoint_forward(
    turbowasm_component_endpoint *source, turbowasm_component_endpoint *destination) {
    turbowasm_component_endpoint *reader, *writer;
    turbowasm_component_endpoint_state next_read, next_write;
    turbowasm_status status;
    uint32_t read_remaining = 0u, write_remaining = 0u, count = 0u;
    bool rendezvous;
    if (!forwardable(source) || !forwardable(destination) || !source->readable || destination->readable)
        return TURBOWASM_TRAPPED;
    if (!turbowasm_component_value_type_equal(source->graph,
        turbowasm_component_type_ref_indexed(source->type), destination->graph,
        turbowasm_component_type_ref_indexed(destination->type)))
        return TURBOWASM_TYPE_MISMATCH;
    writer = source->peer; reader = destination->peer;
    if (writer == destination || writer == NULL || reader == NULL) {
        status = turbowasm_component_endpoint_close(source);
        return status == TURBOWASM_OK ? turbowasm_component_endpoint_close(destination) : status;
    }
    rendezvous = reader->available != NULL && writer->available != NULL;
    next_read = reader->waitable.state.endpoint;
    next_write = writer->waitable.state.endpoint;
    if (rendezvous) {
        const turbowasm_component_buffer *read = reader->available, *write = writer->available;
        if (read != reader->operation || write != writer->operation || !read->leased || !write->leased ||
            read->progress > read->length || write->progress > write->length ||
            next_read.phase != TURBOWASM_COMPONENT_ENDPOINT_COPYING ||
            next_write.phase != TURBOWASM_COMPONENT_ENDPOINT_COPYING)
            return TURBOWASM_TRAPPED;
        read_remaining = read->length - read->progress;
        write_remaining = write->length - write->progress;
        if (read_remaining != 0u && write_remaining != 0u) {
            if ((reader->has_payload && overlaps(read, write)) ||
                (reader->waitable.table != NULL && reader->waitable.table == writer->waitable.table &&
                 !numeric_or_unit(reader)))
                return TURBOWASM_TRAPPED;
            count = read_remaining < write_remaining ? read_remaining : write_remaining;
            if (!turbowasm_component_endpoint_notify(&next_read, read->progress + count) ||
                !turbowasm_component_endpoint_notify(&next_write, write->progress + count))
                return TURBOWASM_TRAPPED;
        } else if (!turbowasm_component_endpoint_notify(
            write_remaining == 0u ? &next_write : &next_read, 0u))
            return TURBOWASM_TRAPPED;
    }
    /* Registration validity was checked before this callback-free commit. */
    if (source->waitable.table != NULL) {
        status = turbowasm_component_waitable_drop(source->waitable.table, source->waitable.handle);
        if (status != TURBOWASM_OK) return status;
    }
    if (destination->waitable.table != NULL) {
        status = turbowasm_component_waitable_drop(destination->waitable.table, destination->waitable.handle);
        if (status != TURBOWASM_OK) return status;
    }
    source->peer = destination->peer = NULL;
    source->closed = destination->closed = true;
    reader->peer = writer; writer->peer = reader;
    if (count != 0u) {
        status = transfer_buffers(reader, writer, count);
        if (status != TURBOWASM_OK) return status;
    }
    if (rendezvous) {
        reader->waitable.state.endpoint = next_read;
        writer->waitable.state.endpoint = next_write;
        if (count != 0u) {
            if (count == read_remaining) reader->available = NULL;
            if (count == write_remaining) writer->available = NULL;
        } else if (write_remaining == 0u) writer->available = NULL;
        else reader->available = NULL;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_endpoint_codec_lift(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    uint32_t handle, turbowasm_component_value *out) {
    turbowasm_component_endpoint_codec *codec = context;
    const turbowasm_component_type *type;
    turbowasm_component_handle_kind kind;
    turbowasm_component_waitable *waitable;
    turbowasm_component_endpoint *endpoint;
    turbowasm_component_endpoint_value_owner *owner;
    turbowasm_status status;
    if (codec == NULL || codec->table == NULL || out == NULL ||
        out->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || out->release != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    type = ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED
        ? turbowasm_component_type_graph_get(graph, ref.as.indexed) : NULL;
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_STREAM &&
        type->kind != TURBOWASM_COMPONENT_TYPE_FUTURE)) return TURBOWASM_TYPE_MISMATCH;
    kind = type->kind == TURBOWASM_COMPONENT_TYPE_FUTURE
        ? TURBOWASM_COMPONENT_HANDLE_FUTURE_READ : TURBOWASM_COMPONENT_HANDLE_STREAM_READ;
    waitable = turbowasm_component_handle_object(codec->table, handle, kind);
    /* Only endpoint-owned waitables can be interpreted as paired endpoints. */
    if (waitable == NULL || waitable->release_pending != release_buffer ||
        waitable->release_context == NULL || waitable->table != codec->table || waitable->handle != handle)
        return TURBOWASM_TRAPPED;
    endpoint = waitable->release_context;
    if (&endpoint->waitable != waitable || !movable_readable(endpoint))
        return TURBOWASM_TRAPPED;
    if (!turbowasm_component_value_type_equal(graph, ref, endpoint->graph,
        turbowasm_component_type_ref_indexed(endpoint->type))) return TURBOWASM_TYPE_MISMATCH;
    owner = turbowasm_rt_malloc(sizeof(*owner));
    if (owner == NULL) return TURBOWASM_OUT_OF_MEMORY;
    status = turbowasm_component_endpoint_detach_readable(endpoint);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(owner);
        return status;
    }
    bind_endpoint_value(endpoint, owner, out);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_endpoint_codec_lower(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    const turbowasm_component_value *value, uint32_t *out_handle) {
    turbowasm_component_endpoint_codec *codec = context;
    turbowasm_component_endpoint *endpoint = owned_endpoint(value);
    turbowasm_component_resource_handle handle;
    turbowasm_status status;
    if (codec == NULL || codec->table == NULL || out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint == NULL || !turbowasm_component_value_type_equal(graph, ref, endpoint->graph,
        turbowasm_component_type_ref_indexed(endpoint->type))) return TURBOWASM_TYPE_MISMATCH;
    if (endpoint->lower_scope != NULL || endpoint->waitable.table != NULL)
        return TURBOWASM_TRAPPED;
    status = turbowasm_component_handle_insert(codec->table, endpoint_kind(endpoint), &endpoint->waitable, &handle);
    if (status != TURBOWASM_OK) return status;
    endpoint->lower_scope = codec;
    endpoint->lower_handle = handle;
    endpoint->lower_next = codec->lower_head;
    codec->lower_head = endpoint;
    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status finish_codec(turbowasm_component_endpoint_codec *codec, bool commit) {
    turbowasm_component_endpoint *endpoint;
    if (codec == NULL || codec->table == NULL) return TURBOWASM_INVALID_ARGUMENT;
    /* No allocation/callback between this preflight and the exclusive commit. */
    for (endpoint = codec->lower_head; endpoint != NULL; endpoint = endpoint->lower_next)
        if (endpoint->lower_scope != codec || endpoint->value_owner == NULL ||
            endpoint->value_owner->endpoint != endpoint || endpoint->waitable.table != NULL ||
            turbowasm_component_handle_object(codec->table, endpoint->lower_handle,
                endpoint_kind(endpoint)) != &endpoint->waitable)
            return TURBOWASM_TRAPPED;
    while ((endpoint = codec->lower_head) != NULL) {
        if (commit) {
            endpoint->waitable.table = codec->table;
            endpoint->waitable.handle = endpoint->lower_handle;
            endpoint->value_owner->endpoint = NULL;
            endpoint->value_owner = NULL;
        } else {
            void *object;
            turbowasm_status status = turbowasm_component_handle_remove(codec->table,
                endpoint->lower_handle, endpoint_kind(endpoint), &object);
            if (status != TURBOWASM_OK) return status;
        }
        codec->lower_head = endpoint->lower_next;
        endpoint->lower_scope = NULL;
        endpoint->lower_next = NULL;
        endpoint->lower_handle = 0u;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_endpoint_codec_commit(turbowasm_component_endpoint_codec *codec) {
    return finish_codec(codec, true);
}

turbowasm_status turbowasm_component_endpoint_codec_rollback(turbowasm_component_endpoint_codec *codec) {
    return finish_codec(codec, false);
}
