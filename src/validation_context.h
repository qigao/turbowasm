#ifndef TURBOWASM_VALIDATION_CONTEXT_H
#define TURBOWASM_VALIDATION_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct turbowasm_validation_func_type {
    uint8_t *params;
    uint32_t param_count;
    uint8_t *results;
    uint32_t result_count;
} turbowasm_validation_func_type;

typedef struct turbowasm_validation_function {
    uint32_t type_index;
    bool imported;
} turbowasm_validation_function;

typedef struct turbowasm_validation_global {
    uint8_t value_type;
    bool mutable_value;
    bool imported;
} turbowasm_validation_global;

typedef struct turbowasm_validation_table {
    uint8_t reference_type;
    bool imported;
} turbowasm_validation_table;

typedef struct turbowasm_validation_memory {
    bool imported;
} turbowasm_validation_memory;

typedef struct turbowasm_validation_context {
    turbowasm_validation_func_type *types;
    uint32_t type_count;

    turbowasm_validation_function *functions;
    uint32_t function_count;
    uint32_t function_capacity;

    turbowasm_validation_global *globals;
    uint32_t global_count;
    uint32_t global_capacity;

    turbowasm_validation_table *tables;
    uint32_t table_count;
    uint32_t table_capacity;

    turbowasm_validation_memory *memories;
    uint32_t memory_count;
    uint32_t memory_capacity;
} turbowasm_validation_context;

void turbowasm_validation_context_destroy(
    turbowasm_validation_context *context);

bool turbowasm_validation_context_allocate_types(
    turbowasm_validation_context *context,
    uint32_t count);

bool turbowasm_validation_context_define_type(
    turbowasm_validation_context *context,
    uint32_t index,
    uint32_t param_count,
    uint32_t result_count);

turbowasm_validation_func_type *
turbowasm_validation_context_type_mut(
    turbowasm_validation_context *context,
    uint32_t index);

const turbowasm_validation_func_type *
turbowasm_validation_context_type(
    const turbowasm_validation_context *context,
    uint32_t index);

bool turbowasm_validation_context_append_function(
    turbowasm_validation_context *context,
    uint32_t type_index,
    bool imported);

bool turbowasm_validation_context_append_global(
    turbowasm_validation_context *context,
    uint8_t value_type,
    bool mutable_value,
    bool imported);

bool turbowasm_validation_context_append_table(
    turbowasm_validation_context *context,
    uint8_t reference_type,
    bool imported);

bool turbowasm_validation_context_append_memory(
    turbowasm_validation_context *context,
    bool imported);

const turbowasm_validation_func_type *
turbowasm_validation_context_function_type(
    const turbowasm_validation_context *context,
    uint32_t function_index);

const turbowasm_validation_global *
turbowasm_validation_context_global(
    const turbowasm_validation_context *context,
    uint32_t global_index);

#endif /* TURBOWASM_VALIDATION_CONTEXT_H */
