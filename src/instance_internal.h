#ifndef TURBOWASM_INSTANCE_INTERNAL_H
#define TURBOWASM_INSTANCE_INTERNAL_H

#include <turbowasm/instance.h>

#include "validation_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_runtime_memory {
    uint8_t *bytes;
    uint32_t pages;
    turbowasm_validation_limits limits;
} turbowasm_runtime_memory;

typedef struct turbowasm_runtime_table {
    uint32_t *entries;
    uint32_t size;
    uint8_t reference_type;
    turbowasm_validation_limits limits;
} turbowasm_runtime_table;

typedef struct turbowasm_instance_impl {
    const turbowasm_module *module;

    turbowasm_value *globals;
    uint32_t global_count;

    turbowasm_runtime_memory *memories;
    uint32_t memory_count;

    turbowasm_runtime_table *tables;
    uint32_t table_count;

    uint8_t *data_dropped;
    uint32_t data_segment_count;

    uint8_t *element_dropped;
    uint32_t element_segment_count;
} turbowasm_instance_impl;

turbowasm_status turbowasm_instance_state_init(
    turbowasm_instance_impl *instance,
    const turbowasm_module *module);

void turbowasm_instance_state_destroy(
    turbowasm_instance_impl *instance);

turbowasm_status turbowasm_instance_global_get(
    const turbowasm_instance_impl *instance,
    uint32_t index,
    turbowasm_value *out);

turbowasm_status turbowasm_instance_global_set(
    turbowasm_instance_impl *instance,
    uint32_t index,
    const turbowasm_value *value);

turbowasm_status turbowasm_instance_memory_read(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    void *destination,
    size_t size,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_memory_write(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    const void *source,
    size_t size,
    turbowasm_trap *trap);

turbowasm_status turbowasm_instance_memory_size(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t *pages);

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *previous_pages);

turbowasm_status turbowasm_instance_table_function(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    uint32_t *function_index,
    turbowasm_trap *trap);

#endif /* TURBOWASM_INSTANCE_INTERNAL_H */
