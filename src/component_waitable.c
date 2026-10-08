#include "component_waitable.h"

#include <stddef.h>

static bool waitable_kind(turbowasm_component_handle_kind kind) {
    return kind >= TURBOWASM_COMPONENT_HANDLE_SUBTASK &&
           kind <= TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE;
}

static turbowasm_component_waitable *get_waitable(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_handle_kind kind = turbowasm_component_handle_kind_get(table, handle);
    turbowasm_component_waitable *waitable;
    if (!waitable_kind(kind))
        return NULL;
    waitable = turbowasm_component_handle_object(table, handle, kind);
    return waitable != NULL && waitable->table == table && waitable->handle == handle
        ? waitable : NULL;
}

static turbowasm_component_waitable_set *get_set(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable_set *set = turbowasm_component_handle_object(
        table, handle, TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET);
    return set != NULL && set->table == table && set->handle == handle ? set : NULL;
}

turbowasm_status turbowasm_component_waitable_register(
    turbowasm_component_resource_table *table,
    turbowasm_component_handle_kind kind, turbowasm_component_waitable *waitable) {
    turbowasm_component_resource_handle handle;
    turbowasm_status status;
    if (waitable == NULL || !waitable_kind(kind) || waitable->table != NULL ||
        waitable->handle != 0u || waitable->set_handle != 0u ||
        waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_handle_insert(table, kind, waitable, &handle);
    if (status != TURBOWASM_OK)
        return status;
    waitable->table = table;
    waitable->handle = handle;
    if (kind != TURBOWASM_COMPONENT_HANDLE_SUBTASK)
        waitable->state.endpoint.future = kind == TURBOWASM_COMPONENT_HANDLE_FUTURE_READ ||
                                         kind == TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_set_register(
    turbowasm_component_resource_table *table, turbowasm_component_waitable_set *set) {
    turbowasm_component_resource_handle handle;
    turbowasm_status status;
    if (set == NULL || set->table != NULL || set->handle != 0u ||
        set->wait_count != 0u || set->next_slot != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_handle_insert(table,
        TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET, set, &handle);
    if (status != TURBOWASM_OK)
        return status;
    set->table = table;
    set->handle = handle;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_join(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_resource_handle set_handle) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    if (waitable == NULL || waitable->sync_waiter || waitable->delivering ||
        (set_handle != 0u && get_set(table, set_handle) == NULL))
        return TURBOWASM_TRAPPED;
    waitable->set_handle = set_handle;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    turbowasm_component_handle_kind kind = turbowasm_component_handle_kind_get(table, handle);
    void *object;
    turbowasm_status status;
    if (waitable == NULL || waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_TRAPPED;
    if (kind == TURBOWASM_COMPONENT_HANDLE_SUBTASK) {
        if (!waitable->state.subtask.resolve_delivered)
            return TURBOWASM_TRAPPED;
    } else if (waitable->state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_IDLE &&
               waitable->state.endpoint.phase != TURBOWASM_COMPONENT_ENDPOINT_DONE) {
        return TURBOWASM_TRAPPED;
    }
    status = turbowasm_component_handle_remove(table, handle, kind, &object);
    if (status != TURBOWASM_OK)
        return status;
    waitable->set_handle = 0u;
    waitable->table = NULL;
    waitable->handle = 0u;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_set_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable_set *set = get_set(table, handle);
    uint32_t i;
    void *object;
    turbowasm_status status;
    if (set == NULL || set->wait_count != 0u)
        return TURBOWASM_TRAPPED;
    for (i = 0u; i < table->capacity; ++i) {
        turbowasm_component_resource_handle member;
        turbowasm_component_handle_kind kind;
        if (turbowasm_component_handle_at(table, i, &member, &kind, &object) &&
            waitable_kind(kind) &&
            ((turbowasm_component_waitable *)object)->set_handle == handle)
            return TURBOWASM_TRAPPED;
    }
    status = turbowasm_component_handle_remove(table, handle,
        TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET, &object);
    if (status != TURBOWASM_OK)
        return status;
    set->table = NULL;
    set->handle = 0u;
    set->next_slot = 0u;
    return TURBOWASM_OK;
}

static turbowasm_status take_event(turbowasm_component_waitable *waitable,
    turbowasm_component_handle_kind kind, turbowasm_component_event *out_event) {
    turbowasm_component_event event = {0};
    turbowasm_status status = TURBOWASM_OK;
    bool terminal = false;
    if (waitable->failure != TURBOWASM_OK) return waitable->failure;
    event.handle = waitable->handle;
    if (kind == TURBOWASM_COMPONENT_HANDLE_SUBTASK) {
        turbowasm_component_subtask_phase phase;
        if (!waitable->state.subtask.pending_event)
            return TURBOWASM_YIELDED;
        if (!turbowasm_component_subtask_take_event(&waitable->state.subtask, &phase))
            return TURBOWASM_TRAPPED;
        event.code = TURBOWASM_COMPONENT_EVENT_SUBTASK;
        event.payload = (uint32_t)phase;
        terminal = waitable->state.subtask.resolve_delivered;
    } else {
        if (!waitable->state.endpoint.pending_event)
            return TURBOWASM_YIELDED;
        if (!turbowasm_component_endpoint_take_event(&waitable->state.endpoint, &event.payload))
            return TURBOWASM_TRAPPED;
        switch (kind) {
            case TURBOWASM_COMPONENT_HANDLE_STREAM_READ: event.code = TURBOWASM_COMPONENT_EVENT_STREAM_READ; break;
            case TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE: event.code = TURBOWASM_COMPONENT_EVENT_STREAM_WRITE; break;
            case TURBOWASM_COMPONENT_HANDLE_FUTURE_READ: event.code = TURBOWASM_COMPONENT_EVENT_FUTURE_READ; break;
            default: event.code = TURBOWASM_COMPONENT_EVENT_FUTURE_WRITE; break;
        }
    }
    if ((terminal || kind != TURBOWASM_COMPONENT_HANDLE_SUBTASK) &&
        waitable->release_pending != NULL) {
        waitable->delivering = true;
        status = waitable->release_pending(waitable->release_context);
        waitable->delivering = false;
    }
    if (status == TURBOWASM_OK)
        *out_event = event;
    return status;
}

turbowasm_status turbowasm_component_waitable_take(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle, turbowasm_component_event *out_event) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    if (out_event == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (waitable == NULL || waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_TRAPPED;
    return take_event(waitable, turbowasm_component_handle_kind_get(table, handle), out_event);
}

static bool ready_member(const turbowasm_component_waitable *waitable,
    turbowasm_component_handle_kind kind, turbowasm_component_resource_handle set_handle) {
    return waitable->set_handle == set_handle && !waitable->delivering && !waitable->sync_waiter &&
        (waitable->failure != TURBOWASM_OK ||
         (kind == TURBOWASM_COMPONENT_HANDLE_SUBTASK ? waitable->state.subtask.pending_event
                                                   : waitable->state.endpoint.pending_event));
}

turbowasm_status turbowasm_component_waitable_set_ready(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle set_handle,
    bool *out_ready) {
    uint32_t i;
    if (out_ready == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (get_set(table, set_handle) == NULL) return TURBOWASM_TRAPPED;
    for (i = 0u; i < table->capacity; ++i) {
        turbowasm_component_resource_handle handle;
        turbowasm_component_handle_kind kind;
        void *object;
        if (turbowasm_component_handle_at(table, i, &handle, &kind, &object) &&
            waitable_kind(kind) && ready_member(object, kind, set_handle)) {
            *out_ready = true;
            return TURBOWASM_OK;
        }
    }
    *out_ready = false;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_set_poll(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle set_handle, turbowasm_component_event *out_event) {
    turbowasm_component_waitable_set *set = get_set(table, set_handle);
    uint32_t i, index;
    turbowasm_component_event none = {0};
    if (out_event == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (set == NULL)
        return TURBOWASM_TRAPPED;
    index = set->next_slot < table->capacity ? set->next_slot : 0u;
    for (i = 0u; i < table->capacity; ++i) {
        turbowasm_component_resource_handle handle;
        turbowasm_component_handle_kind kind;
        turbowasm_component_waitable *waitable;
        void *object;
        uint32_t next = index + 1u == table->capacity ? 0u : index + 1u;
        if (turbowasm_component_handle_at(table, index, &handle, &kind, &object) &&
            waitable_kind(kind)) {
            waitable = object;
            if (ready_member(waitable, kind, set_handle)) {
                set->next_slot = next;
                return take_event(waitable, kind, out_event);
            }
        }
        index = next;
    }
    *out_event = none;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_set_wait_acquire(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable_set *set = get_set(table, handle);
    if (set == NULL || set->wait_count == UINT32_MAX)
        return TURBOWASM_TRAPPED;
    ++set->wait_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_set_wait_release(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable_set *set = get_set(table, handle);
    if (set == NULL || set->wait_count == 0u)
        return TURBOWASM_TRAPPED;
    --set->wait_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_wait_begin(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    if (waitable == NULL || waitable->set_handle != 0u || waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_TRAPPED;
    waitable->sync_waiter = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_waitable_wait_end(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    turbowasm_component_event *out_event) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    turbowasm_status status;
    if (out_event == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (waitable == NULL || !waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_TRAPPED;
    status = take_event(waitable, turbowasm_component_handle_kind_get(table, handle), out_event);
    if (status != TURBOWASM_YIELDED)
        waitable->sync_waiter = false;
    return status;
}

turbowasm_status turbowasm_component_waitable_wait_cancel(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle) {
    turbowasm_component_waitable *waitable = get_waitable(table, handle);
    if (waitable == NULL || !waitable->sync_waiter || waitable->delivering)
        return TURBOWASM_TRAPPED;
    waitable->sync_waiter = false;
    return TURBOWASM_OK;
}
