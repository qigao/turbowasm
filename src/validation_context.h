#ifndef TURBOWASM_VALIDATION_CONTEXT_H
#define TURBOWASM_VALIDATION_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct turbowasm_validation_func_type {
    bool defined;
    uint8_t *params;
    uint32_t param_count;
    uint8_t *results;
    uint32_t result_count;
} turbowasm_validation_func_type;

typedef enum turbowasm_validation_control_kind {
    TURBOWASM_VALIDATION_CONTROL_BLOCK = 1,
    TURBOWASM_VALIDATION_CONTROL_LOOP,
    TURBOWASM_VALIDATION_CONTROL_IF
} turbowasm_validation_control_kind;

typedef struct turbowasm_validation_control {
    turbowasm_validation_control_kind kind;
    uint32_t opcode_offset;
    uint32_t body_offset;
    uint32_t else_offset;
    uint32_t end_offset;
} turbowasm_validation_control;

typedef struct turbowasm_validation_function {
    uint32_t type_index;
    bool imported;

    uint8_t *local_types;
    uint32_t local_count;

    const uint8_t *code;
    uint32_t code_size;

    turbowasm_validation_control *controls;
    uint32_t control_count;
    uint32_t control_capacity;
} turbowasm_validation_function;

typedef struct turbowasm_validation_global {
    uint8_t value_type;
    bool mutable_value;
    bool imported;
} turbowasm_validation_global;

typedef struct turbowasm_validation_limits {
    uint32_t minimum;
    uint32_t maximum;
    bool has_maximum;
} turbowasm_validation_limits;

typedef struct turbowasm_validation_table {
    uint8_t reference_type;
    bool imported;
    turbowasm_validation_limits limits;
} turbowasm_validation_table;

typedef struct turbowasm_validation_memory {
    bool imported;
    turbowasm_validation_limits limits;
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

    uint8_t *declared_refs;
    uint32_t declared_ref_count;

    bool has_data_count;
    uint32_t data_count;
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
    turbowasm_validation_limits limits,
    bool imported);

bool turbowasm_validation_context_append_memory(
    turbowasm_validation_context *context,
    turbowasm_validation_limits limits,
    bool imported);

turbowasm_validation_function *
turbowasm_validation_context_function_mut(
    turbowasm_validation_context *context,
    uint32_t function_index);

const turbowasm_validation_function *
turbowasm_validation_context_function(
    const turbowasm_validation_context *context,
    uint32_t function_index);

bool turbowasm_validation_function_append_control(
    turbowasm_validation_function *function,
    turbowasm_validation_control control,
    uint32_t *out_index);

turbowasm_validation_control *
turbowasm_validation_function_control_mut(
    turbowasm_validation_function *function,
    uint32_t index);

const turbowasm_validation_control *
turbowasm_validation_function_control_at(
    const turbowasm_validation_function *function,
    uint32_t opcode_offset);

const turbowasm_validation_func_type *
turbowasm_validation_context_function_type(
    const turbowasm_validation_context *context,
    uint32_t function_index);

const turbowasm_validation_global *
turbowasm_validation_context_global(
    const turbowasm_validation_context *context,
    uint32_t global_index);

bool turbowasm_validation_context_declare_function_ref(
    turbowasm_validation_context *context,
    uint32_t function_index);

bool turbowasm_validation_context_has_function_ref(
    const turbowasm_validation_context *context,
    uint32_t function_index);

#endif /* TURBOWASM_VALIDATION_CONTEXT_H */
