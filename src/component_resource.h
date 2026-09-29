#ifndef TURBOWASM_COMPONENT_RESOURCE_H
#define TURBOWASM_COMPONENT_RESOURCE_H

#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdbool.h>
#include <stdint.h>

enum {
    TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS = 65535u,
    TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION = 4095u,
    TURBOWASM_COMPONENT_RESOURCE_MAX_HANDLE = 0x0fffffffu
};

typedef uint32_t turbowasm_component_resource_handle;

typedef struct turbowasm_component_resource_entry {
    uint64_t resource_identity;
    turbowasm_value rep;
    uint32_t generation;
    uint32_t lend_count;
    bool occupied;
    bool retired;
    bool owned;
} turbowasm_component_resource_entry;

typedef struct turbowasm_component_resource_table {
    turbowasm_component_resource_entry *entries;
    uint32_t capacity;
    uint32_t live_count;
    uint32_t max_entries;
} turbowasm_component_resource_table;

typedef turbowasm_status (*turbowasm_component_resource_destructor_fn)(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep);

bool turbowasm_component_resource_table_init(
    turbowasm_component_resource_table *table,
    uint32_t max_entries);

void turbowasm_component_resource_table_destroy(
    turbowasm_component_resource_table *table);

turbowasm_status turbowasm_component_resource_new_owned(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle);

/*
 * Create a transient non-owned handle. Dropping it never runs a resource
 * destructor; the caller owns the surrounding borrow scope.
 */
turbowasm_status turbowasm_component_resource_new_borrowed(
    turbowasm_component_resource_table *table,
    uint64_t resource_identity,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle);

/*
 * Consume an owned handle without invoking its destructor, returning the
 * representation to the lifting caller. This is the canonical lift-own path.
 */
turbowasm_status turbowasm_component_resource_take_owned(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_value *out_rep);

turbowasm_status turbowasm_component_resource_rep(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_value *out_rep);

turbowasm_status turbowasm_component_resource_lend_acquire(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity);

turbowasm_status turbowasm_component_resource_lend_release(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity);

turbowasm_status turbowasm_component_resource_drop(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    uint64_t expected_resource_identity,
    turbowasm_component_resource_destructor_fn destructor,
    void *destructor_context);

#endif
