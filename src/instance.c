#include <turbowasm/instance.h>

#include "instance_internal.h"
#include "module_internal.h"
#include "reader.h"
#include "simd_exec_table.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_EXEC_MAX_CALL_DEPTH = 256u
};

typedef struct turbowasm_value_stack {
    turbowasm_value *values;
    uint32_t size;
    uint32_t capacity;
} turbowasm_value_stack;

typedef enum turbowasm_exec_control_kind {
    TURBOWASM_EXEC_CONTROL_FUNCTION = 0,
    TURBOWASM_EXEC_CONTROL_BLOCK,
    TURBOWASM_EXEC_CONTROL_LOOP,
    TURBOWASM_EXEC_CONTROL_IF
} turbowasm_exec_control_kind;

typedef struct turbowasm_exec_control_frame {
    turbowasm_exec_control_kind kind;
    uint32_t height;
    const turbowasm_validation_control *annotation;
    const turbowasm_validation_func_type *signature_type;
    uint8_t inline_end_type;
    bool has_inline_end_type;
} turbowasm_exec_control_frame;

typedef struct turbowasm_exec_control_stack {
    turbowasm_exec_control_frame *frames;
    uint32_t size;
    uint32_t capacity;
} turbowasm_exec_control_stack;

typedef struct turbowasm_exec_block_signature {
    const turbowasm_validation_func_type *indexed_type;
    uint8_t inline_end_type;
    bool has_inline_end_type;
} turbowasm_exec_block_signature;

static turbowasm_value_kind turbowasm_kind_from_valtype(uint8_t type) {
    switch (type) {
        case 0x7fu: return TURBOWASM_VALUE_I32;
        case 0x7eu: return TURBOWASM_VALUE_I64;
        case 0x7du: return TURBOWASM_VALUE_F32;
        case 0x7cu: return TURBOWASM_VALUE_F64;
        case 0x7bu: return TURBOWASM_VALUE_V128;
        case 0x70u: return TURBOWASM_VALUE_FUNCREF;
        default: return (turbowasm_value_kind)0;
    }
}

static bool turbowasm_value_matches_type(
    const turbowasm_value *value,
    uint8_t type) {
    turbowasm_value_kind kind = turbowasm_kind_from_valtype(type);
    return value != NULL && kind != 0 && value->kind == kind;
}

static bool turbowasm_zero_value(uint8_t type, turbowasm_value *out) {
    turbowasm_value_kind kind;

    if (out == NULL)
        return false;

    kind = turbowasm_kind_from_valtype(type);
    if (kind == 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_VALUE_V128) {
        out->as.v128.shape = TURBOWASM_V128_RAW;
    } else if (kind == TURBOWASM_VALUE_FUNCREF) {
        out->as.funcref.is_null = true;
        out->as.funcref.function_index = UINT32_MAX;
    }
    return true;
}

static bool turbowasm_stack_reserve(
    turbowasm_value_stack *stack,
    uint32_t required) {
    uint32_t next;
    turbowasm_value *grown;

    if (required <= stack->capacity)
        return true;

    next = stack->capacity == 0u ? 16u : stack->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * sizeof(*grown) > (uint64_t)SIZE_MAX)
        return false;

    grown = (turbowasm_value *)realloc(
        stack->values, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    stack->values = grown;
    stack->capacity = next;
    return true;
}

static turbowasm_status turbowasm_stack_push(
    turbowasm_value_stack *stack,
    turbowasm_value value) {
    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_stack_reserve(stack, stack->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;
    stack->values[stack->size++] = value;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_pop(
    turbowasm_value_stack *stack,
    turbowasm_value *out) {
    if (stack == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == 0u)
        return TURBOWASM_MALFORMED_MODULE;
    *out = stack->values[--stack->size];
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_stack_pop_kind(
    turbowasm_value_stack *stack,
    turbowasm_value_kind kind,
    turbowasm_value *out) {
    turbowasm_status status = turbowasm_stack_pop(stack, out);
    if (status != TURBOWASM_OK)
        return status;
    return out->kind == kind
        ? TURBOWASM_OK
        : TURBOWASM_TYPE_MISMATCH;
}


static bool turbowasm_exec_valtype(uint8_t type) {
    return type == 0x7fu || type == 0x7eu ||
           type == 0x7du || type == 0x7cu ||
           type == 0x7bu || type == 0x70u ||
           type == 0x6fu;
}

static bool turbowasm_exec_controls_reserve(
    turbowasm_exec_control_stack *controls,
    uint32_t required) {
    uint32_t next;
    turbowasm_exec_control_frame *grown;

    if (controls == NULL)
        return false;
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

    grown = (turbowasm_exec_control_frame *)realloc(
        controls->frames, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    controls->frames = grown;
    controls->capacity = next;
    return true;
}

static const uint8_t *turbowasm_exec_frame_start_types(
    const turbowasm_exec_control_frame *frame,
    uint32_t *out_count) {
    if (frame->kind == TURBOWASM_EXEC_CONTROL_FUNCTION) {
        *out_count = 0u;
        return NULL;
    }
    if (frame->signature_type != NULL) {
        *out_count = frame->signature_type->param_count;
        return frame->signature_type->params;
    }
    *out_count = 0u;
    return NULL;
}

static const uint8_t *turbowasm_exec_frame_end_types(
    const turbowasm_exec_control_frame *frame,
    uint32_t *out_count) {
    if (frame->signature_type != NULL) {
        *out_count = frame->signature_type->result_count;
        return frame->signature_type->results;
    }
    if (frame->has_inline_end_type) {
        *out_count = 1u;
        return &frame->inline_end_type;
    }
    *out_count = 0u;
    return NULL;
}

static turbowasm_status turbowasm_exec_stack_check_top(
    const turbowasm_value_stack *stack,
    const uint8_t *types,
    uint32_t count) {
    uint32_t index;
    uint32_t base;

    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size < count)
        return TURBOWASM_MALFORMED_MODULE;

    base = stack->size - count;
    for (index = 0u; index < count; ++index) {
        if (!turbowasm_value_matches_type(
                &stack->values[base + index], types[index])) {
            turbowasm_value_kind kind =
                turbowasm_kind_from_valtype(types[index]);
            return kind == 0
                ? TURBOWASM_UNSUPPORTED
                : TURBOWASM_TYPE_MISMATCH;
        }
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_stack_rebase(
    turbowasm_value_stack *stack,
    uint32_t height,
    const uint8_t *types,
    uint32_t count) {
    uint32_t source;
    turbowasm_status status;

    if (stack == NULL || height > stack->size)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_exec_stack_check_top(stack, types, count);
    if (status != TURBOWASM_OK)
        return status;

    source = stack->size - count;
    if (source < height)
        return TURBOWASM_MALFORMED_MODULE;

    if (count != 0u && source != height) {
        memmove(&stack->values[height],
                &stack->values[source],
                (size_t)count * sizeof(*stack->values));
    }
    stack->size = height + count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_read_block_signature(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_exec_block_signature *signature) {
    uint8_t first;

    if (reader == NULL || context == NULL || signature == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(signature, 0, sizeof(*signature));
    if (!turbowasm_reader_u8(reader, &first))
        return TURBOWASM_MALFORMED_MODULE;

    if (first == 0x40u)
        return TURBOWASM_OK;

    if (turbowasm_exec_valtype(first)) {
        signature->inline_end_type = first;
        signature->has_inline_end_type = true;
        return TURBOWASM_OK;
    }

    {
        uint64_t value = (uint64_t)(first & 0x7fu);
        unsigned shift = 7u;
        unsigned count = 1u;
        uint8_t byte = first;
        int64_t signed_value;

        while ((byte & 0x80u) != 0u) {
            if (count >= 5u ||
                !turbowasm_reader_u8(reader, &byte))
                return TURBOWASM_MALFORMED_MODULE;
            value |= (uint64_t)(byte & 0x7fu) << shift;
            shift += 7u;
            ++count;
        }

        if (count == 5u && (byte & 0x70u) != 0u)
            return TURBOWASM_MALFORMED_MODULE;

        if ((byte & 0x40u) != 0u && shift < 64u)
            value |= UINT64_MAX << shift;
        signed_value = (int64_t)value;

        if (signed_value < 0 ||
            (uint64_t)signed_value > UINT32_MAX)
            return TURBOWASM_MALFORMED_MODULE;

        signature->indexed_type =
            turbowasm_validation_context_type(
                context, (uint32_t)signed_value);
        if (signature->indexed_type == NULL ||
            !signature->indexed_type->defined)
            return TURBOWASM_MALFORMED_MODULE;
        return TURBOWASM_OK;
    }
}

static turbowasm_status turbowasm_exec_control_push(
    turbowasm_exec_control_stack *controls,
    turbowasm_value_stack *stack,
    turbowasm_exec_control_kind kind,
    const turbowasm_validation_control *annotation,
    const turbowasm_exec_block_signature *signature) {
    turbowasm_exec_control_frame frame = {0};
    const uint8_t *start_types;
    uint32_t start_count;
    turbowasm_status status;

    if (controls == NULL || stack == NULL || signature == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (controls->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    frame.kind = kind;
    frame.annotation = annotation;
    frame.signature_type = signature->indexed_type;
    frame.inline_end_type = signature->inline_end_type;
    frame.has_inline_end_type = signature->has_inline_end_type;

    start_types = turbowasm_exec_frame_start_types(
        &frame, &start_count);
    status = turbowasm_exec_stack_check_top(
        stack, start_types, start_count);
    if (status != TURBOWASM_OK)
        return status;

    frame.height = stack->size - start_count;

    if (!turbowasm_exec_controls_reserve(
            controls, controls->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;

    controls->frames[controls->size++] = frame;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_control_push_function(
    turbowasm_exec_control_stack *controls,
    const turbowasm_validation_func_type *type) {
    turbowasm_exec_control_frame frame = {0};

    if (controls == NULL || type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_exec_controls_reserve(controls, 1u))
        return TURBOWASM_OUT_OF_MEMORY;

    frame.kind = TURBOWASM_EXEC_CONTROL_FUNCTION;
    frame.height = 0u;
    frame.signature_type = type;
    controls->frames[0] = frame;
    controls->size = 1u;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_jump(
    turbowasm_reader *reader,
    const turbowasm_validation_function *function,
    uint32_t offset) {
    if (reader == NULL || function == NULL ||
        function->code == NULL || offset > function->code_size)
        return TURBOWASM_MALFORMED_MODULE;

    reader->cursor = function->code + offset;
    reader->end = function->code + function->code_size;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_finish_control(
    turbowasm_value_stack *stack,
    turbowasm_exec_control_stack *controls) {
    turbowasm_exec_control_frame *frame;
    const uint8_t *end_types;
    uint32_t end_count;
    turbowasm_status status;

    if (stack == NULL || controls == NULL || controls->size <= 1u)
        return TURBOWASM_MALFORMED_MODULE;

    frame = &controls->frames[controls->size - 1u];
    end_types = turbowasm_exec_frame_end_types(
        frame, &end_count);

    status = turbowasm_exec_stack_rebase(
        stack, frame->height, end_types, end_count);
    if (status != TURBOWASM_OK)
        return status;

    --controls->size;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_branch(
    turbowasm_value_stack *stack,
    turbowasm_exec_control_stack *controls,
    turbowasm_reader *reader,
    const turbowasm_validation_function *function,
    uint32_t depth,
    bool *out_finished,
    bool *out_returned) {
    uint32_t target_index;
    turbowasm_exec_control_frame *target;
    const uint8_t *label_types;
    uint32_t label_count;
    turbowasm_status status;

    if (stack == NULL || controls == NULL ||
        out_finished == NULL || out_returned == NULL ||
        depth >= controls->size)
        return TURBOWASM_MALFORMED_MODULE;

    target_index = controls->size - 1u - depth;
    target = &controls->frames[target_index];

    if (target->kind == TURBOWASM_EXEC_CONTROL_LOOP) {
        label_types = turbowasm_exec_frame_start_types(
            target, &label_count);
    } else {
        label_types = turbowasm_exec_frame_end_types(
            target, &label_count);
    }

    status = turbowasm_exec_stack_rebase(
        stack, target->height, label_types, label_count);
    if (status != TURBOWASM_OK)
        return status;

    if (target->kind == TURBOWASM_EXEC_CONTROL_FUNCTION) {
        controls->size = 1u;
        *out_finished = true;
        *out_returned = true;
        return TURBOWASM_OK;
    }

    if (target->annotation == NULL ||
        target->annotation->end_offset == UINT32_MAX)
        return TURBOWASM_MALFORMED_MODULE;

    if (target->kind == TURBOWASM_EXEC_CONTROL_LOOP) {
        controls->size = target_index + 1u;
        return turbowasm_exec_jump(
            reader, function, target->annotation->body_offset);
    }

    controls->size = target_index;
    return turbowasm_exec_jump(
        reader, function, target->annotation->end_offset + 1u);
}

static turbowasm_status turbowasm_exec_br_table(
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_exec_control_stack *controls,
    const turbowasm_validation_function *function,
    bool *out_finished,
    bool *out_returned) {
    uint32_t count;
    uint32_t index;
    uint32_t selected_depth = 0u;
    uint32_t default_depth;
    turbowasm_value selector;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &selector);
    if (status != TURBOWASM_OK)
        return status;

    for (index = 0u; index < count; ++index) {
        uint32_t depth;
        if (!turbowasm_reader_uleb32(reader, &depth))
            return TURBOWASM_MALFORMED_MODULE;
        if ((uint32_t)selector.as.i32 == index)
            selected_depth = depth;
    }

    if (!turbowasm_reader_uleb32(reader, &default_depth))
        return TURBOWASM_MALFORMED_MODULE;

    if ((uint32_t)selector.as.i32 >= count)
        selected_depth = default_depth;

    return turbowasm_exec_branch(
        stack, controls, reader, function,
        selected_depth, out_finished, out_returned);
}

static int32_t turbowasm_sar32(int32_t value, uint32_t shift) {
    uint32_t bits = (uint32_t)value;
    shift &= 31u;
    if (shift == 0u)
        return value;
    if (value >= 0)
        return (int32_t)(bits >> shift);
    return (int32_t)((bits >> shift) |
                     (~UINT32_C(0) << (32u - shift)));
}

static int64_t turbowasm_sar64(int64_t value, uint64_t shift) {
    uint64_t bits = (uint64_t)value;
    shift &= 63u;
    if (shift == 0u)
        return value;
    if (value >= 0)
        return (int64_t)(bits >> shift);
    return (int64_t)((bits >> shift) |
                     (~UINT64_C(0) << (64u - shift)));
}

static uint32_t turbowasm_rotl32(uint32_t value, uint32_t shift) {
    shift &= 31u;
    return shift == 0u
        ? value
        : (value << shift) | (value >> (32u - shift));
}

static uint32_t turbowasm_rotr32(uint32_t value, uint32_t shift) {
    shift &= 31u;
    return shift == 0u
        ? value
        : (value >> shift) | (value << (32u - shift));
}

static uint64_t turbowasm_rotl64(uint64_t value, uint64_t shift) {
    shift &= 63u;
    return shift == 0u
        ? value
        : (value << shift) | (value >> (64u - shift));
}

static uint64_t turbowasm_rotr64(uint64_t value, uint64_t shift) {
    shift &= 63u;
    return shift == 0u
        ? value
        : (value >> shift) | (value << (64u - shift));
}

static turbowasm_status turbowasm_exec_function(
    turbowasm_instance_impl *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    uint32_t depth);

static bool turbowasm_exec_type_equal(
    const turbowasm_validation_func_type *left,
    const turbowasm_validation_func_type *right) {
    if (left == NULL || right == NULL ||
        left->param_count != right->param_count ||
        left->result_count != right->result_count)
        return false;
    if (left->param_count != 0u &&
        memcmp(left->params, right->params,
               (size_t)left->param_count) != 0)
        return false;
    if (left->result_count != 0u &&
        memcmp(left->results, right->results,
               (size_t)left->result_count) != 0)
        return false;
    return true;
}

static turbowasm_status turbowasm_exec_call_index(
    turbowasm_instance_impl *instance,
    uint32_t function_index,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint32_t depth) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_func_type *type;
    turbowasm_value *arguments = NULL;
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    uint32_t index;
    turbowasm_status status;

    if (instance == NULL || stack == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_validation_context_function_type(
        &module->validation, function_index);
    if (type == NULL || !type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    if (type->param_count != 0u) {
        arguments = (turbowasm_value *)calloc(
            (size_t)type->param_count, sizeof(*arguments));
        if (arguments == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    index = type->param_count;
    while (index != 0u) {
        --index;
        status = turbowasm_stack_pop(stack, &arguments[index]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    if (type->result_count != 0u) {
        results = (turbowasm_value *)calloc(
            (size_t)type->result_count, sizeof(*results));
        if (results == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto done;
        }
    }

    status = turbowasm_exec_function(
        instance,
        function_index,
        arguments,
        type->param_count,
        results,
        type->result_count,
        &result_count,
        trap,
        depth + 1u);
    if (status != TURBOWASM_OK)
        goto done;

    if (result_count != type->result_count) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    for (index = 0u; index < type->result_count; ++index) {
        status = turbowasm_stack_push(stack, results[index]);
        if (status != TURBOWASM_OK)
            goto done;
    }

done:
    free(arguments);
    free(results);
    return status;
}

static turbowasm_status turbowasm_exec_direct_call(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint32_t depth) {
    uint32_t function_index;

    if (!turbowasm_reader_uleb32(reader, &function_index))
        return TURBOWASM_MALFORMED_MODULE;

    return turbowasm_exec_call_index(
        instance, function_index, stack, trap, depth);
}

static turbowasm_status turbowasm_exec_indirect_call(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint32_t depth) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_func_type *expected_type;
    const turbowasm_validation_func_type *actual_type;
    turbowasm_instance_table_entry entry;
    turbowasm_value selector;
    uint32_t type_index;
    uint32_t table_index;
    turbowasm_status status;

    if (instance == NULL || reader == NULL ||
        stack == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_reader_uleb32(reader, &type_index) ||
        !turbowasm_reader_uleb32(reader, &table_index))
        return TURBOWASM_MALFORMED_MODULE;

    expected_type = turbowasm_validation_context_type(
        &module->validation, type_index);
    if (expected_type == NULL || !expected_type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &selector);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_instance_table_lookup(
        instance, table_index,
        (uint32_t)selector.as.i32, &entry);
    if (status == TURBOWASM_TRAPPED) {
        *trap = TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }
    if (status != TURBOWASM_OK)
        return status;

    if (entry.is_null) {
        *trap = TURBOWASM_TRAP_INDIRECT_CALL_NULL;
        return TURBOWASM_TRAPPED;
    }

    actual_type = turbowasm_validation_context_function_type(
        &module->validation, entry.function_index);
    if (!turbowasm_exec_type_equal(expected_type, actual_type)) {
        *trap = TURBOWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH;
        return TURBOWASM_TRAPPED;
    }

    return turbowasm_exec_call_index(
        instance, entry.function_index, stack, trap, depth);
}

static turbowasm_status turbowasm_exec_i32_binary(
    uint8_t opcode,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value out = {0};
    uint32_t a;
    uint32_t b;
    turbowasm_status status;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &left);
    if (status != TURBOWASM_OK) return status;

    a = (uint32_t)left.as.i32;
    b = (uint32_t)right.as.i32;
    out.kind = TURBOWASM_VALUE_I32;

    switch (opcode) {
        case 0x6au: out.as.i32 = (int32_t)(a + b); break;
        case 0x6bu: out.as.i32 = (int32_t)(a - b); break;
        case 0x6cu: out.as.i32 = (int32_t)(a * b); break;
        case 0x6du:
            if (right.as.i32 == 0) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            if (left.as.i32 == INT32_MIN && right.as.i32 == -1) {
                *trap = TURBOWASM_TRAP_INTEGER_OVERFLOW;
                return TURBOWASM_TRAPPED;
            }
            out.as.i32 = left.as.i32 / right.as.i32;
            break;
        case 0x6eu:
            if (b == 0u) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i32 = (int32_t)(a / b);
            break;
        case 0x6fu:
            if (right.as.i32 == 0) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i32 =
                left.as.i32 == INT32_MIN && right.as.i32 == -1
                ? 0
                : left.as.i32 % right.as.i32;
            break;
        case 0x70u:
            if (b == 0u) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i32 = (int32_t)(a % b);
            break;
        case 0x71u: out.as.i32 = (int32_t)(a & b); break;
        case 0x72u: out.as.i32 = (int32_t)(a | b); break;
        case 0x73u: out.as.i32 = (int32_t)(a ^ b); break;
        case 0x74u: out.as.i32 = (int32_t)(a << (b & 31u)); break;
        case 0x75u:
            out.as.i32 = turbowasm_sar32(left.as.i32, b);
            break;
        case 0x76u: out.as.i32 = (int32_t)(a >> (b & 31u)); break;
        case 0x77u: out.as.i32 = (int32_t)turbowasm_rotl32(a, b); break;
        case 0x78u: out.as.i32 = (int32_t)turbowasm_rotr32(a, b); break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_i64_binary(
    uint8_t opcode,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value out = {0};
    uint64_t a;
    uint64_t b;
    turbowasm_status status;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I64, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I64, &left);
    if (status != TURBOWASM_OK) return status;

    a = (uint64_t)left.as.i64;
    b = (uint64_t)right.as.i64;
    out.kind = TURBOWASM_VALUE_I64;

    switch (opcode) {
        case 0x7cu: out.as.i64 = (int64_t)(a + b); break;
        case 0x7du: out.as.i64 = (int64_t)(a - b); break;
        case 0x7eu: out.as.i64 = (int64_t)(a * b); break;
        case 0x7fu:
            if (right.as.i64 == 0) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            if (left.as.i64 == INT64_MIN && right.as.i64 == -1) {
                *trap = TURBOWASM_TRAP_INTEGER_OVERFLOW;
                return TURBOWASM_TRAPPED;
            }
            out.as.i64 = left.as.i64 / right.as.i64;
            break;
        case 0x80u:
            if (b == 0u) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i64 = (int64_t)(a / b);
            break;
        case 0x81u:
            if (right.as.i64 == 0) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i64 =
                left.as.i64 == INT64_MIN && right.as.i64 == -1
                ? 0
                : left.as.i64 % right.as.i64;
            break;
        case 0x82u:
            if (b == 0u) {
                *trap = TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
                return TURBOWASM_TRAPPED;
            }
            out.as.i64 = (int64_t)(a % b);
            break;
        case 0x83u: out.as.i64 = (int64_t)(a & b); break;
        case 0x84u: out.as.i64 = (int64_t)(a | b); break;
        case 0x85u: out.as.i64 = (int64_t)(a ^ b); break;
        case 0x86u: out.as.i64 = (int64_t)(a << (b & 63u)); break;
        case 0x87u:
            out.as.i64 = turbowasm_sar64(left.as.i64, b);
            break;
        case 0x88u: out.as.i64 = (int64_t)(a >> (b & 63u)); break;
        case 0x89u: out.as.i64 = (int64_t)turbowasm_rotl64(a, b); break;
        case 0x8au: out.as.i64 = (int64_t)turbowasm_rotr64(a, b); break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_f32_binary(
    uint8_t opcode,
    turbowasm_value_stack *stack) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value out = {0};
    turbowasm_status status;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_F32, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_F32, &left);
    if (status != TURBOWASM_OK) return status;

    out.kind = TURBOWASM_VALUE_F32;
    switch (opcode) {
        case 0x92u: out.as.f32 = left.as.f32 + right.as.f32; break;
        case 0x93u: out.as.f32 = left.as.f32 - right.as.f32; break;
        case 0x94u: out.as.f32 = left.as.f32 * right.as.f32; break;
        case 0x95u: out.as.f32 = left.as.f32 / right.as.f32; break;
        default: return TURBOWASM_UNSUPPORTED;
    }
    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_f64_binary(
    uint8_t opcode,
    turbowasm_value_stack *stack) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value out = {0};
    turbowasm_status status;

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_F64, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_F64, &left);
    if (status != TURBOWASM_OK) return status;

    out.kind = TURBOWASM_VALUE_F64;
    switch (opcode) {
        case 0xa0u: out.as.f64 = left.as.f64 + right.as.f64; break;
        case 0xa1u: out.as.f64 = left.as.f64 - right.as.f64; break;
        case 0xa2u: out.as.f64 = left.as.f64 * right.as.f64; break;
        case 0xa3u: out.as.f64 = left.as.f64 / right.as.f64; break;
        default: return TURBOWASM_UNSUPPORTED;
    }
    return turbowasm_stack_push(stack, out);
}

static uint16_t turbowasm_read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] |
           (uint16_t)((uint16_t)p[1] << 8u);
}

static uint32_t turbowasm_read_u32_le_bytes(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static uint64_t turbowasm_read_u64_le_bytes(const uint8_t *p) {
    uint64_t value = 0u;
    uint32_t index;
    for (index = 0u; index < 8u; ++index)
        value |= (uint64_t)p[index] << (8u * index);
    return value;
}

static void turbowasm_write_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
}

static void turbowasm_write_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static void turbowasm_write_u64_le(uint8_t *p, uint64_t value) {
    uint32_t index;
    for (index = 0u; index < 8u; ++index)
        p[index] = (uint8_t)(value >> (8u * index));
}

static turbowasm_status turbowasm_exec_read_memarg(
    turbowasm_reader *reader,
    uint32_t *out_offset) {
    uint32_t alignment;
    uint32_t offset;

    if (!turbowasm_reader_uleb32(reader, &alignment) ||
        !turbowasm_reader_uleb32(reader, &offset))
        return TURBOWASM_MALFORMED_MODULE;
    (void)alignment;
    *out_offset = offset;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_memory_load(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint8_t opcode) {
    turbowasm_value address;
    turbowasm_value out = {0};
    uint32_t offset;
    uint8_t *p;
    size_t width;
    turbowasm_status status;

    status = turbowasm_exec_read_memarg(reader, &offset);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &address);
    if (status != TURBOWASM_OK)
        return status;

    switch (opcode) {
        case 0x28u: width = 4u; break;
        case 0x29u: width = 8u; break;
        case 0x2au: width = 4u; break;
        case 0x2bu: width = 8u; break;
        case 0x2cu: case 0x2du:
        case 0x30u: case 0x31u: width = 1u; break;
        case 0x2eu: case 0x2fu:
        case 0x32u: case 0x33u: width = 2u; break;
        case 0x34u: case 0x35u: width = 4u; break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    status = turbowasm_instance_memory_bounds(
        instance, 0u, (uint32_t)address.as.i32,
        offset, width, &p);
    if (status == TURBOWASM_TRAPPED) {
        *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }
    if (status != TURBOWASM_OK)
        return status;

    switch (opcode) {
        case 0x28u:
            out.kind = TURBOWASM_VALUE_I32;
            out.as.i32 = (int32_t)turbowasm_read_u32_le_bytes(p);
            break;
        case 0x29u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 = (int64_t)turbowasm_read_u64_le_bytes(p);
            break;
        case 0x2au: {
            uint32_t bits = turbowasm_read_u32_le_bytes(p);
            out.kind = TURBOWASM_VALUE_F32;
            memcpy(&out.as.f32, &bits, sizeof(bits));
            break;
        }
        case 0x2bu: {
            uint64_t bits = turbowasm_read_u64_le_bytes(p);
            out.kind = TURBOWASM_VALUE_F64;
            memcpy(&out.as.f64, &bits, sizeof(bits));
            break;
        }
        case 0x2cu:
            out.kind = TURBOWASM_VALUE_I32;
            out.as.i32 = (int32_t)(int8_t)p[0];
            break;
        case 0x2du:
            out.kind = TURBOWASM_VALUE_I32;
            out.as.i32 = (int32_t)(uint8_t)p[0];
            break;
        case 0x2eu:
            out.kind = TURBOWASM_VALUE_I32;
            out.as.i32 = (int32_t)(int16_t)turbowasm_read_u16_le(p);
            break;
        case 0x2fu:
            out.kind = TURBOWASM_VALUE_I32;
            out.as.i32 = (int32_t)turbowasm_read_u16_le(p);
            break;
        case 0x30u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 = (int64_t)(int8_t)p[0];
            break;
        case 0x31u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 = (int64_t)(uint8_t)p[0];
            break;
        case 0x32u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 = (int64_t)(int16_t)turbowasm_read_u16_le(p);
            break;
        case 0x33u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 = (int64_t)turbowasm_read_u16_le(p);
            break;
        case 0x34u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 =
                (int64_t)(int32_t)turbowasm_read_u32_le_bytes(p);
            break;
        case 0x35u:
            out.kind = TURBOWASM_VALUE_I64;
            out.as.i64 =
                (int64_t)(uint32_t)turbowasm_read_u32_le_bytes(p);
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_memory_store(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint8_t opcode) {
    turbowasm_value value;
    turbowasm_value address;
    turbowasm_value_kind expected;
    uint32_t offset;
    uint8_t *p;
    size_t width;
    turbowasm_status status;

    status = turbowasm_exec_read_memarg(reader, &offset);
    if (status != TURBOWASM_OK)
        return status;

    switch (opcode) {
        case 0x36u: expected = TURBOWASM_VALUE_I32; width = 4u; break;
        case 0x37u: expected = TURBOWASM_VALUE_I64; width = 8u; break;
        case 0x38u: expected = TURBOWASM_VALUE_F32; width = 4u; break;
        case 0x39u: expected = TURBOWASM_VALUE_F64; width = 8u; break;
        case 0x3au: case 0x3bu:
            expected = TURBOWASM_VALUE_I32;
            width = opcode == 0x3au ? 1u : 2u;
            break;
        case 0x3cu: case 0x3du: case 0x3eu:
            expected = TURBOWASM_VALUE_I64;
            width = opcode == 0x3cu ? 1u :
                    opcode == 0x3du ? 2u : 4u;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    status = turbowasm_stack_pop_kind(stack, expected, &value);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &address);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_instance_memory_bounds(
        instance, 0u, (uint32_t)address.as.i32,
        offset, width, &p);
    if (status == TURBOWASM_TRAPPED) {
        *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }
    if (status != TURBOWASM_OK)
        return status;

    switch (opcode) {
        case 0x36u:
            turbowasm_write_u32_le(p, (uint32_t)value.as.i32);
            break;
        case 0x37u:
            turbowasm_write_u64_le(p, (uint64_t)value.as.i64);
            break;
        case 0x38u: {
            uint32_t bits;
            memcpy(&bits, &value.as.f32, sizeof(bits));
            turbowasm_write_u32_le(p, bits);
            break;
        }
        case 0x39u: {
            uint64_t bits;
            memcpy(&bits, &value.as.f64, sizeof(bits));
            turbowasm_write_u64_le(p, bits);
            break;
        }
        case 0x3au:
            p[0] = (uint8_t)value.as.i32;
            break;
        case 0x3bu:
            turbowasm_write_u16_le(p, (uint16_t)value.as.i32);
            break;
        case 0x3cu:
            p[0] = (uint8_t)value.as.i64;
            break;
        case 0x3du:
            turbowasm_write_u16_le(p, (uint16_t)value.as.i64);
            break;
        case 0x3eu:
            turbowasm_write_u32_le(p, (uint32_t)value.as.i64);
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_memory_size_or_grow(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    uint8_t opcode) {
    uint8_t reserved;
    uint32_t pages;
    turbowasm_value out = {0};
    turbowasm_status status;

    if (!turbowasm_reader_u8(reader, &reserved))
        return TURBOWASM_MALFORMED_MODULE;
    if (reserved != 0u)
        return TURBOWASM_UNSUPPORTED;

    out.kind = TURBOWASM_VALUE_I32;

    if (opcode == 0x3fu) {
        status = turbowasm_instance_memory_size(
            instance, 0u, &pages);
        if (status != TURBOWASM_OK)
            return status;
        out.as.i32 = (int32_t)pages;
    } else if (opcode == 0x40u) {
        turbowasm_value delta;
        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_I32, &delta);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_instance_memory_grow(
            instance, 0u, (uint32_t)delta.as.i32, &pages);
        if (status != TURBOWASM_OK)
            return status;
        out.as.i32 = pages == UINT32_MAX
            ? -1
            : (int32_t)pages;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_bulk_trap(
    turbowasm_status status,
    turbowasm_trap *trap,
    turbowasm_trap trap_kind) {
    if (status == TURBOWASM_TRAPPED) {
        *trap = trap_kind;
        return TURBOWASM_TRAPPED;
    }
    return status;
}

static turbowasm_status turbowasm_exec_fc(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap) {
    uint32_t subopcode;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(reader, &subopcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (subopcode) {
        case 8u: { /* memory.init */
            uint32_t data_index;
            uint8_t memory_index;
            turbowasm_value length;
            turbowasm_value source;
            turbowasm_value destination;

            if (!turbowasm_reader_uleb32(reader, &data_index) ||
                !turbowasm_reader_u8(reader, &memory_index))
                return TURBOWASM_MALFORMED_MODULE;
            if (memory_index != 0u)
                return TURBOWASM_UNSUPPORTED;

            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &source);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_memory_init(
                instance, data_index, 0u,
                (uint32_t)destination.as.i32,
                (uint32_t)source.as.i32,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        }

        case 9u: { /* data.drop */
            uint32_t data_index;
            if (!turbowasm_reader_uleb32(reader, &data_index))
                return TURBOWASM_MALFORMED_MODULE;
            return turbowasm_instance_data_drop(
                instance, data_index);
        }

        case 10u: { /* memory.copy */
            uint8_t destination_memory;
            uint8_t source_memory;
            turbowasm_value length;
            turbowasm_value source;
            turbowasm_value destination;

            if (!turbowasm_reader_u8(
                    reader, &destination_memory) ||
                !turbowasm_reader_u8(
                    reader, &source_memory))
                return TURBOWASM_MALFORMED_MODULE;
            if (destination_memory != 0u ||
                source_memory != 0u)
                return TURBOWASM_UNSUPPORTED;

            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &source);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_memory_copy(
                instance, 0u, 0u,
                (uint32_t)destination.as.i32,
                (uint32_t)source.as.i32,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        }

        case 11u: { /* memory.fill */
            uint8_t memory_index;
            turbowasm_value length;
            turbowasm_value value;
            turbowasm_value destination;

            if (!turbowasm_reader_u8(reader, &memory_index))
                return TURBOWASM_MALFORMED_MODULE;
            if (memory_index != 0u)
                return TURBOWASM_UNSUPPORTED;

            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &value);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_memory_fill(
                instance, 0u,
                (uint32_t)destination.as.i32,
                (uint8_t)value.as.i32,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        }

        case 12u: { /* table.init */
            uint32_t element_index;
            uint32_t table_index;
            turbowasm_value length;
            turbowasm_value source;
            turbowasm_value destination;

            if (!turbowasm_reader_uleb32(
                    reader, &element_index) ||
                !turbowasm_reader_uleb32(
                    reader, &table_index))
                return TURBOWASM_MALFORMED_MODULE;

            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &source);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_table_init(
                instance, element_index, table_index,
                (uint32_t)destination.as.i32,
                (uint32_t)source.as.i32,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        }

        case 13u: { /* elem.drop */
            uint32_t element_index;
            if (!turbowasm_reader_uleb32(
                    reader, &element_index))
                return TURBOWASM_MALFORMED_MODULE;
            return turbowasm_instance_element_drop(
                instance, element_index);
        }

        case 14u: { /* table.copy */
            uint32_t destination_table;
            uint32_t source_table;
            turbowasm_value length;
            turbowasm_value source;
            turbowasm_value destination;

            if (!turbowasm_reader_uleb32(
                    reader, &destination_table) ||
                !turbowasm_reader_uleb32(
                    reader, &source_table))
                return TURBOWASM_MALFORMED_MODULE;

            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &source);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_table_copy(
                instance,
                destination_table, source_table,
                (uint32_t)destination.as.i32,
                (uint32_t)source.as.i32,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        }

        case 15u: { /* table.grow */
            uint32_t table_index;
            uint32_t previous_size;
            turbowasm_value delta;
            turbowasm_value initial;
            turbowasm_value result = {0};

            if (!turbowasm_reader_uleb32(
                    reader, &table_index))
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &delta);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_FUNCREF, &initial);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_table_grow(
                instance, table_index, initial,
                (uint32_t)delta.as.i32,
                &previous_size);
            if (status != TURBOWASM_OK)
                return status;

            result.kind = TURBOWASM_VALUE_I32;
            result.as.i32 = previous_size == UINT32_MAX
                ? -1
                : (int32_t)previous_size;
            return turbowasm_stack_push(stack, result);
        }

        case 16u: { /* table.size */
            uint32_t table_index;
            uint32_t size;
            turbowasm_value result = {0};

            if (!turbowasm_reader_uleb32(
                    reader, &table_index))
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_instance_table_size(
                instance, table_index, &size);
            if (status != TURBOWASM_OK)
                return status;

            result.kind = TURBOWASM_VALUE_I32;
            result.as.i32 = (int32_t)size;
            return turbowasm_stack_push(stack, result);
        }

        case 17u: { /* table.fill */
            uint32_t table_index;
            turbowasm_value length;
            turbowasm_value value;
            turbowasm_value destination;

            if (!turbowasm_reader_uleb32(
                    reader, &table_index))
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &length);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_FUNCREF, &value);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &destination);
            if (status != TURBOWASM_OK) return status;

            status = turbowasm_instance_table_fill(
                instance, table_index,
                (uint32_t)destination.as.i32,
                value,
                (uint32_t)length.as.i32);
            return turbowasm_exec_bulk_trap(
                status, trap,
                TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        }

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status turbowasm_exec_simd_splat(
    const turbowasm_simd_exec_descriptor *descriptor,
    turbowasm_value_stack *stack) {
    salts_simd_scalar scalar = {0};
    turbowasm_value input;
    turbowasm_value out = {0};
    turbowasm_status status;

    switch (descriptor->vector_desc->lane_kind) {
        case CMETA_VECTOR_I8:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.i8 = (int8_t)input.as.i32;
            break;
        case CMETA_VECTOR_I16:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.i16 = (int16_t)input.as.i32;
            break;
        case CMETA_VECTOR_I32:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.i32 = input.as.i32;
            break;
        case CMETA_VECTOR_I64:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I64, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.i64 = input.as.i64;
            break;
        case CMETA_VECTOR_F32:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_F32, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.f32 = input.as.f32;
            break;
        case CMETA_VECTOR_F64:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_F64, &input);
            if (status != TURBOWASM_OK) return status;
            scalar.f64 = input.as.f64;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    out.kind = TURBOWASM_VALUE_V128;
    out.as.v128.shape = descriptor->result_shape;
    if (!salts_simd_splat(
            descriptor->vector_desc,
            &out.as.v128.bits,
            scalar))
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_simd_generic(
    const turbowasm_simd_exec_descriptor *descriptor,
    turbowasm_value_stack *stack) {
    turbowasm_value out = {0};
    turbowasm_value left;
    turbowasm_value right;
    turbowasm_value mask;
    turbowasm_value count;
    uint32_t reduced = 0u;
    bool supported;
    turbowasm_status status;

    if (descriptor == NULL || stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_SPLAT)
        return turbowasm_exec_simd_splat(descriptor, stack);

    out.kind = TURBOWASM_VALUE_V128;
    out.as.v128.shape = descriptor->result_shape;

    switch (descriptor->kind) {
        case TURBOWASM_SIMD_EXEC_UNARY:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_unary(
                descriptor->vector_desc,
                (salts_simd_unary_op)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_BINARY:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_binary(
                descriptor->vector_desc,
                (salts_simd_binary_op)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_COMPARE:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_compare(
                descriptor->vector_desc,
                (salts_simd_compare_op)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_SHIFT:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &count);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_shift(
                descriptor->vector_desc,
                (salts_simd_shift_op)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits,
                (uint32_t)count.as.i32);
            break;

        case TURBOWASM_SIMD_EXEC_SELECT:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &mask);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_select(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits,
                &mask.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_SATURATING:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_saturating_binary(
                descriptor->vector_desc,
                (salts_simd_saturating_op)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_REDUCE:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_reduce(
                descriptor->vector_desc,
                (salts_simd_reduce_op)descriptor->op,
                &left.as.v128.bits,
                &reduced);
            if (supported) {
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = (int32_t)reduced;
            }
            break;

        case TURBOWASM_SIMD_EXEC_NARROW:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_narrow(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_EXTEND_HALF:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_extend_half(
                descriptor->vector_desc,
                (salts_simd_half)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_EXTMUL_HALF:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_extmul_half(
                descriptor->vector_desc,
                (salts_simd_half)descriptor->op,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_extadd_pairwise(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &left.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_Q15MULR:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_q15mulr_sat(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_DOT_PAIRWISE:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &right);
            if (status != TURBOWASM_OK) return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &left);
            if (status != TURBOWASM_OK) return status;
            supported = salts_simd_dot_pairwise(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits);
            break;

        case TURBOWASM_SIMD_EXEC_SPLAT:
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!supported)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_stack_push(stack, out);
}

static bool turbowasm_simd_exec_is_memory(
    turbowasm_simd_exec_kind kind) {
    return kind == TURBOWASM_SIMD_EXEC_MEMORY_EXTEND ||
           kind == TURBOWASM_SIMD_EXEC_MEMORY_SPLAT ||
           kind == TURBOWASM_SIMD_EXEC_MEMORY_ZERO ||
           kind == TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE ||
           kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE;
}

static turbowasm_status turbowasm_exec_simd_memory(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    const turbowasm_simd_exec_descriptor *descriptor) {
    uint32_t offset;
    uint8_t lane = 0u;
    turbowasm_value address;
    turbowasm_value vector_value;
    turbowasm_value out = {0};
    salts_simd_scalar scalar = {0};
    uint8_t *memory;
    turbowasm_status status;
    bool supported;

    if (instance == NULL || reader == NULL ||
        stack == NULL || trap == NULL || descriptor == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_exec_read_memarg(reader, &offset);
    if (status != TURBOWASM_OK)
        return status;

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE ||
        descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE) {
        if (!turbowasm_reader_u8(reader, &lane))
            return TURBOWASM_MALFORMED_MODULE;
        if (descriptor->vector_desc == NULL ||
            lane >= descriptor->vector_desc->lane_count)
            return TURBOWASM_MALFORMED_MODULE;

        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &vector_value);
        if (status != TURBOWASM_OK)
            return status;
    }

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_I32, &address);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_instance_memory_bounds(
        instance, 0u,
        (uint32_t)address.as.i32,
        offset, descriptor->memory_width,
        &memory);
    if (status == TURBOWASM_TRAPPED) {
        *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }
    if (status != TURBOWASM_OK)
        return status;

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE) {
        supported = salts_simd_extract_lane(
            descriptor->vector_desc,
            &vector_value.as.v128.bits,
            lane, &scalar);
        if (!supported)
            return TURBOWASM_UNSUPPORTED;

        switch (descriptor->memory_width) {
            case 1u:
                memory[0] = scalar.u8;
                return TURBOWASM_OK;
            case 2u:
                turbowasm_write_u16_le(memory, scalar.u16);
                return TURBOWASM_OK;
            case 4u:
                turbowasm_write_u32_le(memory, scalar.u32);
                return TURBOWASM_OK;
            case 8u:
                turbowasm_write_u64_le(memory, scalar.u64);
                return TURBOWASM_OK;
            default:
                return TURBOWASM_MALFORMED_MODULE;
        }
    }

    out.kind = TURBOWASM_VALUE_V128;
    out.as.v128.shape = descriptor->result_shape;

    switch (descriptor->kind) {
        case TURBOWASM_SIMD_EXEC_MEMORY_EXTEND:
            supported = salts_simd_load_extend(
                descriptor->vector_desc,
                &out.as.v128.bits,
                memory);
            break;

        case TURBOWASM_SIMD_EXEC_MEMORY_SPLAT:
            supported = salts_simd_load_splat(
                descriptor->vector_desc,
                &out.as.v128.bits,
                memory);
            break;

        case TURBOWASM_SIMD_EXEC_MEMORY_ZERO:
            supported = salts_simd_load_zero(
                (uint16_t)(descriptor->memory_width * 8u),
                &out.as.v128.bits,
                memory);
            break;

        case TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE:
            switch (descriptor->memory_width) {
                case 1u:
                    scalar.u8 = memory[0];
                    break;
                case 2u:
                    scalar.u16 = turbowasm_read_u16_le(memory);
                    break;
                case 4u:
                    scalar.u32 = turbowasm_read_u32_le_bytes(memory);
                    break;
                case 8u:
                    scalar.u64 = turbowasm_read_u64_le_bytes(memory);
                    break;
                default:
                    return TURBOWASM_MALFORMED_MODULE;
            }

            supported = salts_simd_replace_lane(
                descriptor->vector_desc,
                &out.as.v128.bits,
                &vector_value.as.v128.bits,
                lane, scalar);
            break;

        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!supported)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_stack_push(stack, out);
}

static bool turbowasm_simd_exec_is_lane_special(
    turbowasm_simd_exec_kind kind) {
    return kind == TURBOWASM_SIMD_EXEC_LANE_EXTRACT ||
           kind == TURBOWASM_SIMD_EXEC_LANE_REPLACE ||
           kind == TURBOWASM_SIMD_EXEC_SHUFFLE ||
           kind == TURBOWASM_SIMD_EXEC_SWIZZLE;
}

static turbowasm_status turbowasm_exec_simd_lane_special(
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    const turbowasm_simd_exec_descriptor *descriptor) {
    turbowasm_value left;
    turbowasm_value right;
    turbowasm_value scalar_value;
    turbowasm_value out = {0};
    salts_simd_scalar scalar = {0};
    uint8_t lane;
    turbowasm_status status;
    bool supported;

    if (reader == NULL || stack == NULL || descriptor == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_SHUFFLE) {
        uint8_t lanes[16];
        size_t index;

        for (index = 0u; index < 16u; ++index) {
            if (!turbowasm_reader_u8(reader, &lanes[index]))
                return TURBOWASM_MALFORMED_MODULE;
            if (lanes[index] >= 32u)
                return TURBOWASM_MALFORMED_MODULE;
        }

        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &right);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &left);
        if (status != TURBOWASM_OK)
            return status;

        out.kind = TURBOWASM_VALUE_V128;
        out.as.v128.shape = descriptor->result_shape;
        if (!salts_simd_shuffle_bytes(
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits,
                lanes))
            return TURBOWASM_UNSUPPORTED;
        return turbowasm_stack_push(stack, out);
    }

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_SWIZZLE) {
        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &right);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &left);
        if (status != TURBOWASM_OK)
            return status;

        out.kind = TURBOWASM_VALUE_V128;
        out.as.v128.shape = descriptor->result_shape;
        if (!salts_simd_swizzle_bytes(
                &out.as.v128.bits,
                &left.as.v128.bits,
                &right.as.v128.bits))
            return TURBOWASM_UNSUPPORTED;
        return turbowasm_stack_push(stack, out);
    }

    if (!turbowasm_reader_u8(reader, &lane))
        return TURBOWASM_MALFORMED_MODULE;
    if (descriptor->vector_desc == NULL ||
        lane >= descriptor->vector_desc->lane_count)
        return TURBOWASM_MALFORMED_MODULE;

    if (descriptor->kind == TURBOWASM_SIMD_EXEC_LANE_EXTRACT) {
        status = turbowasm_stack_pop_kind(
            stack, TURBOWASM_VALUE_V128, &left);
        if (status != TURBOWASM_OK)
            return status;

        supported = salts_simd_extract_lane(
            descriptor->vector_desc,
            &left.as.v128.bits,
            lane, &scalar);
        if (!supported)
            return TURBOWASM_UNSUPPORTED;

        switch (descriptor->vector_desc->lane_kind) {
            case CMETA_VECTOR_I8:
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = (int32_t)scalar.i8;
                break;
            case CMETA_VECTOR_U8:
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = (int32_t)scalar.u8;
                break;
            case CMETA_VECTOR_I16:
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = (int32_t)scalar.i16;
                break;
            case CMETA_VECTOR_U16:
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = (int32_t)scalar.u16;
                break;
            case CMETA_VECTOR_I32:
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = scalar.i32;
                break;
            case CMETA_VECTOR_I64:
                out.kind = TURBOWASM_VALUE_I64;
                out.as.i64 = scalar.i64;
                break;
            case CMETA_VECTOR_F32:
                out.kind = TURBOWASM_VALUE_F32;
                out.as.f32 = scalar.f32;
                break;
            case CMETA_VECTOR_F64:
                out.kind = TURBOWASM_VALUE_F64;
                out.as.f64 = scalar.f64;
                break;
            default:
                return TURBOWASM_UNSUPPORTED;
        }
        return turbowasm_stack_push(stack, out);
    }

    if (descriptor->kind != TURBOWASM_SIMD_EXEC_LANE_REPLACE)
        return TURBOWASM_UNSUPPORTED;

    switch (descriptor->vector_desc->lane_kind) {
        case CMETA_VECTOR_I8:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.i8 = (int8_t)scalar_value.as.i32;
            break;
        case CMETA_VECTOR_I16:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.i16 = (int16_t)scalar_value.as.i32;
            break;
        case CMETA_VECTOR_I32:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.i32 = scalar_value.as.i32;
            break;
        case CMETA_VECTOR_I64:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I64, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.i64 = scalar_value.as.i64;
            break;
        case CMETA_VECTOR_F32:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_F32, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.f32 = scalar_value.as.f32;
            break;
        case CMETA_VECTOR_F64:
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_F64, &scalar_value);
            if (status != TURBOWASM_OK) return status;
            scalar.f64 = scalar_value.as.f64;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    status = turbowasm_stack_pop_kind(
        stack, TURBOWASM_VALUE_V128, &left);
    if (status != TURBOWASM_OK)
        return status;

    out.kind = TURBOWASM_VALUE_V128;
    out.as.v128.shape = descriptor->result_shape;
    supported = salts_simd_replace_lane(
        descriptor->vector_desc,
        &out.as.v128.bits,
        &left.as.v128.bits,
        lane, scalar);
    if (!supported)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_stack_push(stack, out);
}

static turbowasm_status turbowasm_exec_simd(
    turbowasm_instance_impl *instance,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap) {
    uint32_t subopcode;
    turbowasm_status status;

    if (instance == NULL || reader == NULL ||
        stack == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &subopcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (subopcode) {
        case 0x00u: { /* v128.load */
            uint32_t offset;
            turbowasm_value address;
            turbowasm_value out = {0};
            uint8_t *source;

            status = turbowasm_exec_read_memarg(reader, &offset);
            if (status != TURBOWASM_OK)
                return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &address);
            if (status != TURBOWASM_OK)
                return status;

            status = turbowasm_instance_memory_bounds(
                instance, 0u,
                (uint32_t)address.as.i32,
                offset, 16u, &source);
            if (status == TURBOWASM_TRAPPED) {
                *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
                return TURBOWASM_TRAPPED;
            }
            if (status != TURBOWASM_OK)
                return status;

            out.kind = TURBOWASM_VALUE_V128;
            out.as.v128.shape = TURBOWASM_V128_RAW;
            salts_simd_v128_load(&out.as.v128.bits, source);
            return turbowasm_stack_push(stack, out);
        }

        case 0x0bu: { /* v128.store */
            uint32_t offset;
            turbowasm_value value;
            turbowasm_value address;
            uint8_t *destination;

            status = turbowasm_exec_read_memarg(reader, &offset);
            if (status != TURBOWASM_OK)
                return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_V128, &value);
            if (status != TURBOWASM_OK)
                return status;
            status = turbowasm_stack_pop_kind(
                stack, TURBOWASM_VALUE_I32, &address);
            if (status != TURBOWASM_OK)
                return status;

            status = turbowasm_instance_memory_bounds(
                instance, 0u,
                (uint32_t)address.as.i32,
                offset, 16u, &destination);
            if (status == TURBOWASM_TRAPPED) {
                *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
                return TURBOWASM_TRAPPED;
            }
            if (status != TURBOWASM_OK)
                return status;

            salts_simd_v128_store(
                destination, &value.as.v128.bits);
            return TURBOWASM_OK;
        }

        case 0x0cu: { /* v128.const */
            turbowasm_reader bytes;
            turbowasm_value out = {0};

            if (!turbowasm_reader_slice(reader, 16u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;

            out.kind = TURBOWASM_VALUE_V128;
            out.as.v128.shape = TURBOWASM_V128_RAW;
            salts_simd_v128_load(
                &out.as.v128.bits, bytes.cursor);
            return turbowasm_stack_push(stack, out);
        }

        default: {
            const turbowasm_simd_exec_descriptor *descriptor =
                turbowasm_simd_exec_descriptor_find(subopcode);
            if (descriptor == NULL)
                return TURBOWASM_UNSUPPORTED;
            if (turbowasm_simd_exec_is_memory(descriptor->kind))
                return turbowasm_exec_simd_memory(
                    instance, reader, stack, trap, descriptor);
            if (turbowasm_simd_exec_is_lane_special(descriptor->kind))
                return turbowasm_exec_simd_lane_special(
                    reader, stack, descriptor);
            return turbowasm_exec_simd_generic(
                descriptor, stack);
        }
    }
}


static turbowasm_status turbowasm_exec_function(
    turbowasm_instance_impl *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    uint32_t depth) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_context *context;
    const turbowasm_validation_function *function;
    const turbowasm_validation_func_type *type;
    turbowasm_value *locals = NULL;
    turbowasm_value_stack stack = {0};
    turbowasm_exec_control_stack controls = {0};
    turbowasm_reader reader;
    uint32_t index;
    turbowasm_status status = TURBOWASM_OK;
    bool finished = false;
    bool returned = false;

    if (instance == NULL || result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= TURBOWASM_EXEC_MAX_CALL_DEPTH) {
        *trap = TURBOWASM_TRAP_CALL_STACK_EXHAUSTED;
        return TURBOWASM_TRAPPED;
    }

    context = &module->validation;
    function = turbowasm_validation_context_function(
        context, function_index);
    type = turbowasm_validation_context_function_type(
        context, function_index);

    if (function == NULL || type == NULL || !type->defined)
        return TURBOWASM_INVALID_ARGUMENT;
    if (function->imported)
        return TURBOWASM_UNSUPPORTED;

    if (argument_count != type->param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count > result_capacity)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->param_count != 0u && arguments == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count != 0u && results == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < type->param_count; ++index) {
        if (!turbowasm_value_matches_type(
                &arguments[index], type->params[index]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    if (function->local_count != 0u) {
        locals = (turbowasm_value *)calloc(
            (size_t)function->local_count, sizeof(*locals));
        if (locals == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < function->local_count; ++index) {
        if (!turbowasm_zero_value(function->local_types[index],
                                  &locals[index])) {
            status = TURBOWASM_UNSUPPORTED;
            goto done;
        }
    }

    for (index = 0u; index < type->param_count; ++index)
        locals[index] = arguments[index];

    turbowasm_reader_init(&reader, function->code, function->code_size);

    status = turbowasm_exec_control_push_function(&controls, type);
    if (status != TURBOWASM_OK)
        goto done;

    while (turbowasm_reader_remaining(&reader) != 0u && !finished) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&reader, &opcode)) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }

        switch (opcode) {
            case 0x00u: /* unreachable */
                *trap = TURBOWASM_TRAP_UNREACHABLE;
                status = TURBOWASM_TRAPPED;
                goto done;
            case 0x01u: /* nop */
                break;
            case 0x02u: /* block */
            case 0x03u: /* loop */
            case 0x04u: { /* if */
                uint32_t opcode_offset;
                const turbowasm_validation_control *annotation;
                turbowasm_exec_block_signature signature;
                turbowasm_exec_control_kind kind;
                turbowasm_value condition = {0};

                if (reader.cursor <= function->code ||
                    (size_t)(reader.cursor - function->code - 1u) >
                        UINT32_MAX) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                opcode_offset =
                    (uint32_t)(reader.cursor - function->code - 1u);
                annotation =
                    turbowasm_validation_function_control_at(
                        function, opcode_offset);
                if (annotation == NULL ||
                    annotation->end_offset == UINT32_MAX) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                status = turbowasm_exec_read_block_signature(
                    &reader, context, &signature);
                if (status != TURBOWASM_OK)
                    goto done;
                if ((size_t)(reader.cursor - function->code) >
                        UINT32_MAX ||
                    (uint32_t)(reader.cursor - function->code) !=
                        annotation->body_offset) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                kind = opcode == 0x02u
                    ? TURBOWASM_EXEC_CONTROL_BLOCK
                    : opcode == 0x03u
                        ? TURBOWASM_EXEC_CONTROL_LOOP
                        : TURBOWASM_EXEC_CONTROL_IF;

                if (opcode == 0x04u) {
                    status = turbowasm_stack_pop_kind(
                        &stack, TURBOWASM_VALUE_I32, &condition);
                    if (status != TURBOWASM_OK)
                        goto done;
                }

                status = turbowasm_exec_control_push(
                    &controls, &stack, kind,
                    annotation, &signature);
                if (status != TURBOWASM_OK)
                    goto done;

                if (opcode == 0x04u && condition.as.i32 == 0) {
                    if (annotation->else_offset != UINT32_MAX) {
                        status = turbowasm_exec_jump(
                            &reader, function,
                            annotation->else_offset + 1u);
                    } else {
                        status = turbowasm_exec_jump(
                            &reader, function,
                            annotation->end_offset);
                    }
                    if (status != TURBOWASM_OK)
                        goto done;
                }
                break;
            }
            case 0x05u: { /* else */
                uint32_t opcode_offset;
                turbowasm_exec_control_frame *frame;

                if (controls.size <= 1u ||
                    reader.cursor <= function->code ||
                    (size_t)(reader.cursor - function->code - 1u) >
                        UINT32_MAX) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                opcode_offset =
                    (uint32_t)(reader.cursor - function->code - 1u);
                frame = &controls.frames[controls.size - 1u];
                if (frame->kind != TURBOWASM_EXEC_CONTROL_IF ||
                    frame->annotation == NULL ||
                    frame->annotation->else_offset != opcode_offset ||
                    frame->annotation->end_offset == UINT32_MAX) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                status = turbowasm_exec_jump(
                    &reader, function,
                    frame->annotation->end_offset);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x0bu: /* end */
                if (controls.size == 1u) {
                    if (turbowasm_reader_remaining(&reader) != 0u) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        goto done;
                    }
                    finished = true;
                } else {
                    uint32_t opcode_offset;
                    turbowasm_exec_control_frame *frame;

                    if (reader.cursor <= function->code ||
                        (size_t)(reader.cursor - function->code - 1u) >
                            UINT32_MAX) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        goto done;
                    }
                    opcode_offset =
                        (uint32_t)(reader.cursor - function->code - 1u);
                    frame = &controls.frames[controls.size - 1u];
                    if (frame->annotation == NULL ||
                        frame->annotation->end_offset != opcode_offset) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        goto done;
                    }

                    status = turbowasm_exec_finish_control(
                        &stack, &controls);
                    if (status != TURBOWASM_OK)
                        goto done;
                }
                break;
            case 0x0cu: /* br */
            case 0x0du: { /* br_if */
                uint32_t branch_depth;
                turbowasm_value condition = {0};

                if (!turbowasm_reader_uleb32(
                        &reader, &branch_depth)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                if (opcode == 0x0du) {
                    status = turbowasm_stack_pop_kind(
                        &stack, TURBOWASM_VALUE_I32, &condition);
                    if (status != TURBOWASM_OK)
                        goto done;
                    if (condition.as.i32 == 0)
                        break;
                }

                status = turbowasm_exec_branch(
                    &stack, &controls, &reader, function,
                    branch_depth, &finished, &returned);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x0eu: /* br_table */
                status = turbowasm_exec_br_table(
                    &reader, &stack, &controls, function,
                    &finished, &returned);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            case 0x0fu: /* return */
                status = turbowasm_exec_branch(
                    &stack, &controls, &reader, function,
                    controls.size - 1u, &finished, &returned);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            case 0x10u: /* call */
                status = turbowasm_exec_direct_call(
                    instance, &reader, &stack, trap, depth);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            case 0x11u: /* call_indirect */
                status = turbowasm_exec_indirect_call(
                    instance, &reader, &stack, trap, depth);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            case 0x1au: { /* drop */
                turbowasm_value ignored;
                status = turbowasm_stack_pop(&stack, &ignored);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x20u: /* local.get */
            case 0x21u: /* local.set */
            case 0x22u: { /* local.tee */
                uint32_t local_index;
                turbowasm_value value;

                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                if (opcode == 0x20u) {
                    status = turbowasm_stack_push(
                        &stack, locals[local_index]);
                } else {
                    status = turbowasm_stack_pop(&stack, &value);
                    if (status != TURBOWASM_OK)
                        goto done;
                    if (value.kind != locals[local_index].kind) {
                        status = TURBOWASM_TYPE_MISMATCH;
                        goto done;
                    }
                    locals[local_index] = value;
                    if (opcode == 0x22u)
                        status = turbowasm_stack_push(&stack, value);
                }
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }
            case 0x23u: /* global.get */
            case 0x24u: { /* global.set */
                uint32_t global_index;
                turbowasm_value value;

                if (!turbowasm_reader_uleb32(
                        &reader, &global_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                if (opcode == 0x23u) {
                    status = turbowasm_instance_global_get(
                        instance, global_index, &value);
                    if (status == TURBOWASM_OK)
                        status = turbowasm_stack_push(
                            &stack, value);
                } else {
                    status = turbowasm_stack_pop(
                        &stack, &value);
                    if (status == TURBOWASM_OK)
                        status = turbowasm_instance_global_set(
                            instance, global_index, value);
                }

                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }

            case 0x25u: /* table.get */
            case 0x26u: { /* table.set */
                uint32_t table_index;
                turbowasm_value element_index;
                turbowasm_value value;

                if (!turbowasm_reader_uleb32(
                        &reader, &table_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                if (opcode == 0x25u) {
                    status = turbowasm_stack_pop_kind(
                        &stack, TURBOWASM_VALUE_I32,
                        &element_index);
                    if (status != TURBOWASM_OK)
                        goto done;
                    status = turbowasm_instance_table_get_value(
                        instance, table_index,
                        (uint32_t)element_index.as.i32,
                        &value);
                    if (status == TURBOWASM_TRAPPED) {
                        *trap = TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS;
                        status = TURBOWASM_TRAPPED;
                        goto done;
                    }
                    if (status != TURBOWASM_OK)
                        goto done;
                    status = turbowasm_stack_push(&stack, value);
                } else {
                    status = turbowasm_stack_pop_kind(
                        &stack, TURBOWASM_VALUE_FUNCREF, &value);
                    if (status != TURBOWASM_OK)
                        goto done;
                    status = turbowasm_stack_pop_kind(
                        &stack, TURBOWASM_VALUE_I32,
                        &element_index);
                    if (status != TURBOWASM_OK)
                        goto done;
                    status = turbowasm_instance_table_set_value(
                        instance, table_index,
                        (uint32_t)element_index.as.i32,
                        value);
                    if (status == TURBOWASM_TRAPPED) {
                        *trap = TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS;
                        status = TURBOWASM_TRAPPED;
                        goto done;
                    }
                }

                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }

            case 0x28u: case 0x29u:
            case 0x2au: case 0x2bu:
            case 0x2cu: case 0x2du:
            case 0x2eu: case 0x2fu:
            case 0x30u: case 0x31u:
            case 0x32u: case 0x33u:
            case 0x34u: case 0x35u:
                status = turbowasm_exec_memory_load(
                    instance, &reader, &stack, trap, opcode);
                if (status != TURBOWASM_OK)
                    goto done;
                break;

            case 0x36u: case 0x37u:
            case 0x38u: case 0x39u:
            case 0x3au: case 0x3bu:
            case 0x3cu: case 0x3du:
            case 0x3eu:
                status = turbowasm_exec_memory_store(
                    instance, &reader, &stack, trap, opcode);
                if (status != TURBOWASM_OK)
                    goto done;
                break;

            case 0x3fu: /* memory.size */
            case 0x40u: /* memory.grow */
                status = turbowasm_exec_memory_size_or_grow(
                    instance, &reader, &stack, opcode);
                if (status != TURBOWASM_OK)
                    goto done;
                break;

            case 0x41u: {
                int32_t value;
                turbowasm_value out = {0};
                if (!turbowasm_reader_sleb32(&reader, &value)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = value;
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK) goto done;
                break;
            }
            case 0x42u: {
                int64_t value;
                turbowasm_value out = {0};
                if (!turbowasm_reader_sleb64(&reader, &value)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                out.kind = TURBOWASM_VALUE_I64;
                out.as.i64 = value;
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK) goto done;
                break;
            }
            case 0x43u: {
                uint32_t bits;
                turbowasm_value out = {0};
                if (!turbowasm_reader_u32le(&reader, &bits)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                out.kind = TURBOWASM_VALUE_F32;
                memcpy(&out.as.f32, &bits, sizeof(bits));
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK) goto done;
                break;
            }
            case 0x44u: {
                turbowasm_reader bytes;
                uint64_t bits = 0u;
                turbowasm_value out = {0};

                if (!turbowasm_reader_slice(
                        &reader, 8u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                for (index = 0u; index < 8u; ++index)
                    bits |= (uint64_t)bytes.cursor[index] << (8u * index);
                out.kind = TURBOWASM_VALUE_F64;
                memcpy(&out.as.f64, &bits, sizeof(bits));
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK) goto done;
                break;
            }

            case 0x6au: case 0x6bu: case 0x6cu:
            case 0x6du: case 0x6eu: case 0x6fu: case 0x70u:
            case 0x71u: case 0x72u: case 0x73u:
            case 0x74u: case 0x75u: case 0x76u:
            case 0x77u: case 0x78u:
                status = turbowasm_exec_i32_binary(
                    opcode, &stack, trap);
                if (status != TURBOWASM_OK) goto done;
                break;

            case 0x7cu: case 0x7du: case 0x7eu:
            case 0x7fu: case 0x80u: case 0x81u: case 0x82u:
            case 0x83u: case 0x84u: case 0x85u:
            case 0x86u: case 0x87u: case 0x88u:
            case 0x89u: case 0x8au:
                status = turbowasm_exec_i64_binary(
                    opcode, &stack, trap);
                if (status != TURBOWASM_OK) goto done;
                break;

            case 0x92u: case 0x93u: case 0x94u: case 0x95u:
                status = turbowasm_exec_f32_binary(
                    opcode, &stack);
                if (status != TURBOWASM_OK) goto done;
                break;

            case 0xa0u: case 0xa1u: case 0xa2u: case 0xa3u:
                status = turbowasm_exec_f64_binary(
                    opcode, &stack);
                if (status != TURBOWASM_OK) goto done;
                break;

            case 0xd0u: { /* ref.null */
                uint8_t reference_type;
                turbowasm_value out = {0};

                if (!turbowasm_reader_u8(
                        &reader, &reference_type)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                if (reference_type != 0x70u) {
                    status = TURBOWASM_UNSUPPORTED;
                    goto done;
                }

                out.kind = TURBOWASM_VALUE_FUNCREF;
                out.as.funcref.is_null = true;
                out.as.funcref.function_index = UINT32_MAX;
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }

            case 0xd1u: { /* ref.is_null */
                turbowasm_value reference;
                turbowasm_value out = {0};

                status = turbowasm_stack_pop_kind(
                    &stack, TURBOWASM_VALUE_FUNCREF,
                    &reference);
                if (status != TURBOWASM_OK)
                    goto done;

                out.kind = TURBOWASM_VALUE_I32;
                out.as.i32 = reference.as.funcref.is_null ? 1 : 0;
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }

            case 0xd2u: { /* ref.func */
                uint32_t function_ref;
                turbowasm_value out = {0};

                if (!turbowasm_reader_uleb32(
                        &reader, &function_ref) ||
                    function_ref >= context->function_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }

                out.kind = TURBOWASM_VALUE_FUNCREF;
                out.as.funcref.is_null = false;
                out.as.funcref.function_index = function_ref;
                status = turbowasm_stack_push(&stack, out);
                if (status != TURBOWASM_OK)
                    goto done;
                break;
            }

            case 0xfcu:
                status = turbowasm_exec_fc(
                    instance, &reader, &stack, trap);
                if (status != TURBOWASM_OK)
                    goto done;
                break;

            case 0xfdu:
                status = turbowasm_exec_simd(
                    instance, &reader, &stack, trap);
                if (status != TURBOWASM_OK)
                    goto done;
                break;

            default:
                status = TURBOWASM_UNSUPPORTED;
                goto done;
        }
    }

    if (!finished) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    if ((!returned && stack.size != type->result_count) ||
        (returned && stack.size < type->result_count)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    for (index = type->result_count; index != 0u; ) {
        --index;
        status = turbowasm_stack_pop(
            &stack, &results[index]);
        if (status != TURBOWASM_OK)
            goto done;
        if (!turbowasm_value_matches_type(
                &results[index], type->results[index])) {
            status = TURBOWASM_TYPE_MISMATCH;
            goto done;
        }
    }

    *result_count = type->result_count;

done:
    free(locals);
    free(stack.values);
    free(controls.frames);
    return status;
}

turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module) {
    turbowasm_instance_impl *impl;

    if (instance == NULL || module == NULL ||
        module->impl == NULL || instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->module = module;
    {
        const turbowasm_module_impl *module_impl =
            turbowasm_module_impl_get(module);
        turbowasm_status status;
        if (module_impl == NULL) {
            free(impl);
            return TURBOWASM_INVALID_ARGUMENT;
        }
        status = turbowasm_instance_state_init(impl, module_impl);
        if (status != TURBOWASM_OK) {
            free(impl);
            return status;
        }
    }
    instance->impl = impl;

    {
        const turbowasm_module_impl *module_impl =
            turbowasm_module_impl_get(module);
        if (module_impl != NULL &&
            module_impl->summary.has_start) {
            size_t result_count = 0u;
            turbowasm_trap trap = TURBOWASM_TRAP_NONE;
            turbowasm_status status = turbowasm_instance_invoke(
                instance,
                module_impl->summary.start_function_index,
                NULL, 0u,
                NULL, 0u,
                &result_count,
                &trap);
            if (status != TURBOWASM_OK) {
                turbowasm_instance_destroy(instance);
                return status;
            }
        }
    }

    return TURBOWASM_OK;
}

void turbowasm_instance_destroy(turbowasm_instance *instance) {
    turbowasm_instance_impl *impl;
    if (instance == NULL || instance->impl == NULL)
        return;
    impl = (turbowasm_instance_impl *)instance->impl;
    turbowasm_instance_state_destroy(impl);
    free(impl);
    instance->impl = NULL;
}

const turbowasm_module *turbowasm_instance_module(
    const turbowasm_instance *instance) {
    const turbowasm_instance_impl *impl;

    if (instance == NULL || instance->impl == NULL)
        return NULL;
    impl = (const turbowasm_instance_impl *)instance->impl;
    return impl->module;
}

turbowasm_status turbowasm_instance_invoke(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_instance_impl *impl;

    if (instance == NULL || instance->impl == NULL ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    impl = (turbowasm_instance_impl *)instance->impl;

    return turbowasm_exec_function(
        impl,
        function_index,
        arguments,
        argument_count,
        results,
        result_capacity,
        result_count,
        trap,
        0u);
}

const char *turbowasm_trap_string(turbowasm_trap trap) {
    switch (trap) {
        case TURBOWASM_TRAP_NONE: return "none";
        case TURBOWASM_TRAP_UNREACHABLE: return "unreachable";
        case TURBOWASM_TRAP_CALL_STACK_EXHAUSTED:
            return "call_stack_exhausted";
        case TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO:
            return "integer_divide_by_zero";
        case TURBOWASM_TRAP_INTEGER_OVERFLOW:
            return "integer_overflow";
        case TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS:
            return "memory_out_of_bounds";
        case TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS:
            return "table_out_of_bounds";
        case TURBOWASM_TRAP_INDIRECT_CALL_NULL:
            return "indirect_call_null";
        case TURBOWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH:
            return "indirect_call_type_mismatch";
        default: return "unknown";
    }
}
