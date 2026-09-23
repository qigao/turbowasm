#ifndef TURBOWASM_INSTANCE_INTERNAL_H
#define TURBOWASM_INSTANCE_INTERNAL_H

#include <turbowasm/instance.h>

#include "module_internal.h"

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

typedef struct turbowasm_instance_impl {
    const turbowasm_module *module;

    turbowasm_value *globals;
    uint32_t global_count;

    turbowasm_instance_memory *memories;
    uint32_t memory_count;

    turbowasm_instance_table *tables;
    uint32_t table_count;
} turbowasm_instance_impl;

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

#endif /* TURBOWASM_INSTANCE_INTERNAL_H */
