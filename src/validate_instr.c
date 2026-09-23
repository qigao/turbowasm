#include "validate_instr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TW_I32 = 0x7f,
    TW_I64 = 0x7e,
    TW_F32 = 0x7d,
    TW_F64 = 0x7c,
    TW_V128 = 0x7b,
    TW_FUNCREF = 0x70,
    TW_EXTERNREF = 0x6f,
    TW_ANY = 0xff
};

typedef struct turbowasm_type_stack {
    uint8_t *values;
    uint32_t size;
    uint32_t capacity;
    uint32_t floor;
    bool unreachable;
} turbowasm_type_stack;

typedef enum turbowasm_control_kind {
    TURBOWASM_CTRL_FUNCTION = 0,
    TURBOWASM_CTRL_BLOCK,
    TURBOWASM_CTRL_LOOP,
    TURBOWASM_CTRL_IF
} turbowasm_control_kind;

typedef struct turbowasm_control_frame {
    turbowasm_control_kind kind;
    uint32_t height;
    uint32_t parent_floor;
    bool parent_unreachable;
    bool else_seen;
    uint8_t *start_types;
    uint32_t start_count;
    uint8_t *end_types;
    uint32_t end_count;
} turbowasm_control_frame;

typedef struct turbowasm_control_stack {
    turbowasm_control_frame *frames;
    uint32_t size;
    uint32_t capacity;
} turbowasm_control_stack;

static bool turbowasm_instr_valtype(uint8_t type) {
    return type == TW_I32 || type == TW_I64 ||
           type == TW_F32 || type == TW_F64 ||
           type == TW_V128 || type == TW_FUNCREF ||
           type == TW_EXTERNREF;
}


static bool turbowasm_types_equal(
    const uint8_t *left,
    uint32_t left_count,
    const uint8_t *right,
    uint32_t right_count) {
    if (left_count != right_count)
        return false;
    if (left_count == 0u)
        return true;
    return memcmp(left, right, (size_t)left_count) == 0;
}

static bool turbowasm_control_reserve(
    turbowasm_control_stack *controls,
    uint32_t required) {
    uint32_t next;
    turbowasm_control_frame *grown;

    if (required <= controls->capacity)
        return true;

    next = controls->capacity == 0u ? 8u : controls->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * sizeof(*grown) > (uint64_t)SIZE_MAX)
        return false;

    grown = (turbowasm_control_frame *)realloc(
        controls->frames, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    memset(grown + controls->capacity, 0,
           (size_t)(next - controls->capacity) * sizeof(*grown));
    controls->frames = grown;
    controls->capacity = next;
    return true;
}

static bool turbowasm_copy_types(
    const uint8_t *source,
    uint32_t count,
    uint8_t **out) {
    uint8_t *copy = NULL;

    if (out == NULL)
        return false;
    if (count != 0u) {
        copy = (uint8_t *)malloc((size_t)count);
        if (copy == NULL)
            return false;
        memcpy(copy, source, (size_t)count);
    }
    *out = copy;
    return true;
}

static void turbowasm_control_frame_destroy(
    turbowasm_control_frame *frame) {
    if (frame == NULL)
        return;
    free(frame->start_types);
    free(frame->end_types);
    memset(frame, 0, sizeof(*frame));
}

static void turbowasm_control_stack_destroy(
    turbowasm_control_stack *controls) {
    uint32_t index;

    if (controls == NULL)
        return;
    for (index = 0u; index < controls->size; ++index)
        turbowasm_control_frame_destroy(&controls->frames[index]);
    free(controls->frames);
    memset(controls, 0, sizeof(*controls));
}

static turbowasm_control_frame *turbowasm_control_top(
    turbowasm_control_stack *controls) {
    if (controls == NULL || controls->size == 0u)
        return NULL;
    return &controls->frames[controls->size - 1u];
}

static const turbowasm_control_frame *turbowasm_control_target(
    const turbowasm_control_stack *controls,
    uint32_t depth) {
    if (controls == NULL || depth >= controls->size)
        return NULL;
    return &controls->frames[controls->size - 1u - depth];
}

static bool turbowasm_stack_reserve(
    turbowasm_type_stack *stack,
    uint32_t required) {
    uint32_t next;
    uint8_t *grown;

    if (required <= stack->capacity)
        return true;

    next = stack->capacity == 0u ? 8u : stack->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    grown = (uint8_t *)realloc(stack->values, (size_t)next);
    if (grown == NULL)
        return false;

    stack->values = grown;
    stack->capacity = next;
    return true;
}

static turbowasm_status turbowasm_stack_push(
    turbowasm_type_stack *stack,
    uint8_t type) {
    if (stack == NULL || !turbowasm_instr_valtype(type))
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_stack_reserve(stack, stack->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;
    stack->values[stack->size++] = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_pop_any(
    turbowasm_type_stack *stack,
    uint8_t *out_type) {
    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size < stack->floor)
        return TURBOWASM_MALFORMED_MODULE;
    if (stack->size == stack->floor) {
        if (!stack->unreachable)
            return TURBOWASM_MALFORMED_MODULE;
        if (out_type != NULL)
            *out_type = TW_ANY;
        return TURBOWASM_OK;
    }

    --stack->size;
    if (out_type != NULL)
        *out_type = stack->values[stack->size];
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_pop(
    turbowasm_type_stack *stack,
    uint8_t expected) {
    uint8_t actual;
    turbowasm_status status = turbowasm_stack_pop_any(
        stack, &actual);
    if (status != TURBOWASM_OK)
        return status;
    if (actual == TW_ANY)
        return TURBOWASM_OK;
    return actual == expected
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status turbowasm_stack_unary(
    turbowasm_type_stack *stack,
    uint8_t input,
    uint8_t output) {
    turbowasm_status status = turbowasm_stack_pop(stack, input);
    if (status != TURBOWASM_OK)
        return status;
    return turbowasm_stack_push(stack, output);
}

static turbowasm_status turbowasm_stack_binary(
    turbowasm_type_stack *stack,
    uint8_t input,
    uint8_t output) {
    turbowasm_status status = turbowasm_stack_pop(stack, input);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_stack_pop(stack, input);
    if (status != TURBOWASM_OK)
        return status;
    return turbowasm_stack_push(stack, output);
}

static turbowasm_status turbowasm_stack_push_results(
    turbowasm_type_stack *stack,
    const turbowasm_validation_func_type *type) {
    uint32_t index;

    for (index = 0u; index < type->result_count; ++index) {
        turbowasm_status status = turbowasm_stack_push(
            stack, type->results[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_pop_params(
    turbowasm_type_stack *stack,
    const turbowasm_validation_func_type *type) {
    uint32_t index = type->param_count;

    while (index != 0u) {
        turbowasm_status status;
        --index;
        status = turbowasm_stack_pop(
            stack, type->params[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_pop_results(
    turbowasm_type_stack *stack,
    const turbowasm_validation_func_type *type) {
    uint32_t index = type->result_count;

    while (index != 0u) {
        turbowasm_status status;
        --index;
        status = turbowasm_stack_pop(
            stack, type->results[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_result_stack(
    turbowasm_type_stack *stack,
    const turbowasm_validation_func_type *type) {
    turbowasm_status status = turbowasm_pop_results(stack, type);
    if (status != TURBOWASM_OK)
        return status;
    return stack->size == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}


typedef struct turbowasm_block_signature {
    const uint8_t *start_types;
    uint32_t start_count;
    const uint8_t *end_types;
    uint32_t end_count;
    uint8_t inline_end;
} turbowasm_block_signature;

static turbowasm_status turbowasm_stack_pop_types(
    turbowasm_type_stack *stack,
    const uint8_t *types,
    uint32_t count) {
    uint32_t index = count;

    while (index != 0u) {
        turbowasm_status status;
        --index;
        status = turbowasm_stack_pop(stack, types[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_push_types(
    turbowasm_type_stack *stack,
    const uint8_t *types,
    uint32_t count) {
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        turbowasm_status status = turbowasm_stack_push(
            stack, types[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_block_signature(
    turbowasm_reader *body,
    const turbowasm_validation_context *context,
    turbowasm_block_signature *signature) {
    uint8_t first;

    if (body == NULL || context == NULL || signature == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(signature, 0, sizeof(*signature));
    if (!turbowasm_reader_u8(body, &first))
        return TURBOWASM_MALFORMED_MODULE;

    if (first == 0x40u)
        return TURBOWASM_OK;

    if (turbowasm_instr_valtype(first)) {
        signature->inline_end = first;
        signature->end_types = &signature->inline_end;
        signature->end_count = 1u;
        return TURBOWASM_OK;
    }

    {
        uint64_t value = (uint64_t)(first & 0x7fu);
        unsigned shift = 7u;
        unsigned count = 1u;
        uint8_t byte = first;
        int64_t signed_value;
        const turbowasm_validation_func_type *type;

        while ((byte & 0x80u) != 0u) {
            if (count >= 5u || !turbowasm_reader_u8(body, &byte))
                return TURBOWASM_MALFORMED_MODULE;
            value |= (uint64_t)(byte & 0x7fu) << shift;
            shift += 7u;
            ++count;
        }

        if ((byte & 0x40u) != 0u && shift < 64u)
            value |= UINT64_MAX << shift;
        signed_value = (int64_t)value;

        if (signed_value < 0 ||
            (uint64_t)signed_value > UINT32_MAX)
            return TURBOWASM_MALFORMED_MODULE;

        type = turbowasm_validation_context_type(
            context, (uint32_t)signed_value);
        if (type == NULL || !type->defined)
            return TURBOWASM_MALFORMED_MODULE;

        signature->start_types = type->params;
        signature->start_count = type->param_count;
        signature->end_types = type->results;
        signature->end_count = type->result_count;
        return TURBOWASM_OK;
    }
}

static turbowasm_status turbowasm_control_push(
    turbowasm_type_stack *stack,
    turbowasm_control_stack *controls,
    turbowasm_control_kind kind,
    const uint8_t *start_types,
    uint32_t start_count,
    const uint8_t *end_types,
    uint32_t end_count) {
    turbowasm_control_frame frame = {0};
    turbowasm_control_frame *stored;
    turbowasm_status status;

    if (stack == NULL || controls == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (controls->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    if (!turbowasm_copy_types(
            start_types, start_count, &frame.start_types) ||
        !turbowasm_copy_types(
            end_types, end_count, &frame.end_types)) {
        turbowasm_control_frame_destroy(&frame);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    status = turbowasm_stack_pop_types(
        stack, start_types, start_count);
    if (status != TURBOWASM_OK) {
        turbowasm_control_frame_destroy(&frame);
        return status;
    }

    if (!turbowasm_control_reserve(
            controls, controls->size + 1u)) {
        turbowasm_control_frame_destroy(&frame);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    frame.kind = kind;
    frame.height = stack->size;
    frame.parent_floor = stack->floor;
    frame.parent_unreachable = stack->unreachable;
    frame.start_count = start_count;
    frame.end_count = end_count;

    stored = &controls->frames[controls->size++];
    *stored = frame;

    stack->floor = stored->height;
    stack->unreachable = false;
    status = turbowasm_stack_push_types(
        stack, stored->start_types, stored->start_count);
    return status;
}

static void turbowasm_control_mark_unreachable(
    turbowasm_type_stack *stack) {
    stack->size = stack->floor;
    stack->unreachable = true;
}

static turbowasm_status turbowasm_control_validate_end(
    turbowasm_type_stack *stack,
    const turbowasm_control_frame *frame) {
    turbowasm_status status = turbowasm_stack_pop_types(
        stack, frame->end_types, frame->end_count);
    if (status != TURBOWASM_OK)
        return status;
    return stack->size == frame->height
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status turbowasm_control_else(
    turbowasm_type_stack *stack,
    turbowasm_control_stack *controls) {
    turbowasm_control_frame *frame = turbowasm_control_top(controls);
    turbowasm_status status;

    if (frame == NULL ||
        frame->kind != TURBOWASM_CTRL_IF ||
        frame->else_seen)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_control_validate_end(stack, frame);
    if (status != TURBOWASM_OK)
        return status;

    stack->size = frame->height;
    stack->floor = frame->height;
    stack->unreachable = false;
    status = turbowasm_stack_push_types(
        stack, frame->start_types, frame->start_count);
    if (status != TURBOWASM_OK)
        return status;

    frame->else_seen = true;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_control_end(
    turbowasm_type_stack *stack,
    turbowasm_control_stack *controls) {
    turbowasm_control_frame *frame = turbowasm_control_top(controls);
    turbowasm_status status;
    uint32_t parent_floor;
    bool parent_unreachable;
    uint32_t end_count;
    uint8_t *end_types;

    if (frame == NULL || frame->kind == TURBOWASM_CTRL_FUNCTION)
        return TURBOWASM_MALFORMED_MODULE;

    if (frame->kind == TURBOWASM_CTRL_IF &&
        !frame->else_seen &&
        !turbowasm_types_equal(
            frame->start_types, frame->start_count,
            frame->end_types, frame->end_count))
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_control_validate_end(stack, frame);
    if (status != TURBOWASM_OK)
        return status;

    parent_floor = frame->parent_floor;
    parent_unreachable = frame->parent_unreachable;
    end_count = frame->end_count;
    end_types = frame->end_types;
    frame->end_types = NULL;

    stack->size = frame->height;
    stack->floor = parent_floor;
    stack->unreachable = parent_unreachable;

    turbowasm_control_frame_destroy(frame);
    --controls->size;

    status = turbowasm_stack_push_types(
        stack, end_types, end_count);
    free(end_types);
    return status;
}

static turbowasm_status turbowasm_control_branch(
    turbowasm_type_stack *stack,
    const turbowasm_control_stack *controls,
    uint32_t depth,
    bool conditional) {
    const turbowasm_control_frame *target =
        turbowasm_control_target(controls, depth);
    const uint8_t *types;
    uint32_t count;
    turbowasm_status status;

    if (target == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    if (target->kind == TURBOWASM_CTRL_LOOP) {
        types = target->start_types;
        count = target->start_count;
    } else {
        types = target->end_types;
        count = target->end_count;
    }

    status = turbowasm_stack_pop_types(stack, types, count);
    if (status != TURBOWASM_OK)
        return status;

    if (conditional)
        return turbowasm_stack_push_types(stack, types, count);

    turbowasm_control_mark_unreachable(stack);
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_locals(
    turbowasm_reader *body,
    const turbowasm_validation_func_type *function_type,
    uint8_t **out_locals,
    uint32_t *out_count) {
    uint32_t group_count;
    uint32_t group;
    uint64_t total = function_type->param_count;
    uint8_t *locals = NULL;
    uint32_t used = 0u;

    typedef struct local_group {
        uint32_t count;
        uint8_t type;
    } local_group;

    local_group *groups = NULL;

    if (!turbowasm_reader_uleb32(body, &group_count))
        return TURBOWASM_MALFORMED_MODULE;

    if (group_count != 0u) {
        if ((uint64_t)group_count * sizeof(*groups) >
            (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        groups = (local_group *)calloc(
            (size_t)group_count, sizeof(*groups));
        if (groups == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (group = 0u; group < group_count; ++group) {
        if (!turbowasm_reader_uleb32(body, &groups[group].count) ||
            !turbowasm_reader_u8(body, &groups[group].type)) {
            free(groups);
            return TURBOWASM_MALFORMED_MODULE;
        }
        if (!turbowasm_instr_valtype(groups[group].type)) {
            free(groups);
            return TURBOWASM_UNSUPPORTED;
        }
        total += groups[group].count;
        if (total > UINT32_MAX) {
            free(groups);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (total != 0u) {
        locals = (uint8_t *)malloc((size_t)total);
        if (locals == NULL) {
            free(groups);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (function_type->param_count != 0u) {
        memcpy(locals,
               function_type->params,
               (size_t)function_type->param_count);
        used = function_type->param_count;
    }

    for (group = 0u; group < group_count; ++group) {
        uint32_t item;
        for (item = 0u; item < groups[group].count; ++item)
            locals[used++] = groups[group].type;
    }

    free(groups);
    *out_locals = locals;
    *out_count = (uint32_t)total;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_call(
    turbowasm_reader *body,
    turbowasm_type_stack *stack,
    const turbowasm_validation_context *context) {
    uint32_t function_index;
    const turbowasm_validation_func_type *type;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(body, &function_index))
        return TURBOWASM_MALFORMED_MODULE;

    type = turbowasm_validation_context_function_type(
        context, function_index);
    if (type == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_stack_pop_params(stack, type);
    if (status != TURBOWASM_OK)
        return status;
    return turbowasm_stack_push_results(stack, type);
}

static turbowasm_status turbowasm_validate_call_indirect(
    turbowasm_reader *body,
    turbowasm_type_stack *stack,
    const turbowasm_validation_context *context) {
    uint32_t type_index;
    uint32_t table_index;
    const turbowasm_validation_func_type *type;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(body, &type_index) ||
        !turbowasm_reader_uleb32(body, &table_index))
        return TURBOWASM_MALFORMED_MODULE;
    if (table_index >= context->table_count ||
        context->tables[table_index].reference_type != TW_FUNCREF)
        return TURBOWASM_MALFORMED_MODULE;

    type = turbowasm_validation_context_type(context, type_index);
    if (type == NULL || !type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_stack_pop(stack, TW_I32);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_stack_pop_params(stack, type);
    if (status != TURBOWASM_OK)
        return status;
    return turbowasm_stack_push_results(stack, type);
}

static turbowasm_status turbowasm_validate_simd(
    turbowasm_reader *body,
    turbowasm_type_stack *stack) {
    uint32_t subopcode;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(body, &subopcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (subopcode) {
        case 0x0cu: {
            turbowasm_reader bytes;
            if (!turbowasm_reader_slice(body, 16u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            return turbowasm_stack_push(stack, TW_V128);
        }
        case 0xaeu: /* i32x4.add */
        case 0xe6u: /* f32x4.mul */
            status = turbowasm_stack_binary(
                stack, TW_V128, TW_V128);
            return status;
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_validate_function_body(
    turbowasm_reader *body,
    const turbowasm_validation_context *context,
    uint32_t function_index) {
    const turbowasm_validation_func_type *function_type;
    turbowasm_type_stack stack = {0};
    turbowasm_control_stack controls = {0};
    uint8_t *locals = NULL;
    uint32_t local_count = 0u;
    turbowasm_status result;

    if (body == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    function_type = turbowasm_validation_context_function_type(
        context, function_index);
    if (function_type == NULL || !function_type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    result = turbowasm_read_locals(
        body, function_type, &locals, &local_count);
    if (result != TURBOWASM_OK)
        goto done;

    result = turbowasm_control_push(
        &stack, &controls, TURBOWASM_CTRL_FUNCTION,
        NULL, 0u,
        function_type->results, function_type->result_count);
    if (result != TURBOWASM_OK)
        goto done;

    while (turbowasm_reader_remaining(body) != 0u) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(body, &opcode)) {
            result = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }

        switch (opcode) {
            case 0x00u: /* unreachable */
                turbowasm_control_mark_unreachable(&stack);
                break;
            case 0x01u: /* nop */
                break;
            case 0x02u: /* block */
            case 0x03u: /* loop */
            case 0x04u: { /* if */
                turbowasm_block_signature signature;
                turbowasm_control_kind kind =
                    opcode == 0x02u ? TURBOWASM_CTRL_BLOCK :
                    opcode == 0x03u ? TURBOWASM_CTRL_LOOP :
                                      TURBOWASM_CTRL_IF;

                if (opcode == 0x04u) {
                    result = turbowasm_stack_pop(&stack, TW_I32);
                    if (result != TURBOWASM_OK)
                        goto done;
                }

                result = turbowasm_read_block_signature(
                    body, context, &signature);
                if (result != TURBOWASM_OK)
                    goto done;

                result = turbowasm_control_push(
                    &stack, &controls, kind,
                    signature.start_types, signature.start_count,
                    signature.end_types, signature.end_count);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x05u: /* else */
                result = turbowasm_control_else(&stack, &controls);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            case 0x0bu: /* end */
                if (controls.size == 1u) {
                    if (turbowasm_reader_remaining(body) != 0u) {
                        result = TURBOWASM_MALFORMED_MODULE;
                        goto done;
                    }
                    result = turbowasm_validate_result_stack(
                        &stack, function_type);
                    goto done;
                }
                result = turbowasm_control_end(&stack, &controls);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            case 0x0cu: /* br */
            case 0x0du: { /* br_if */
                uint32_t depth;
                if (!turbowasm_reader_uleb32(body, &depth)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                if (opcode == 0x0du) {
                    result = turbowasm_stack_pop(&stack, TW_I32);
                    if (result != TURBOWASM_OK)
                        goto done;
                }
                result = turbowasm_control_branch(
                    &stack, &controls, depth, opcode == 0x0du);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x0fu: /* return */
                result = turbowasm_pop_results(
                    &stack, function_type);
                if (result != TURBOWASM_OK)
                    goto done;
                turbowasm_control_mark_unreachable(&stack);
                break;
            case 0x10u: /* call */
                result = turbowasm_validate_call(
                    body, &stack, context);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            case 0x11u: /* call_indirect */
                result = turbowasm_validate_call_indirect(
                    body, &stack, context);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            case 0x1au: /* drop */
                result = turbowasm_stack_pop_any(&stack, NULL);
                if (result != TURBOWASM_OK)
                    goto done;
                break;
            case 0x1bu: { /* select */
                uint8_t right;
                uint8_t left;
                result = turbowasm_stack_pop(&stack, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                result = turbowasm_stack_pop_any(&stack, &right);
                if (result != TURBOWASM_OK) goto done;
                result = turbowasm_stack_pop_any(&stack, &left);
                if (result != TURBOWASM_OK) goto done;
                if (left != TW_ANY && right != TW_ANY &&
                    left != right) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                if (left == TW_ANY)
                    left = right == TW_ANY ? TW_I32 : right;
                if (left == TW_FUNCREF || left == TW_EXTERNREF) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_stack_push(&stack, left);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x20u: /* local.get */
            case 0x21u: /* local.set */
            case 0x22u: { /* local.tee */
                uint32_t local_index;
                uint8_t type;
                if (!turbowasm_reader_uleb32(body, &local_index) ||
                    local_index >= local_count) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                type = locals[local_index];
                if (opcode != 0x20u) {
                    result = turbowasm_stack_pop(&stack, type);
                    if (result != TURBOWASM_OK) goto done;
                }
                if (opcode != 0x21u) {
                    result = turbowasm_stack_push(&stack, type);
                    if (result != TURBOWASM_OK) goto done;
                }
                break;
            }
            case 0x23u: /* global.get */
            case 0x24u: { /* global.set */
                uint32_t global_index;
                const turbowasm_validation_global *global;
                if (!turbowasm_reader_uleb32(body, &global_index)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                global = turbowasm_validation_context_global(
                    context, global_index);
                if (global == NULL) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                if (opcode == 0x23u) {
                    result = turbowasm_stack_push(
                        &stack, global->value_type);
                } else {
                    if (!global->mutable_value) {
                        result = TURBOWASM_MALFORMED_MODULE;
                        goto done;
                    }
                    result = turbowasm_stack_pop(
                        &stack, global->value_type);
                }
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x41u: {
                int32_t value;
                if (!turbowasm_reader_sleb32(body, &value)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                (void)value;
                result = turbowasm_stack_push(&stack, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x42u: {
                int64_t value;
                if (!turbowasm_reader_sleb64(body, &value)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                (void)value;
                result = turbowasm_stack_push(&stack, TW_I64);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x43u: {
                turbowasm_reader bytes;
                if (!turbowasm_reader_slice(body, 4u, &bytes)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_stack_push(&stack, TW_F32);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x44u: {
                turbowasm_reader bytes;
                if (!turbowasm_reader_slice(body, 8u, &bytes)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_stack_push(&stack, TW_F64);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0x45u: /* i32.eqz */
                result = turbowasm_stack_unary(
                    &stack, TW_I32, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x46u: case 0x47u: case 0x48u: case 0x49u:
            case 0x4au: case 0x4bu: case 0x4cu: case 0x4du:
            case 0x4eu: case 0x4fu:
                result = turbowasm_stack_binary(
                    &stack, TW_I32, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x50u: /* i64.eqz */
                result = turbowasm_stack_unary(
                    &stack, TW_I64, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x51u: case 0x52u: case 0x53u: case 0x54u:
            case 0x55u: case 0x56u: case 0x57u: case 0x58u:
            case 0x59u: case 0x5au:
                result = turbowasm_stack_binary(
                    &stack, TW_I64, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x5bu: case 0x5cu: case 0x5du:
            case 0x5eu: case 0x5fu: case 0x60u:
                result = turbowasm_stack_binary(
                    &stack, TW_F32, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x61u: case 0x62u: case 0x63u:
            case 0x64u: case 0x65u: case 0x66u:
                result = turbowasm_stack_binary(
                    &stack, TW_F64, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x67u: case 0x68u: case 0x69u:
                result = turbowasm_stack_unary(
                    &stack, TW_I32, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x6au: case 0x6bu: case 0x6cu: case 0x6du:
            case 0x6eu: case 0x6fu: case 0x70u: case 0x71u:
            case 0x72u: case 0x73u: case 0x74u: case 0x75u:
            case 0x76u: case 0x77u: case 0x78u:
                result = turbowasm_stack_binary(
                    &stack, TW_I32, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x79u: case 0x7au: case 0x7bu:
                result = turbowasm_stack_unary(
                    &stack, TW_I64, TW_I64);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x7cu: case 0x7du: case 0x7eu: case 0x7fu:
            case 0x80u: case 0x81u: case 0x82u: case 0x83u:
            case 0x84u: case 0x85u: case 0x86u: case 0x87u:
            case 0x88u: case 0x89u: case 0x8au:
                result = turbowasm_stack_binary(
                    &stack, TW_I64, TW_I64);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x8bu: case 0x8cu: case 0x8du:
            case 0x8eu: case 0x8fu: case 0x90u: case 0x91u:
                result = turbowasm_stack_unary(
                    &stack, TW_F32, TW_F32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x92u: case 0x93u: case 0x94u:
            case 0x95u: case 0x96u: case 0x97u: case 0x98u:
                result = turbowasm_stack_binary(
                    &stack, TW_F32, TW_F32);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0x99u: case 0x9au: case 0x9bu:
            case 0x9cu: case 0x9du: case 0x9eu: case 0x9fu:
                result = turbowasm_stack_unary(
                    &stack, TW_F64, TW_F64);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0xa0u: case 0xa1u: case 0xa2u:
            case 0xa3u: case 0xa4u: case 0xa5u: case 0xa6u:
                result = turbowasm_stack_binary(
                    &stack, TW_F64, TW_F64);
                if (result != TURBOWASM_OK) goto done;
                break;
            case 0xd0u: { /* ref.null */
                uint8_t type;
                if (!turbowasm_reader_u8(body, &type) ||
                    (type != TW_FUNCREF && type != TW_EXTERNREF)) {
                    result = TURBOWASM_UNSUPPORTED;
                    goto done;
                }
                result = turbowasm_stack_push(&stack, type);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0xd1u: { /* ref.is_null */
                uint8_t type;
                result = turbowasm_stack_pop_any(&stack, &type);
                if (result != TURBOWASM_OK) goto done;
                if (type != TW_ANY &&
                    type != TW_FUNCREF && type != TW_EXTERNREF) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_stack_push(&stack, TW_I32);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0xd2u: { /* ref.func */
                uint32_t ref_index;
                if (!turbowasm_reader_uleb32(body, &ref_index) ||
                    ref_index >= context->function_count ||
                    !turbowasm_validation_context_has_function_ref(
                        context, ref_index)) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_stack_push(
                    &stack, TW_FUNCREF);
                if (result != TURBOWASM_OK) goto done;
                break;
            }
            case 0xfdu:
                result = turbowasm_validate_simd(body, &stack);
                if (result != TURBOWASM_OK) goto done;
                break;
            default:
                result = TURBOWASM_UNSUPPORTED;
                goto done;
        }
    }

    result = TURBOWASM_MALFORMED_MODULE;

done:
    free(locals);
    free(stack.values);
    turbowasm_control_stack_destroy(&controls);
    return result;
}
