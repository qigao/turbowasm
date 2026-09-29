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

turbowasm_status turbowasm_component_resource_new_owned(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle) {
    uint32_t index;
    turbowasm_component_resource_entry *entry;

    if (table == NULL || out_handle == NULL ||
        table->max_entries == 0u || resource_identity == 0u)
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

    entry->resource_identity = resource_identity;
    entry->rep = rep;
    entry->lend_count = 0u;
    entry->occupied = true;
    entry->owned = true;
    ++table->live_count;

    *out_handle = handle_encode(index, entry->generation);
    if (*out_handle == 0u) {
        entry->occupied = false;
        --table->live_count;
        return TURBOWASM_INVALID_ARGUMENT;
    }
    return TURBOWASM_OK;
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
    if (entry == NULL)
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
    if (entry == NULL ||
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
    if (entry == NULL ||
        entry->resource_identity != expected_resource_identity ||
        entry->lend_count == 0u)
        return TURBOWASM_TRAPPED;

    --entry->lend_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_component_resource_destructor_fn destructor,
    void *destructor_context) {
    uint32_t index;
    uint32_t generation;
    turbowasm_component_resource_entry *entry;
    turbowasm_value rep;
    turbowasm_status status = TURBOWASM_OK;

    if (table == NULL || expected_resource_identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!handle_decode(handle, &index, &generation) ||
        index >= table->capacity)
        return TURBOWASM_TRAPPED;

    entry = &table->entries[index];
    if (!entry->occupied || entry->generation != generation ||
        entry->resource_identity != expected_resource_identity ||
        entry->lend_count != 0u)
        return TURBOWASM_TRAPPED;

    rep = entry->rep;
    entry->occupied = false;
    entry->resource_identity = 0u;
    memset(&entry->rep, 0, sizeof(entry->rep));
    entry->lend_count = 0u;
    entry->owned = false;
    --table->live_count;

    if (entry->generation ==
        TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION) {
        entry->retired = true;
    } else {
        ++entry->generation;
    }

    if (destructor != NULL)
        status = destructor(
            destructor_context,
            expected_resource_identity,
            rep);
    return status;
}
