#ifndef TURBOWASM_INSTANCE_INTERNAL_H
#define TURBOWASM_INSTANCE_INTERNAL_H

#include <turbowasm/instance.h>
#include <turbowasm/link.h>

#include "atomic.h"

#include <salts/thread.h>

#include "module_internal.h"
#include "jit_backend.h"
#include "store_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_WASM_PAGE_SIZE = 65536u,
    TURBOWASM_JIT_TAIL_ARGUMENT_LIMIT = 2u,
    TURBOWASM_MEMORY_WAITER_CAPACITY = 64u
};

typedef struct turbowasm_memory_waiter {
    cmeta_cond_t condition;
    uint64_t address;
    bool active;
    bool notified;
} turbowasm_memory_waiter;

typedef struct turbowasm_instance_memory {
    uint8_t *data;
    cmeta_rwlock_t access_lock;
    cmeta_mutex_t waiter_mutex;
    turbowasm_memory_waiter *waiters;
    uint32_t waiter_count;
    uint32_t waiter_capacity;
    uint64_t pages;
    uint64_t maximum_pages;
    uint32_t page_size;
    size_t resource_max_bytes;
    bool has_maximum;
    bool shared;
    bool memory64;
    bool access_lock_initialized;
    bool waiter_mutex_initialized;
    bool storage_initialized;
} turbowasm_instance_memory;

typedef struct turbowasm_instance_limits {
    uint64_t minimum;
    uint64_t maximum;
    bool has_maximum;
} turbowasm_instance_limits;

struct turbowasm_instance_impl;

typedef struct turbowasm_instance_table_entry {
    turbowasm_value value;
} turbowasm_instance_table_entry;

typedef struct turbowasm_instance_table {
    turbowasm_instance_table_entry *entries;
    uint32_t size;
    uint64_t maximum;
    uint32_t resource_max_elements;
    bool has_maximum;
    uint8_t reference_type;
    bool table64;
    turbowasm_validation_value_type semantic_type;
} turbowasm_instance_table;

typedef struct turbowasm_linked_function {
    struct turbowasm_instance_impl *provider;
    uint32_t function_index;
    turbowasm_host_function_fn host_function;
    void *host_context;
} turbowasm_linked_function;

typedef struct turbowasm_linked_global {
    struct turbowasm_instance_impl *provider;
    uint32_t global_index;
} turbowasm_linked_global;

typedef struct turbowasm_linked_memory {
    struct turbowasm_instance_impl *provider;
    uint32_t memory_index;
} turbowasm_linked_memory;

typedef struct turbowasm_linked_table {
    struct turbowasm_instance_impl *provider;
    uint32_t table_index;
} turbowasm_linked_table;

typedef struct turbowasm_linked_tag {
    struct turbowasm_instance_impl *provider;
    uint32_t tag_index;
} turbowasm_linked_tag;

typedef struct turbowasm_tag_identity {
    struct turbowasm_instance_impl *owner;
    uint32_t tag_index;
} turbowasm_tag_identity;

typedef struct turbowasm_exception {
    turbowasm_tag_identity tag;
    turbowasm_value *payload;
    uint32_t payload_count;
    struct turbowasm_exception *next;
} turbowasm_exception;

typedef turbowasm_status (*turbowasm_execution_suspend_fn)(
    void *context,
    turbowasm_status reason);

typedef turbowasm_status (*turbowasm_execution_host_wait_fn)(
    void *context,
    uintptr_t operation_token,
    turbowasm_host_wait *out_wait,
    int *out_status);

typedef turbowasm_status (*turbowasm_execution_host_wait_complete_fn)(
    void *context,
    turbowasm_host_wait wait,
    int status);

typedef struct turbowasm_jit_execution_control {
    uint64_t fuel_remaining;
    bool fuel_limited;
    turbowasm_interrupt_check_fn should_interrupt;
    void *interrupt_context;

    /*
     * Optional restartable-execution hook. One-shot invoke paths leave this
     * NULL and preserve the historical FUEL_EXHAUSTED / INTERRUPTED statuses.
     * A resumable interpreter may suspend here and return TURBOWASM_OK after
     * it is resumed with a fresh execution budget.
     */
    turbowasm_execution_suspend_fn suspend;
    void *suspend_context;

    /* Only resumable interpreter execution installs this hook. */
    turbowasm_execution_host_wait_fn host_wait;
    turbowasm_execution_host_wait_complete_fn host_wait_complete;
    void *host_wait_context;
} turbowasm_jit_execution_control;

typedef enum turbowasm_jit_function_state_kind {
    TURBOWASM_JIT_INTERPRET = 0,
    TURBOWASM_JIT_INTERPRET_ONLY,
    TURBOWASM_JIT_COMPILED
} turbowasm_jit_function_state_kind;

typedef struct turbowasm_jit_function_state {
    uint32_t call_count;
    turbowasm_jit_function_state_kind state;
    turbowasm_compiled_function compiled;
} turbowasm_jit_function_state;

typedef struct turbowasm_instance_impl {
    const turbowasm_module *module;
    turbowasm_store_impl *store;
    struct turbowasm_instance_impl *store_next;
    const turbowasm_validation_context *store_types;

    turbowasm_linked_function *linked_functions;
    uint32_t linked_function_count;

    turbowasm_linked_global *linked_globals;
    uint32_t linked_global_count;

    turbowasm_linked_memory *linked_memories;
    uint32_t linked_memory_count;

    turbowasm_linked_table *linked_tables;
    uint32_t linked_table_count;

    turbowasm_linked_tag *linked_tags;
    uint32_t linked_tag_count;

    /* Instance-owned exception objects keep exnref values stable until the
     * instance is destroyed. */
    turbowasm_exception *exceptions;
    turbowasm_exception *pending_exception;

    turbowasm_value *globals;
    uint32_t global_count;

    turbowasm_instance_memory *memories;
    uint32_t memory_count;

    turbowasm_instance_table *tables;
    uint32_t table_count;

    uint8_t *data_segment_dropped;
    uint32_t data_segment_count;

    uint8_t *element_segment_dropped;
    uint32_t element_segment_count;
    turbowasm_value **element_values;

    bool jit_backend_attached;
    turbowasm_jit_backend jit_backend;
    turbowasm_jit_function_state *jit_functions;
    uint32_t jit_function_count;
    uint32_t jit_hot_threshold;

    bool jit_artifact_cache_enabled;
    turbowasm_jit_artifact_cache jit_artifact_cache;
    uint8_t jit_artifact_source_sha256[
        TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE];
    uint64_t jit_artifact_validation_fingerprint;
    uint8_t jit_artifact_backend_fingerprint[
        TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE];
} turbowasm_instance_impl;

typedef struct turbowasm_jit_invocation_context {
    turbowasm_instance_impl *instance;
    turbowasm_jit_execution_control *execution;
    uint32_t depth;
    turbowasm_status call_status;
    turbowasm_trap call_trap;

    /*
     * Compiled tail calls unwind generated code before dispatching the next
     * function.  The dispatcher consumes this request in a loop at the same
     * logical depth, so neither TurboWasm call depth nor native generated-code
     * frames grow across a direct tail-call chain.
     */
    bool tail_call_pending;
    uint32_t tail_function_index;
    size_t tail_argument_count;
    turbowasm_value
        tail_arguments[TURBOWASM_JIT_TAIL_ARGUMENT_LIMIT];

    /* Invocation-local v128 temporary frame used by helper-backed JIT
     * lowering. Nested compiled calls save/replace/restore this frame. */
    cmeta_v128 *simd_slots;
    uint32_t simd_slot_count;
} turbowasm_jit_invocation_context;

bool turbowasm_value_matches_semantic(turbowasm_instance_impl *instance,
    const turbowasm_value *value,const turbowasm_validation_value_type *type);
turbowasm_status turbowasm_instance_create_in_store_preserve_failure(
    turbowasm_instance *instance,const turbowasm_module *module,
    const turbowasm_linker *linker,turbowasm_store *store);

turbowasm_status turbowasm_jit_instance_attach_backend(
    turbowasm_instance_impl *instance,
    turbowasm_jit_backend *backend,
    uint32_t hot_threshold);

turbowasm_status turbowasm_jit_instance_attach_backend_with_cache(
    turbowasm_instance_impl *instance,
    turbowasm_jit_backend *backend,
    uint32_t hot_threshold,
    const turbowasm_jit_artifact_cache *cache);

void turbowasm_jit_instance_detach_backend(
    turbowasm_instance_impl *instance);

turbowasm_status turbowasm_jit_execution_checkpoint(
    turbowasm_jit_invocation_context *context);

turbowasm_status turbowasm_jit_request_tail_call(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count);

turbowasm_status turbowasm_jit_direct_call(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_invoke_interpreter_internal(
    turbowasm_instance_impl *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    turbowasm_jit_execution_control *execution);

turbowasm_status turbowasm_instance_state_init(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module);

turbowasm_status turbowasm_instance_tag_identity(
    const turbowasm_instance_impl *instance,
    uint32_t tag_index,
    turbowasm_tag_identity *out);

/*
 * Spec-conformance store path: on an instantiation trap, leave the partially
 * instantiated instance alive so escaped references and imported-store side
 * effects retain their WebAssembly store lifetime. Public create APIs keep
 * their existing fail-closed cleanup behavior.
 */
turbowasm_status turbowasm_instance_create_linked_preserve_failure(
    turbowasm_instance *instance,
    const turbowasm_module *module,
    const struct turbowasm_linker *linker);

/*
 * Runtime-internal sibling instantiation used by WASI threads.
 *
 * The sibling gets fresh defined state but copies the parent's already-resolved
 * import bindings, preserving provider identity for imported memories/tables/
 * globals/tags/functions without retaining or rebuilding a linker.
 */
turbowasm_status turbowasm_instance_create_sibling_internal(
    turbowasm_instance *instance,
    const turbowasm_instance *parent);

void turbowasm_instance_state_destroy(
    turbowasm_instance_impl *instance);

turbowasm_status turbowasm_instance_global_get(
    const turbowasm_instance_impl *instance,
    uint32_t index,
    turbowasm_value *out);

turbowasm_status turbowasm_instance_global_set(
    turbowasm_instance_impl *instance,
    uint32_t index,
    turbowasm_value value);

turbowasm_status turbowasm_instance_memory_storage_init(
    turbowasm_instance_memory *memory,
    bool shared,
    size_t bytes);

void turbowasm_instance_memory_storage_destroy(
    turbowasm_instance_memory *memory);

turbowasm_status turbowasm_instance_memory_shared(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    bool *out_shared);

turbowasm_status turbowasm_instance_memory_read_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    void *out,
    size_t width);

turbowasm_status turbowasm_instance_memory_write_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    const void *source,
    size_t width);

turbowasm_status turbowasm_instance_memory_fill_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t destination,
    uint8_t value,
    uint64_t length);

turbowasm_status turbowasm_instance_memory_copy_bytes(
    turbowasm_instance_impl *instance,
    uint32_t destination_memory,
    uint32_t source_memory,
    uint64_t destination,
    uint64_t source,
    uint64_t length);

turbowasm_status turbowasm_instance_memory_atomic(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    const turbowasm_atomic_descriptor *descriptor,
    uint64_t value,
    uint64_t expected,
    uint64_t replacement,
    uint64_t *out_old,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_memory_wait(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    uint8_t width,
    uint64_t expected,
    int64_t timeout_ns,
    uint32_t *out_result,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_memory_wait_with_interrupt(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    uint8_t width,
    uint64_t expected,
    int64_t timeout_ns,
    turbowasm_interrupt_check_fn should_interrupt,
    void *interrupt_context,
    uint32_t *out_result,
    turbowasm_trap *trap);

/*
 * Wake active wait32/wait64 waiters reachable through this instance without
 * marking them notified. Interruptible waiters re-check their policy and may
 * leave with TURBOWASM_INTERRUPTED; ordinary waits treat this as a spurious
 * condition wake and continue waiting.
 */
void turbowasm_instance_interrupt_waiters(
    turbowasm_instance_impl *instance);

turbowasm_status turbowasm_instance_memory_notify(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    uint32_t count,
    uint32_t *out_woken,
    turbowasm_trap *trap);

turbowasm_status turbowasm_threads_sc_fence(void);

/* Shared semantic boundary for interpreter and native memory lowering. */
turbowasm_status turbowasm_instance_memory_load_value(
    turbowasm_instance_impl *instance, uint32_t memory_index,
    uint64_t address, uint64_t offset, uint8_t opcode,
    turbowasm_value *out_value, turbowasm_trap *trap);
turbowasm_status turbowasm_instance_memory_store_value(
    turbowasm_instance_impl *instance, uint32_t memory_index,
    uint64_t address, uint64_t offset, uint8_t opcode,
    turbowasm_value value, turbowasm_trap *trap);

turbowasm_status turbowasm_instance_memory_bounds(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    uint64_t offset,
    size_t width,
    uint8_t **out);

turbowasm_status turbowasm_instance_memory_size64(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t *out_pages);

turbowasm_status turbowasm_instance_memory_size(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t *out_pages);

turbowasm_status turbowasm_instance_memory_limits(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    turbowasm_instance_limits *out_limits);

turbowasm_status turbowasm_instance_memory_grow64(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t delta_pages,
    uint64_t *out_previous_pages);

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *out_previous_pages);

turbowasm_status turbowasm_instance_table_lookup(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint64_t element_index,
    turbowasm_instance_table_entry *out);

turbowasm_status turbowasm_instance_table_get_value(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint64_t element_index,
    turbowasm_value *out);

turbowasm_status turbowasm_instance_table_set_value(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint64_t element_index,
    turbowasm_value value);

turbowasm_status turbowasm_instance_memory_init(
    turbowasm_instance_impl *instance,
    uint32_t data_index,
    uint32_t memory_index,
    uint64_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_data_drop(
    turbowasm_instance_impl *instance,
    uint32_t data_index);

turbowasm_status turbowasm_instance_memory_copy(
    turbowasm_instance_impl *instance,
    uint32_t destination_memory,
    uint32_t source_memory,
    uint64_t destination,
    uint64_t source,
    uint64_t length);

turbowasm_status turbowasm_instance_memory_fill(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t destination,
    uint8_t value,
    uint64_t length);

turbowasm_status turbowasm_instance_table_init(
    turbowasm_instance_impl *instance,
    uint32_t element_index,
    uint32_t table_index,
    uint64_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_element_drop(
    turbowasm_instance_impl *instance,
    uint32_t element_index);

turbowasm_status turbowasm_instance_table_copy(
    turbowasm_instance_impl *instance,
    uint64_t destination_table,
    uint64_t source_table,
    uint64_t destination,
    uint64_t source,
    uint64_t length);

turbowasm_status turbowasm_instance_table_grow(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    turbowasm_value initial,
    uint32_t delta,
    uint32_t *out_previous_size);

turbowasm_status turbowasm_instance_table_grow_wide(
    turbowasm_instance_impl *instance,uint32_t table_index,
    turbowasm_value initial,uint64_t delta,uint64_t *out_previous_size);

turbowasm_status turbowasm_instance_table_size(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t *out_size);

turbowasm_status turbowasm_instance_table_limits(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    turbowasm_instance_limits *out_limits);

turbowasm_status turbowasm_instance_table_fill(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint64_t destination,
    turbowasm_value value,
    uint64_t length);

#endif /* TURBOWASM_INSTANCE_INTERNAL_H */
