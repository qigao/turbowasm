#ifndef TURBOWASM_INSTANCE_INTERNAL_H
#define TURBOWASM_INSTANCE_INTERNAL_H

#include <turbowasm/instance.h>

#include "module_internal.h"
#include "jit_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_WASM_PAGE_SIZE = 65536u
};

typedef struct turbowasm_instance_memory {
    uint8_t *data;
    uint32_t pages;
    uint32_t maximum_pages;
    bool has_maximum;
} turbowasm_instance_memory;

typedef struct turbowasm_instance_table_entry {
    bool is_null;
    uint32_t function_index;
} turbowasm_instance_table_entry;

typedef struct turbowasm_instance_table {
    turbowasm_instance_table_entry *entries;
    uint32_t size;
    uint32_t maximum;
    bool has_maximum;
    uint8_t reference_type;
} turbowasm_instance_table;

typedef struct turbowasm_jit_execution_control {
    uint64_t fuel_remaining;
    bool fuel_limited;
    turbowasm_interrupt_check_fn should_interrupt;
    void *interrupt_context;
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

    bool jit_backend_attached;
    turbowasm_jit_backend jit_backend;
    turbowasm_jit_function_state *jit_functions;
    uint32_t jit_function_count;
    uint32_t jit_hot_threshold;
} turbowasm_instance_impl;

typedef struct turbowasm_jit_invocation_context {
    turbowasm_instance_impl *instance;
    turbowasm_jit_execution_control *execution;
    uint32_t depth;
    turbowasm_status call_status;
    turbowasm_trap call_trap;
} turbowasm_jit_invocation_context;

turbowasm_status turbowasm_jit_instance_attach_backend(
    turbowasm_instance_impl *instance,
    turbowasm_jit_backend *backend,
    uint32_t hot_threshold);

void turbowasm_jit_instance_detach_backend(
    turbowasm_instance_impl *instance);

turbowasm_status turbowasm_jit_direct_call(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_state_init(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module);

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

turbowasm_status turbowasm_instance_memory_bounds(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    size_t width,
    uint8_t **out);

turbowasm_status turbowasm_instance_memory_size(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t *out_pages);

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *out_previous_pages);

turbowasm_status turbowasm_instance_table_lookup(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_instance_table_entry *out);

turbowasm_status turbowasm_instance_table_get_value(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value *out);

turbowasm_status turbowasm_instance_table_set_value(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value value);

turbowasm_status turbowasm_instance_memory_init(
    turbowasm_instance_impl *instance,
    uint32_t data_index,
    uint32_t memory_index,
    uint32_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_data_drop(
    turbowasm_instance_impl *instance,
    uint32_t data_index);

turbowasm_status turbowasm_instance_memory_copy(
    turbowasm_instance_impl *instance,
    uint32_t destination_memory,
    uint32_t source_memory,
    uint32_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_memory_fill(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t destination,
    uint8_t value,
    uint32_t length);

turbowasm_status turbowasm_instance_table_init(
    turbowasm_instance_impl *instance,
    uint32_t element_index,
    uint32_t table_index,
    uint32_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_element_drop(
    turbowasm_instance_impl *instance,
    uint32_t element_index);

turbowasm_status turbowasm_instance_table_copy(
    turbowasm_instance_impl *instance,
    uint32_t destination_table,
    uint32_t source_table,
    uint32_t destination,
    uint32_t source,
    uint32_t length);

turbowasm_status turbowasm_instance_table_grow(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    turbowasm_value initial,
    uint32_t delta,
    uint32_t *out_previous_size);

turbowasm_status turbowasm_instance_table_size(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t *out_size);

turbowasm_status turbowasm_instance_table_fill(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t destination,
    turbowasm_value value,
    uint32_t length);

#endif /* TURBOWASM_INSTANCE_INTERNAL_H */
