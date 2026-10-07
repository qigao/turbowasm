#include "component_resource.h"

#include "runtime_alloc.h"

#include <stddef.h>
#include <string.h>

enum {
    TW_RESOURCE_SLOT_BITS = 16u,
    TW_RESOURCE_SLOT_MASK = 0xffffu
};

static bool handle_decode(
    turbowasm_component_resource_handle handle,
    uint32_t *out_index,
    uint32_t *out_generation) {
    uint32_t slot;
    uint32_t generation;

    if (out_index == NULL || out_generation == NULL ||
        handle == 0u ||
        handle > TURBOWASM_COMPONENT_RESOURCE_MAX_HANDLE)
        return false;

    slot = handle & TW_RESOURCE_SLOT_MASK;
    generation = handle >> TW_RESOURCE_SLOT_BITS;
    if (slot == 0u || generation == 0u ||
        generation > TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION)
        return false;

    *out_index = slot - 1u;
    *out_generation = generation;
    return true;
}

static turbowasm_component_resource_handle handle_encode(
    uint32_t index,
    uint32_t generation) {
    uint32_t slot = index + 1u;

    if (slot == 0u || slot > TW_RESOURCE_SLOT_MASK ||
        generation == 0u ||
        generation > TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION)
        return 0u;

    return (generation << TW_RESOURCE_SLOT_BITS) | slot;
}

static turbowasm_component_resource_entry *entry_get(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    uint32_t index;
    uint32_t generation;
    turbowasm_component_resource_entry *entry;

    if (table == NULL ||
        !handle_decode(handle, &index, &generation) ||
        index >= table->capacity)
        return NULL;

    entry = &table->entries[index];
    if (!entry->occupied || entry->generation != generation)
        return NULL;
    return entry;
}

static const turbowasm_component_resource_entry *entry_get_const(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    uint32_t index;
    uint32_t generation;
    const turbowasm_component_resource_entry *entry;

    if (table == NULL ||
        !handle_decode(handle, &index, &generation) ||
        index >= table->capacity)
        return NULL;

    entry = &table->entries[index];
    if (!entry->occupied || entry->generation != generation)
        return NULL;
    return entry;
}

static bool table_grow(
    turbowasm_component_resource_table *table) {
    uint32_t next;
    turbowasm_component_resource_entry *grown;

    if (table == NULL || table->capacity >= table->max_entries)
        return false;

    next = table->capacity == 0u ? 8u : table->capacity * 2u;
    if (next < table->capacity || next > table->max_entries)
        next = table->max_entries;
    if ((size_t)next > SIZE_MAX / sizeof(*grown))
        return false;

    grown = (turbowasm_component_resource_entry *)turbowasm_rt_realloc(
        table->entries, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    memset(
        grown + table->capacity,
        0,
        (size_t)(next - table->capacity) * sizeof(*grown));
    table->entries = grown;
    table->capacity = next;
    return true;
}

bool turbowasm_component_resource_table_init(
    turbowasm_component_resource_table *table,
    uint32_t max_entries) {
    if (table == NULL ||
        table->entries != NULL ||
        table->capacity != 0u ||
        table->live_count != 0u ||
        table->max_entries != 0u)
        return false;

    if (max_entries == 0u)
        max_entries = TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS;
    if (max_entries > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS)
        return false;

    table->max_entries = max_entries;
    return true;
}

void turbowasm_component_resource_table_destroy(
    turbowasm_component_resource_table *table) {
    if (table == NULL)
        return;
    turbowasm_rt_free(table->entries);
    memset(table, 0, sizeof(*table));
}

static turbowasm_status entry_allocate(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle *out_handle,
    turbowasm_component_resource_entry **out_entry) {
    uint32_t index;
    turbowasm_component_resource_entry *entry;

    if (table == NULL || out_handle == NULL ||
        out_entry == NULL || table->max_entries == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    for (;;) {
        for (index = 0u; index < table->capacity; ++index) {
            entry = &table->entries[index];
            if (!entry->occupied && !entry->retired)
                goto found;
        }
        if (!table_grow(table))
            return TURBOWASM_OUT_OF_MEMORY;
    }

found:
    if (entry->generation == 0u)
        entry->generation = 1u;

    entry->occupied = true;
    ++table->live_count;

    *out_handle = handle_encode(index, entry->generation);
    if (*out_handle == 0u) {
        entry->occupied = false;
        entry->owned = false;
        --table->live_count;
        return TURBOWASM_INVALID_ARGUMENT;
    }
    *out_entry = entry;
    return TURBOWASM_OK;
}

static turbowasm_status resource_new(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    bool owned,
    turbowasm_component_resource_handle *out_handle) {
    turbowasm_component_resource_entry *entry;
    turbowasm_status status;
    if (resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    status = entry_allocate(table, out_handle, &entry);
    if (status != TURBOWASM_OK)
        return status;
    entry->kind = TURBOWASM_COMPONENT_HANDLE_RESOURCE;
    entry->resource_identity = resource_identity;
    entry->rep = rep;
    entry->owned = owned;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_handle_insert(
    turbowasm_component_resource_table *table,
    turbowasm_component_handle_kind kind, void *object,
    turbowasm_component_resource_handle *out_handle) {
    turbowasm_component_resource_entry *entry;
    turbowasm_status status;
    if (kind <= TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        kind >= TURBOWASM_COMPONENT_HANDLE_INVALID || object == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = entry_allocate(table, out_handle, &entry);
    if (status != TURBOWASM_OK)
        return status;
    entry->kind = kind;
    entry->object = object;
    return TURBOWASM_OK;
}

turbowasm_component_handle_kind turbowasm_component_handle_kind_get(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle) {
    const turbowasm_component_resource_entry *entry = entry_get_const(table, handle);
    return entry == NULL ? TURBOWASM_COMPONENT_HANDLE_INVALID : entry->kind;
}

turbowasm_status turbowasm_component_resource_publish(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    void *reservation, uint64_t resource_identity, turbowasm_value rep) {
    turbowasm_component_resource_entry *entry = entry_get(table, handle);
    if (reservation == NULL || resource_identity == 0u ||
        (rep.kind != TURBOWASM_VALUE_I32 && rep.kind != TURBOWASM_VALUE_I64))
        return TURBOWASM_INVALID_ARGUMENT;
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION ||
        entry->object != reservation) return TURBOWASM_TRAPPED;
    entry->kind = TURBOWASM_COMPONENT_HANDLE_RESOURCE;
    entry->object = NULL; entry->resource_identity = resource_identity;
    entry->rep = rep; entry->owned = true;
    return TURBOWASM_OK;
}

void *turbowasm_component_handle_object(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_handle_kind kind) {
    const turbowasm_component_resource_entry *entry = entry_get_const(table, handle);
    if (entry == NULL || entry->kind != kind ||
        kind <= TURBOWASM_COMPONENT_HANDLE_RESOURCE || kind >= TURBOWASM_COMPONENT_HANDLE_INVALID)
        return NULL;
    return entry->object;
}

bool turbowasm_component_handle_at(
    const turbowasm_component_resource_table *table, uint32_t index,
    turbowasm_component_resource_handle *out_handle,
    turbowasm_component_handle_kind *out_kind, void **out_object) {
    const turbowasm_component_resource_entry *entry;
    if (table == NULL || index >= table->capacity ||
        out_handle == NULL || out_kind == NULL || out_object == NULL)
        return false;
    entry = &table->entries[index];
    if (!entry->occupied)
        return false;
    *out_handle = handle_encode(index, entry->generation);
    *out_kind = entry->kind;
    *out_object = entry->object;
    return true;
}

turbowasm_status turbowasm_component_resource_new_owned(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle) {
    return resource_new(
        table, resource_identity, rep, true, out_handle);
}

turbowasm_status turbowasm_component_resource_new_borrowed(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle) {
    return resource_new(
        table, resource_identity, rep, false, out_handle);
}

turbowasm_status turbowasm_component_resource_rep(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_value *out_rep) {
    const turbowasm_component_resource_entry *entry =
        entry_get_const(table, handle);

    if (out_rep == NULL || expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE)
        return TURBOWASM_TRAPPED;
    if (entry->resource_identity != expected_resource_identity)
        return TURBOWASM_TRAPPED;

    *out_rep = entry->rep;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_lend_acquire(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity) {
    turbowasm_component_resource_entry *entry =
        entry_get(table, handle);

    if (expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        entry->resource_identity != expected_resource_identity)
        return TURBOWASM_TRAPPED;
    if (entry->lend_count == UINT32_MAX)
        return TURBOWASM_TRAPPED;

    ++entry->lend_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_lend_release(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity) {
    turbowasm_component_resource_entry *entry =
        entry_get(table, handle);

    if (expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        entry->resource_identity != expected_resource_identity ||
        entry->lend_count == 0u)
        return TURBOWASM_TRAPPED;

    --entry->lend_count;
    return TURBOWASM_OK;
}

static void resource_entry_remove(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_entry *entry) {
    if (table == NULL || entry == NULL)
        return;

    entry->occupied = false;
    entry->kind = TURBOWASM_COMPONENT_HANDLE_RESOURCE;
    entry->object = NULL;
    entry->resource_identity = 0u;
    memset(&entry->rep, 0, sizeof(entry->rep));
    entry->lend_count = 0u;
    entry->owned = false;
    if (table->live_count != 0u)
        --table->live_count;

    if (entry->generation ==
        TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION) {
        entry->retired = true;
    } else {
        ++entry->generation;
    }
}

turbowasm_status turbowasm_component_handle_remove(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_handle_kind kind, void **out_object) {
    turbowasm_component_resource_entry *entry;
    if (out_object == NULL || kind <= TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        kind >= TURBOWASM_COMPONENT_HANDLE_INVALID)
        return TURBOWASM_INVALID_ARGUMENT;
    entry = entry_get(table, handle);
    if (entry == NULL || entry->kind != kind)
        return TURBOWASM_TRAPPED;
    *out_object = entry->object;
    resource_entry_remove(table, entry);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_take_owned(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_value *out_rep) {
    turbowasm_component_resource_entry *entry =
        entry_get(table, handle);

    if (table == NULL || out_rep == NULL ||
        expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        entry->resource_identity != expected_resource_identity ||
        !entry->owned ||
        entry->lend_count != 0u)
        return TURBOWASM_TRAPPED;

    *out_rep = entry->rep;
    resource_entry_remove(table, entry);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_component_resource_destructor_fn destructor,
    void *destructor_context) {
    turbowasm_component_resource_entry *entry;
    turbowasm_value rep;
    bool owned;
    turbowasm_status status = TURBOWASM_OK;

    if (table == NULL || expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    entry = entry_get(table, handle);
    if (entry == NULL || entry->kind != TURBOWASM_COMPONENT_HANDLE_RESOURCE ||
        entry->resource_identity != expected_resource_identity ||
        entry->lend_count != 0u)
        return TURBOWASM_TRAPPED;

    rep = entry->rep;
    owned = entry->owned;
    resource_entry_remove(table, entry);

    if (owned && destructor != NULL)
        status = destructor(
            destructor_context,
            expected_resource_identity,
            rep);
    return status;
}
