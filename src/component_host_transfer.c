#include "component_host_endpoint_internal.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"

#include <string.h>

typedef struct component_host_transfer_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_endpoint endpoint;
    turbowasm_component_host_budget *budget;
    turbowasm_component_buffer buffer;
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_ref payload;
    void *origin;
    void (*origin_release)(void *);
    size_t bytes, payload_capacity, payload_used;
    turbowasm_status status;
    turbowasm_component_event event;
    bool readable, terminal, driving;
} component_host_transfer_impl;

static bool transfer_driving(const component_host_transfer_impl *impl) {
    const component_host_endpoint_impl *body = impl->endpoint.impl;
    /* A peer may drive the copy on a retained guest stack while this owner is
     * otherwise idle. Its allocator/finalizers must not reenter the transfer. */
    return impl->driving || (body != NULL && (body->driving || body->endpoint->waitable.delivering));
}

static turbowasm_status size_add(size_t *bytes, size_t count, size_t stride, size_t limit) {
    if (*bytes > limit || count > (limit - *bytes) / stride) return TURBOWASM_OUT_OF_MEMORY;
    *bytes += count * stride;
    return TURBOWASM_OK;
}

/* Logical retained bytes, matching existing host admission accounting. Root
 * cells are charged separately. Inputs are unique Runtime-owned trees; scalar
 * reps do not allocate, while every nested node/owner has one cleanup authority. */
static turbowasm_status payload_size(const turbowasm_component_value *value, uint32_t depth,
    size_t *bytes, size_t limit) {
    const turbowasm_component_value_list *sequence = NULL;
    const turbowasm_component_value_variant *variant = NULL;
    const turbowasm_component_endpoint *endpoint;
    turbowasm_status status;
    uint64_t i;
    if (depth >= TURBOWASM_COMPONENT_VALUE_MAX_DEPTH) return TURBOWASM_TRAPPED;
    if (value->kind == TURBOWASM_COMPONENT_TYPE_OWN) {
        if (!turbowasm_component_exec_resource_value_retained(value)) return TURBOWASM_INVALID_ARGUMENT;
        return size_add(bytes, 1u, turbowasm_component_exec_resource_adopt_size(), limit);
    }
    if (value->kind == TURBOWASM_COMPONENT_TYPE_FUTURE || value->kind == TURBOWASM_COMPONENT_TYPE_STREAM) {
        endpoint = turbowasm_component_endpoint_value_get(value);
        if (endpoint == NULL || endpoint->lower_scope != NULL ||
            !turbowasm_component_endpoint_domain_pair_retained(endpoint)) return TURBOWASM_INVALID_ARGUMENT;
        return size_add(bytes, 1u, sizeof(turbowasm_component_endpoint_value_owner), limit);
    }
    if (value->kind == TURBOWASM_COMPONENT_TYPE_BORROW) return TURBOWASM_TYPE_MISMATCH;
    if (value->release != NULL || value->release_context != NULL) return TURBOWASM_INVALID_ARGUMENT;
    switch (value->kind) {
        case TURBOWASM_COMPONENT_TYPE_STRING:
            if (value->as.string.size != 0u && value->as.string.data == NULL) return TURBOWASM_INVALID_ARGUMENT;
            return size_add(bytes, value->as.string.size, 1u, limit);
        case TURBOWASM_COMPONENT_TYPE_LIST: sequence = &value->as.list; break;
        case TURBOWASM_COMPONENT_TYPE_RECORD: sequence = &value->as.record; break;
        case TURBOWASM_COMPONENT_TYPE_TUPLE: sequence = &value->as.tuple; break;
        case TURBOWASM_COMPONENT_TYPE_VARIANT: variant = &value->as.variant; break;
        case TURBOWASM_COMPONENT_TYPE_OPTION: variant = &value->as.option; break;
        case TURBOWASM_COMPONENT_TYPE_RESULT: variant = &value->as.result; break;
        default:
            return value->kind >= TURBOWASM_COMPONENT_TYPE_BOOL && value->kind <= TURBOWASM_COMPONENT_TYPE_FLAGS
                ? TURBOWASM_OK : TURBOWASM_TYPE_MISMATCH;
    }
    if (sequence != NULL) {
        if (sequence->count > SIZE_MAX || (sequence->count != 0u && sequence->items == NULL))
            return TURBOWASM_INVALID_ARGUMENT;
        status = size_add(bytes, (size_t)sequence->count, sizeof(*sequence->items), limit);
        for (i = 0u; status == TURBOWASM_OK && i < sequence->count; ++i)
            status = payload_size(&sequence->items[i], depth + 1u, bytes, limit);
        return status;
    }
    if (variant->payload == NULL) return TURBOWASM_OK;
    status = size_add(bytes, 1u, sizeof(*variant->payload), limit);
    return status == TURBOWASM_OK ? payload_size(variant->payload, depth + 1u, bytes, limit) : status;
}

static turbowasm_status receive_batch(void *context, const turbowasm_component_value *values, uint32_t count) {
    component_host_transfer_impl *impl = context;
    size_t bytes = impl->payload_used;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t i;
    for (i = 0u; status == TURBOWASM_OK && i < count; ++i) {
        status = payload_size(&values[i], 0u, &bytes, impl->payload_capacity);
        if (status == TURBOWASM_OK)
            status = turbowasm_component_canonical_validate_value(impl->graph, impl->payload, &values[i]);
    }
    if (status == TURBOWASM_OK) impl->payload_used = bytes;
    return status;
}

static turbowasm_status start_transfer(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *source, turbowasm_component_value *values,
    uint32_t count, size_t capacity, bool readable, turbowasm_component_host_budget *budget,
    turbowasm_component_task *driver) {
    component_host_endpoint_impl *body = source != NULL ? source->impl : NULL;
    component_host_transfer_impl *impl = NULL;
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_endpoint *end;
    turbowasm_runtime_scope scope;
    void *origin = NULL;
    void (*origin_release)(void *) = NULL;
    turbowasm_status status;
    size_t bytes = sizeof(*impl), available, payload_bytes = 0u;
    uint32_t i;
    bool cells_moved = false;
    if (owner == NULL || owner->impl != NULL || (void *)owner == (void *)source || body == NULL || body->driving ||
        budget == NULL || budget->limit == 0u || budget->limit == SIZE_MAX || budget->used > budget->limit ||
        capacity == SIZE_MAX || count > TURBOWASM_COMPONENT_COPY_MAX_LENGTH) return TURBOWASM_INVALID_ARGUMENT;
    end = body->endpoint; instance = body->instance;
    if (end->readable != readable || end->closed || end->value_owner != NULL || end->lower_scope != NULL ||
        end->operation != NULL || end->waitable.delivering || end->waitable.sync_waiter ||
        end->waitable.set_handle != 0u || end->waitable.state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE ||
        (end->waitable.state.endpoint.future && count != 1u) ||
        (!readable && end->has_payload && count != 0u && values == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    if (end->failure != TURBOWASM_OK) return end->failure;
    if (instance->host_transfer_limit == 0u || instance->host_transfer_count >= instance->host_transfer_limit)
        return TURBOWASM_OUT_OF_MEMORY;
    available = budget->limit - budget->used;
    status = size_add(&bytes, end->has_payload ? count : 0u, sizeof(*values), available);
    if (status != TURBOWASM_OK) return status;
    if (!readable && end->has_payload) {
        for (i = 0u; i < count; ++i) {
            status = payload_size(&values[i], 0u, &payload_bytes, available - bytes);
            if (status == TURBOWASM_OK)
                status = turbowasm_component_canonical_validate_value(end->graph, end->payload, &values[i]);
            if (status != TURBOWASM_OK) return status;
        }
        capacity = payload_bytes;
    }
    status = size_add(&bytes, capacity, 1u, available);
    if (status != TURBOWASM_OK) return status;
    body->driving = true;
    status = turbowasm_component_endpoint_domain_pair_acquire(end, &origin, &origin_release);
    if (status != TURBOWASM_OK) { body->driving = false; return status; }
    if (!turbowasm_component_instance_public_impl_retain(instance)) {
        body->driving = false; origin_release(origin); return TURBOWASM_INVALID_ARGUMENT;
    }
    ++instance->host_transfer_count; budget->used += bytes;
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    impl->instance = instance; impl->origin = origin; impl->origin_release = origin_release;
    impl->budget = budget; impl->bytes = bytes; impl->payload_capacity = capacity;
    impl->payload_used = payload_bytes; impl->readable = readable; impl->driving = true;
    impl->graph = end->graph; impl->payload = end->payload; impl->buffer.length = count;
    owner->impl = impl;
    if (end->has_payload && count != 0u) {
        impl->buffer.values = turbowasm_rt_calloc(count, sizeof(*values));
        if (impl->buffer.values == NULL) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    }
    if (readable) { impl->buffer.receive = receive_batch; impl->buffer.receive_context = impl; }
    if (!readable && end->has_payload) {
        for (i = 0u; i < count; ++i) {
            impl->buffer.values[i] = values[i]; memset(&values[i], 0, sizeof(*values));
        }
        cells_moved = true;
    }
    status = turbowasm_component_endpoint_submit_from_task(end, &impl->buffer, driver);
    if (status != TURBOWASM_OK) goto fail;
    impl->endpoint.impl = body; source->impl = NULL;
    impl->driving = body->driving = false;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;
fail:
    if (cells_moved) {
        for (i = 0u; i < count; ++i) values[i] = impl->buffer.values[i];
    }
    owner->impl = NULL;
    if (impl != NULL) turbowasm_rt_free(impl->buffer.values);
    turbowasm_rt_free(impl);
    budget->used -= bytes; --instance->host_transfer_count;
    turbowasm_runtime_scope_leave(scope);
    body->driving = false;
    origin_release(origin);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}

turbowasm_status turbowasm_component_host_transfer_read(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *endpoint, uint32_t count, size_t payload_capacity,
    turbowasm_component_host_budget *budget, turbowasm_component_task *driver) {
    return start_transfer(owner, endpoint, NULL, count, payload_capacity, true, budget, driver);
}
turbowasm_status turbowasm_component_host_transfer_write_move(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *endpoint, turbowasm_component_value *values, uint32_t count,
    turbowasm_component_host_budget *budget, turbowasm_component_task *driver) {
    return start_transfer(owner, endpoint, values, count, 0u, false, budget, driver);
}

turbowasm_status turbowasm_component_host_transfer_poll(turbowasm_component_host_transfer *owner,
    turbowasm_component_event *event) {
    component_host_transfer_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || transfer_driving(impl) || impl->terminal || event == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_host_endpoint_take(&impl->endpoint, event);
    if (!impl->buffer.leased) {
        impl->terminal = true; impl->status = status;
        if (status == TURBOWASM_OK) impl->event = *event;
    }
    impl->driving = false;
    return status;
}
turbowasm_status turbowasm_component_host_transfer_cancel(turbowasm_component_host_transfer *owner) {
    component_host_transfer_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_status status;
    if (impl == NULL || transfer_driving(impl) || impl->terminal) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = turbowasm_component_host_endpoint_cancel(&impl->endpoint);
    impl->driving = false;
    return status;
}
turbowasm_status turbowasm_component_host_transfer_state_get(const turbowasm_component_host_transfer *owner,
    turbowasm_component_host_transfer_state *out) {
    const component_host_transfer_impl *impl = owner != NULL ? owner->impl : NULL;
    if (impl == NULL || transfer_driving(impl) || out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    *out = (turbowasm_component_host_transfer_state){impl->buffer.length, impl->buffer.progress,
        impl->status, impl->readable, impl->terminal, impl->event};
    return TURBOWASM_OK;
}
const turbowasm_component_value *turbowasm_component_host_transfer_values(
    const turbowasm_component_host_transfer *owner, uint32_t *count) {
    const component_host_transfer_impl *impl = owner != NULL ? owner->impl : NULL;
    uint32_t offset, length;
    if (impl == NULL || transfer_driving(impl) || !impl->terminal || count == NULL) return NULL;
    offset = impl->readable ? 0u : impl->buffer.progress;
    length = impl->readable ? impl->buffer.progress : impl->buffer.length - offset;
    *count = length;
    return impl->buffer.values != NULL ? impl->buffer.values + offset : NULL;
}
turbowasm_status turbowasm_component_host_transfer_take_endpoint(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *out) {
    component_host_transfer_impl *impl = owner != NULL ? owner->impl : NULL;
    if (impl == NULL || transfer_driving(impl) || out == NULL || out->impl != NULL || (void *)owner == (void *)out ||
        impl->endpoint.impl == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (!impl->terminal) return TURBOWASM_TRAPPED;
    *out = impl->endpoint; impl->endpoint.impl = NULL;
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_host_transfer_destroy(turbowasm_component_host_transfer *owner) {
    component_host_transfer_impl *impl;
    turbowasm_component_instance_public_impl *instance;
    turbowasm_runtime_scope scope;
    turbowasm_status status, cleanup;
    void *origin;
    void (*origin_release)(void *);
    uint32_t i;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl = owner->impl;
    if (impl == NULL) return TURBOWASM_OK;
    if (transfer_driving(impl)) return TURBOWASM_INVALID_ARGUMENT;
    if (!impl->terminal) return TURBOWASM_TRAPPED;
    impl->driving = true;
    instance = impl->instance; origin = impl->origin; origin_release = impl->origin_release;
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    cleanup = turbowasm_component_host_endpoint_destroy(&impl->endpoint);
    if (cleanup != TURBOWASM_OK) {
        impl->driving = false; turbowasm_runtime_scope_leave(scope); return cleanup;
    }
    status = impl->status;
    if (impl->buffer.values != NULL) {
        for (i = 0u; i < impl->buffer.length; ++i) {
            cleanup = turbowasm_component_value_destroy(&impl->buffer.values[i]);
            if (status == TURBOWASM_OK) status = cleanup;
        }
    }
    turbowasm_rt_free(impl->buffer.values);
    impl->budget->used -= impl->bytes; --instance->host_transfer_count;
    owner->impl = NULL; turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    origin_release(origin);
    turbowasm_component_instance_public_impl_release(instance);
    return status;
}
