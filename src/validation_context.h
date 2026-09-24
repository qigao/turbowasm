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

    /*
     * Retained validated block signature identity.
     *
     * type_index != UINT32_MAX:
     *   signature is validation_context.types[type_index].
     *
     * type_index == UINT32_MAX && inline_result_type != 0:
     *   no parameters, one inline result type.
     *
     * type_index == UINT32_MAX && inline_result_type == 0:
     *   empty [] -> [] block signature.
     */
    uint32_t type_index;
    uint8_t inline_result_type;
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

    const uint8_t *initializer;
    uint32_t initializer_size;
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

typedef enum turbowasm_validation_segment_mode {
    TURBOWASM_VALIDATION_SEGMENT_ACTIVE = 0,
    TURBOWASM_VALIDATION_SEGMENT_PASSIVE,
    TURBOWASM_VALIDATION_SEGMENT_DECLARATIVE
} turbowasm_validation_segment_mode;

typedef struct turbowasm_validation_expr_span {
    const uint8_t *bytes;
    uint32_t size;
    uint8_t result_type;
} turbowasm_validation_expr_span;

typedef struct turbowasm_validation_data_segment {
    turbowasm_validation_segment_mode mode;
    uint32_t memory_index;
    turbowasm_validation_expr_span offset;
    const uint8_t *data;
    uint32_t data_size;
} turbowasm_validation_data_segment;

typedef enum turbowasm_validation_element_item_kind {
    TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX = 0,
    TURBOWASM_VALIDATION_ELEMENT_CONST_EXPR
} turbowasm_validation_element_item_kind;

typedef struct turbowasm_validation_element_item {
    turbowasm_validation_element_item_kind kind;
    uint32_t function_index;
    turbowasm_validation_expr_span expression;
} turbowasm_validation_element_item;

typedef struct turbowasm_validation_element_segment {
    turbowasm_validation_segment_mode mode;
    uint32_t table_index;
    uint8_t reference_type;
    turbowasm_validation_expr_span offset;
    turbowasm_validation_element_item *items;
    uint32_t item_count;
} turbowasm_validation_element_segment;

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

    turbowasm_validation_data_segment *data_segments;
    uint32_t data_segment_count;
    uint32_t data_segment_capacity;

    turbowasm_validation_element_segment *element_segments;
    uint32_t element_segment_count;
    uint32_t element_segment_capacity;

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
    bool imported,
    const uint8_t *initializer,
    uint32_t initializer_size);

bool turbowasm_validation_context_append_table(
    turbowasm_validation_context *context,
    uint8_t reference_type,
    turbowasm_validation_limits limits,
    bool imported);

bool turbowasm_validation_context_append_memory(
    turbowasm_validation_context *context,
    turbowasm_validation_limits limits,
    bool imported);

bool turbowasm_validation_context_append_data_segment(
    turbowasm_validation_context *context,
    turbowasm_validation_data_segment segment);

bool turbowasm_validation_context_append_element_segment(
    turbowasm_validation_context *context,
    turbowasm_validation_element_segment segment);

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

bool turbowasm_validation_control_signature(
    const turbowasm_validation_context *context,
    const turbowasm_validation_control *control,
    const uint8_t **out_start_types,
    uint32_t *out_start_count,
    const uint8_t **out_end_types,
    uint32_t *out_end_count);

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
