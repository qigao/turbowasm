#include "validate_data.h"
#include "validate_type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_VAL_I32 = 0x7f,
    TURBOWASM_VAL_I64 = 0x7e,
    TURBOWASM_VAL_F32 = 0x7d,
    TURBOWASM_VAL_F64 = 0x7c,
    TURBOWASM_VAL_V128 = 0x7b,
    TURBOWASM_VAL_FUNCREF = 0x70,
    TURBOWASM_VAL_EXTERNREF = 0x6f
};

typedef struct turbowasm_global_type {
    turbowasm_validation_value_type semantic_type;
    bool is_mutable;
} turbowasm_global_type;

static turbowasm_status turbowasm_read_global_type(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_global_type *out) {
    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_validation_read_globaltype(
        reader, context, &out->semantic_type, &out->is_mutable);
}

static bool turbowasm_function_index_valid(
    uint32_t index,
    const turbowasm_validation_context *context) {
    return context != NULL && index < context->function_count;
}

static bool turbowasm_memory_index_valid(
    uint32_t index,
    const turbowasm_validation_context *context) {
    return context != NULL && index < context->memory_count;
}

typedef struct turbowasm_const_type_stack {
    turbowasm_validation_value_type *values;
    uint32_t size;
    uint32_t capacity;
} turbowasm_const_type_stack;

static bool turbowasm_const_type_stack_reserve(
    turbowasm_const_type_stack *stack,
    uint32_t required) {
    uint32_t next;
    turbowasm_validation_value_type *grown;

    if (stack == NULL)
        return false;
    if (required <= stack->capacity)
        return true;

    next = stack->capacity == 0u ? 4u : stack->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * sizeof(*grown) > (uint64_t)SIZE_MAX)
        return false;
    grown = (turbowasm_validation_value_type *)realloc(
        stack->values, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    stack->values = grown;
    stack->capacity = next;
    return true;
}

static turbowasm_status turbowasm_const_type_stack_push(
    turbowasm_const_type_stack *stack,
    turbowasm_validation_value_type type) {
    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_const_type_stack_reserve(
            stack, stack->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;

    stack->values[stack->size++] = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_const_type_stack_pop(
    turbowasm_const_type_stack *stack,
    uint8_t carrier) {
    turbowasm_validation_value_type expected =
        turbowasm_validation_value_type_legacy(carrier);

    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    --stack->size;
    return turbowasm_validation_value_type_matches(
        &stack->values[stack->size], &expected)
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

turbowasm_status turbowasm_validate_const_expr_semantic(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    turbowasm_validation_value_type *out_type) {
    turbowasm_const_type_stack stack = {0};
    turbowasm_status status = TURBOWASM_OK;

    if (reader == NULL || context == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (;;) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(reader, &opcode)) {
            status = TURBOWASM_MALFORMED_MODULE;
            break;
        }
        if (opcode == 0x0bu)
            break;

        switch (opcode) {
            case 0x41u: {
                int32_t value;
                if (!turbowasm_reader_sleb32(reader, &value)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                (void)value;
                status = turbowasm_const_type_stack_push(
                    &stack,
                    turbowasm_validation_value_type_legacy(
                        TURBOWASM_VAL_I32));
                break;
            }
            case 0x42u: {
                int64_t value;
                if (!turbowasm_reader_sleb64(reader, &value)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                (void)value;
                status = turbowasm_const_type_stack_push(
                    &stack,
                    turbowasm_validation_value_type_legacy(
                        TURBOWASM_VAL_I64));
                break;
            }
            case 0x43u: {
                turbowasm_reader bytes;
                if (!turbowasm_reader_slice(reader, 4u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_type_stack_push(
                    &stack,
                    turbowasm_validation_value_type_legacy(
                        TURBOWASM_VAL_F32));
                break;
            }
            case 0x44u: {
                turbowasm_reader bytes;
                if (!turbowasm_reader_slice(reader, 8u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_type_stack_push(
                    &stack,
                    turbowasm_validation_value_type_legacy(
                        TURBOWASM_VAL_F64));
                break;
            }
            case 0x6au: /* i32.add */
            case 0x6bu: /* i32.sub */
            case 0x6cu: /* i32.mul */
                status = turbowasm_const_type_stack_pop(
                    &stack, TURBOWASM_VAL_I32);
                if (status == TURBOWASM_OK)
                    status = turbowasm_const_type_stack_pop(
                        &stack, TURBOWASM_VAL_I32);
                if (status == TURBOWASM_OK)
                    status = turbowasm_const_type_stack_push(
                        &stack,
                        turbowasm_validation_value_type_legacy(
                            TURBOWASM_VAL_I32));
                break;
            case 0x7cu: /* i64.add */
            case 0x7du: /* i64.sub */
            case 0x7eu: /* i64.mul */
                status = turbowasm_const_type_stack_pop(
                    &stack, TURBOWASM_VAL_I64);
                if (status == TURBOWASM_OK)
                    status = turbowasm_const_type_stack_pop(
                        &stack, TURBOWASM_VAL_I64);
                if (status == TURBOWASM_OK)
                    status = turbowasm_const_type_stack_push(
                        &stack,
                        turbowasm_validation_value_type_legacy(
                            TURBOWASM_VAL_I64));
                break;
            case 0xd0u: {
                turbowasm_validation_value_type type;

                status = turbowasm_validation_read_heaptype(
                    reader, &type);
                if (status != TURBOWASM_OK)
                    break;
                if (type.heap_kind ==
                        TURBOWASM_VALIDATION_HEAP_TYPE_INDEX &&
                    type.type_index >= context->type_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                type.nullable = true;
                status = turbowasm_const_type_stack_push(
                    &stack, type);
                break;
            }
            case 0xd2u: {
                uint32_t function_index;
                const turbowasm_validation_function *function;
                turbowasm_validation_value_type type = {0};

                if (!turbowasm_reader_uleb32(
                        reader, &function_index) ||
                    !turbowasm_function_index_valid(
                        function_index, context)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                if (!turbowasm_validation_context_declare_function_ref(
                        context, function_index)) {
                    status = TURBOWASM_OUT_OF_MEMORY;
                    break;
                }

                function = turbowasm_validation_context_function(
                    context, function_index);
                if (function == NULL) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }

                type.carrier = TURBOWASM_VAL_FUNCREF;
                type.is_reference = true;
                type.nullable = false;
                type.heap_kind =
                    TURBOWASM_VALIDATION_HEAP_TYPE_INDEX;
                type.type_index = function->type_index;
                status = turbowasm_const_type_stack_push(
                    &stack, type);
                break;
            }
            case 0xfdu: {
                uint32_t subopcode;
                turbowasm_reader bytes;

                if (!turbowasm_reader_uleb32(
                        reader, &subopcode)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                if (subopcode != 0x0cu) {
                    status = TURBOWASM_UNSUPPORTED;
                    break;
                }
                if (!turbowasm_reader_slice(
                        reader, 16u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_type_stack_push(
                    &stack,
                    turbowasm_validation_value_type_legacy(
                        TURBOWASM_VAL_V128));
                break;
            }
            case 0x23u: {
                uint32_t global_index;
                const turbowasm_validation_global *global;

                if (!turbowasm_reader_uleb32(
                        reader, &global_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                global = turbowasm_validation_context_global(
                    context, global_index);
                if (global == NULL || global->mutable_value) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_type_stack_push(
                    &stack, global->semantic_type);
                break;
            }
            default:
                status = TURBOWASM_UNSUPPORTED;
                break;
        }

        if (status != TURBOWASM_OK)
            break;
    }

    if (status == TURBOWASM_OK) {
        if (stack.size != 1u) {
            status = TURBOWASM_MALFORMED_MODULE;
        } else {
            *out_type = stack.values[0];
        }
    }

    free(stack.values);
    return status;
}

turbowasm_status turbowasm_validate_const_expr(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    uint8_t *out_type) {
    turbowasm_validation_value_type type;
    turbowasm_status status;

    if (out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_validate_const_expr_semantic(
        reader, context, &type);
    if (status == TURBOWASM_OK)
        *out_type = type.carrier;
    return status;
}

turbowasm_status turbowasm_validate_global_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_global_type type;
        turbowasm_validation_value_type expression_type;
        const uint8_t *initializer_start;
        size_t initializer_size;
        turbowasm_status status = turbowasm_read_global_type(
            section, context, &type);

        if (status != TURBOWASM_OK)
            return status;
        initializer_start = section->cursor;
        status = turbowasm_validate_const_expr_semantic(
            section, context, &expression_type);
        if (status != TURBOWASM_OK)
            return status;
        initializer_size = (size_t)(section->cursor - initializer_start);
        if (initializer_size > UINT32_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        if (!turbowasm_validation_value_type_matches(
                &expression_type, &type.semantic_type))
            return TURBOWASM_MALFORMED_MODULE;
        if (!turbowasm_validation_context_append_global_semantic(
                context, type.semantic_type, type.is_mutable, false,
                initializer_start, (uint32_t)initializer_size))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->global_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_byte_vector(
    turbowasm_reader *reader,
    const uint8_t **out_bytes,
    uint32_t *out_size) {
    uint32_t size;
    turbowasm_reader bytes;

    if (out_bytes == NULL || out_size == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &size) ||
        !turbowasm_reader_slice(reader, size, &bytes))
        return TURBOWASM_MALFORMED_MODULE;

    *out_bytes = bytes.cursor;
    *out_size = size;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_i32_offset_expr(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    turbowasm_validation_expr_span *out) {
    const uint8_t *start;
    size_t size;
    uint8_t type;
    turbowasm_status status;

    if (reader == NULL || context == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    start = reader->cursor;
    status = turbowasm_validate_const_expr(
        reader, context, &type);
    if (status != TURBOWASM_OK)
        return status;
    if (type != TURBOWASM_VAL_I32)
        return TURBOWASM_MALFORMED_MODULE;

    size = (size_t)(reader->cursor - start);
    if (size > UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    out->bytes = start;
    out->size = (uint32_t)size;
    out->result_type = type;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_data_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        uint32_t mode;
        turbowasm_validation_data_segment segment = {0};
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &mode))
            return TURBOWASM_MALFORMED_MODULE;

        switch (mode) {
            case 0u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_ACTIVE;
                segment.memory_index = 0u;
                if (!turbowasm_memory_index_valid(0u, context))
                    return TURBOWASM_MALFORMED_MODULE;
                status = turbowasm_read_i32_offset_expr(
                    section, context, &segment.offset);
                if (status != TURBOWASM_OK) return status;
                break;
            case 1u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_PASSIVE;
                break;
            case 2u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_ACTIVE;
                if (!turbowasm_reader_uleb32(
                        section, &segment.memory_index))
                    return TURBOWASM_MALFORMED_MODULE;
                if (!turbowasm_memory_index_valid(
                        segment.memory_index, context))
                    return TURBOWASM_MALFORMED_MODULE;
                status = turbowasm_read_i32_offset_expr(
                    section, context, &segment.offset);
                if (status != TURBOWASM_OK) return status;
                break;
            default:
                return TURBOWASM_UNSUPPORTED;
        }

        status = turbowasm_read_byte_vector(
            section, &segment.data, &segment.data_size);
        if (status != TURBOWASM_OK)
            return status;

        if (!turbowasm_validation_context_append_data_segment(
                context, segment))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->data_segment_count = count;
    if (summary->has_data_count &&
        summary->data_count != summary->data_segment_count)
        return TURBOWASM_MALFORMED_MODULE;

    return TURBOWASM_OK;
}
