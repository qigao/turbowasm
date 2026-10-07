#ifndef TURBOWASM_VALIDATION_CONTEXT_H
#define TURBOWASM_VALIDATION_CONTEXT_H

#include <turbowasm/module.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum turbowasm_validation_heap_kind {
    TURBOWASM_VALIDATION_HEAP_NONE = 0,
    TURBOWASM_VALIDATION_HEAP_FUNC,
    TURBOWASM_VALIDATION_HEAP_EXTERN,
    TURBOWASM_VALIDATION_HEAP_EXN,
    TURBOWASM_VALIDATION_HEAP_NOEXN,
    TURBOWASM_VALIDATION_HEAP_TYPE_INDEX,
    TURBOWASM_VALIDATION_HEAP_NOFUNC,
    TURBOWASM_VALIDATION_HEAP_NOEXTERN,
    TURBOWASM_VALIDATION_HEAP_ANY,
    TURBOWASM_VALIDATION_HEAP_EQ,
    TURBOWASM_VALIDATION_HEAP_I31,
    TURBOWASM_VALIDATION_HEAP_STRUCT,
    TURBOWASM_VALIDATION_HEAP_ARRAY,
    TURBOWASM_VALIDATION_HEAP_BOTTOM
} turbowasm_validation_heap_kind;

typedef struct turbowasm_validation_value_type {
    uint8_t carrier;
    bool is_reference;
    bool nullable;
    turbowasm_validation_heap_kind heap_kind;
    uint32_t type_index;
    const struct turbowasm_validation_func_type *definition;
} turbowasm_validation_value_type;

typedef enum turbowasm_validation_type_kind {
    TURBOWASM_TYPE_FUNCTION = 0,
    TURBOWASM_TYPE_STRUCT,
    TURBOWASM_TYPE_ARRAY
} turbowasm_validation_type_kind;

typedef struct turbowasm_validation_field {
    turbowasm_validation_value_type type;
    uint8_t packed_bits;
    bool mutable_value;
} turbowasm_validation_field;

typedef struct turbowasm_validation_func_type {
    bool defined;
    uint8_t *params;
    turbowasm_validation_value_type *param_semantics;
    uint32_t param_count;
    uint8_t *results;
    turbowasm_validation_value_type *result_semantics;
    uint32_t result_count;
    turbowasm_validation_type_kind kind;
    turbowasm_validation_field *fields;
    uint32_t field_count;
    bool final_type;
    uint32_t super_index;
    const struct turbowasm_validation_func_type *super;
    const struct turbowasm_validation_func_type *group;
    uint32_t group_count;
    uint32_t group_offset;
    uint32_t dependency_depth;
    const struct turbowasm_validation_func_type *canonical;
} turbowasm_validation_func_type;

bool turbowasm_validation_defined_type_equal(
    const turbowasm_validation_func_type *left,
    const turbowasm_validation_func_type *right);
bool turbowasm_validation_defined_type_matches(
    const turbowasm_validation_func_type *actual,
    const turbowasm_validation_func_type *expected);

typedef enum turbowasm_validation_control_kind {
    TURBOWASM_VALIDATION_CONTROL_BLOCK = 1,
    TURBOWASM_VALIDATION_CONTROL_LOOP,
    TURBOWASM_VALIDATION_CONTROL_IF,
    TURBOWASM_VALIDATION_CONTROL_TRY_TABLE
} turbowasm_validation_control_kind;

typedef enum turbowasm_validation_catch_kind {
    TURBOWASM_VALIDATION_CATCH = 0,
    TURBOWASM_VALIDATION_CATCH_REF = 1,
    TURBOWASM_VALIDATION_CATCH_ALL = 2,
    TURBOWASM_VALIDATION_CATCH_ALL_REF = 3
} turbowasm_validation_catch_kind;

typedef struct turbowasm_validation_catch {
    turbowasm_validation_catch_kind kind;
    uint32_t tag_index;
    uint32_t label_depth;
} turbowasm_validation_catch;

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

    /*
     * try_table catches are retained in binary order so the interpreter can
     * later select a handler without reparsing validation-only immediates.
     * Non-try controls use catches == NULL and catch_count == 0.
     */
    turbowasm_validation_catch *catches;
    uint32_t catch_count;
} turbowasm_validation_control;

typedef struct turbowasm_validation_function {
    uint32_t type_index;
    bool imported;

    uint8_t *local_types;
    turbowasm_validation_value_type *local_semantics;
    uint32_t local_count;

    const uint8_t *code;
    uint32_t code_size;

    turbowasm_validation_control *controls;
    uint32_t control_count;
    uint32_t control_capacity;
} turbowasm_validation_function;

typedef struct turbowasm_validation_global {
    uint8_t value_type;
    turbowasm_validation_value_type semantic_type;
    bool mutable_value;
    bool imported;

    const uint8_t *initializer;
    uint32_t initializer_size;
} turbowasm_validation_global;

typedef struct turbowasm_validation_limits {
    uint64_t minimum;
    uint64_t maximum;
    bool has_maximum;
    bool table64;
} turbowasm_validation_limits;

typedef struct turbowasm_validation_table {
    uint8_t reference_type;
    turbowasm_validation_value_type semantic_type;
    bool imported;
    turbowasm_validation_limits limits;

    /*
     * Defined tables may carry an explicit constant initializer. The bytes
     * borrow the loaded module just like global/segment expression spans.
     * Imported tables never have an initializer here.
     */
    const uint8_t *initializer;
    uint32_t initializer_size;
} turbowasm_validation_table;

typedef struct turbowasm_validation_memory_limits {
    uint64_t minimum;
    uint64_t maximum;
    bool has_maximum;
} turbowasm_validation_memory_limits;

typedef struct turbowasm_validation_memory {
    bool imported;
    bool shared;
    bool memory64;
    uint32_t page_size;
    turbowasm_validation_memory_limits limits;
} turbowasm_validation_memory;

typedef struct turbowasm_validation_tag {
    uint32_t type_index;
    bool imported;
} turbowasm_validation_tag;

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
    turbowasm_validation_value_type semantic_type;
    turbowasm_validation_expr_span offset;
    turbowasm_validation_element_item *items;
    uint32_t item_count;
} turbowasm_validation_element_segment;

typedef struct turbowasm_validation_context {
    bool requires_store;
    turbowasm_import_desc *imports;
    uint32_t import_count;
    uint32_t import_capacity;

    turbowasm_export_desc *exports;
    uint32_t export_count;
    uint32_t export_capacity;

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

    turbowasm_validation_tag *tags;
    uint32_t tag_count;
    uint32_t tag_capacity;

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

turbowasm_validation_value_type
turbowasm_validation_value_type_legacy(uint8_t carrier);

bool turbowasm_validation_value_type_equal(
    const turbowasm_validation_value_type *left,
    const turbowasm_validation_value_type *right);

bool turbowasm_validation_value_type_matches(
    const turbowasm_validation_value_type *actual,
    const turbowasm_validation_value_type *expected);

void turbowasm_validation_context_destroy(
    turbowasm_validation_context *context);

bool turbowasm_validation_context_append_import(
    turbowasm_validation_context *context,
    turbowasm_import_desc import_desc);

bool turbowasm_validation_context_append_export(
    turbowasm_validation_context *context,
    turbowasm_export_desc export_desc);

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

bool turbowasm_validation_context_append_global_semantic(
    turbowasm_validation_context *context,
    turbowasm_validation_value_type value_type,
    bool mutable_value,
    bool imported,
    const uint8_t *initializer,
    uint32_t initializer_size);

bool turbowasm_validation_context_append_table(
    turbowasm_validation_context *context,
    uint8_t reference_type,
    turbowasm_validation_limits limits,
    bool imported);

bool turbowasm_validation_context_append_table_semantic(
    turbowasm_validation_context *context,
    turbowasm_validation_value_type reference_type,
    turbowasm_validation_limits limits,
    bool imported);

bool turbowasm_validation_context_append_table_semantic_initialized(
    turbowasm_validation_context *context,
    turbowasm_validation_value_type reference_type,
    turbowasm_validation_limits limits,
    const uint8_t *initializer,
    uint32_t initializer_size);

bool turbowasm_validation_context_append_memory(
    turbowasm_validation_context *context,
    turbowasm_validation_memory_limits limits,
    uint32_t page_size,
    bool shared,
    bool memory64,
    bool imported);

bool turbowasm_validation_context_append_tag(
    turbowasm_validation_context *context,
    uint32_t type_index,
    bool imported);

const turbowasm_validation_tag *
turbowasm_validation_context_tag(
    const turbowasm_validation_context *context,
    uint32_t index);

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

bool turbowasm_validation_func_type_equal(
    const turbowasm_validation_func_type *left,
    const turbowasm_validation_func_type *right);

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
