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

typedef enum turbowasm_component_handle_kind {
    TURBOWASM_COMPONENT_HANDLE_RESOURCE = 0,
    TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET,
    TURBOWASM_COMPONENT_HANDLE_SUBTASK,
    TURBOWASM_COMPONENT_HANDLE_STREAM_READ,
    TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE,
    TURBOWASM_COMPONENT_HANDLE_FUTURE_READ,
    TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE,
    TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION,
    TURBOWASM_COMPONENT_HANDLE_INVALID
} turbowasm_component_handle_kind;

typedef struct turbowasm_component_resource_entry {
    turbowasm_component_handle_kind kind;
    void *object;
    uint64_t resource_identity;
    turbowasm_value rep;
    uint32_t generation;
    uint32_t lend_count;
    /* Stable task counter, borrowed until this non-owned handle is removed. */
    uint32_t *borrow_scope;
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

/* Private non-resource slots share the resource handle namespace and quota.
 * Objects are borrowed at stable addresses until removed; the caller owns
 * lifecycle preflight and cleanup. Never retain an entry pointer across growth.
 * Resource APIs cannot access or consume these slots. Failed mutations preserve
 * output and ownership. Remove does not call guest code or release the object. */
turbowasm_status turbowasm_component_handle_insert(
    turbowasm_component_resource_table *table,
    turbowasm_component_handle_kind kind, void *object,
    turbowasm_component_resource_handle *out_handle);
turbowasm_component_handle_kind turbowasm_component_handle_kind_get(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle);
void *turbowasm_component_handle_object(
    const turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_handle_kind kind);
bool turbowasm_component_handle_at(
    const turbowasm_component_resource_table *table, uint32_t slot,
    turbowasm_component_resource_handle *out_handle,
    turbowasm_component_handle_kind *out_kind, void **out_object);
turbowasm_status turbowasm_component_handle_remove(
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_handle handle,
    turbowasm_component_handle_kind kind, void **out_object);

/* Publish a private reservation without allocating or changing its generation.
 * Until publication all ordinary resource operations reject this handle. The
 * reservation object is an identity token, borrowed through commit/rollback. */
turbowasm_status turbowasm_component_resource_publish(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    void *reservation, uint64_t resource_identity, turbowasm_value rep);
turbowasm_status turbowasm_component_resource_publish_borrowed(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    void *reservation, uint64_t resource_identity, turbowasm_value rep, uint32_t *borrow_scope);
/* Preflight all scope handles before removal. Active lends reject unchanged;
 * successful cleanup never invokes a destructor and clears each counter claim. */
turbowasm_status turbowasm_component_resource_scope_clear(
    turbowasm_component_resource_table *table, uint32_t *borrow_scope);

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
