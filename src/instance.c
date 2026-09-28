#include <turbowasm/instance.h>

#include "instance_internal.h"
#include "link_internal.h"
#include "module_internal.h"
#include "reader.h"
#include "simd_exec_table.h"
#include "validate_type.h"

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

static turbowasm_status turbowasm_execution_checkpoint(
    turbowasm_jit_execution_control *execution) {
    if (execution == NULL)
        return TURBOWASM_OK;

    if (execution->should_interrupt != NULL &&
        execution->should_interrupt(execution->interrupt_context))
        return TURBOWASM_INTERRUPTED;

    if (execution->fuel_limited) {
        if (execution->fuel_remaining == 0u)
            return TURBOWASM_FUEL_EXHAUSTED;
        --execution->fuel_remaining;
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_jit_execution_checkpoint(
    turbowasm_jit_invocation_context *context) {
    turbowasm_status status;

    if (context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_execution_checkpoint(context->execution);
    if (status != TURBOWASM_OK) {
        context->call_status = status;
        context->call_trap = TURBOWASM_TRAP_NONE;
    }
    return status;
}

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
        case 0x6fu: return TURBOWASM_VALUE_EXTERNREF;
        default: return (turbowasm_value_kind)0;
    }
}

static bool turbowasm_value_matches_type(
    const turbowasm_value *value,
    uint8_t type) {
    turbowasm_value_kind kind = turbowasm_kind_from_valtype(type);
    return value != NULL && kind != 0 && value->kind == kind;
}

turbowasm_status turbowasm_jit_request_tail_call(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_func_type *type;
    size_t index;

    if (context == NULL || context->instance == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    context->tail_call_pending = false;
    context->tail_argument_count = 0u;

    if (argument_count > TURBOWASM_JIT_TAIL_ARGUMENT_LIMIT ||
        (argument_count != 0u && arguments == NULL)) {
        context->call_status = TURBOWASM_UNSUPPORTED;
        context->call_trap = TURBOWASM_TRAP_NONE;
        return TURBOWASM_UNSUPPORTED;
    }

    module = turbowasm_module_impl_get(context->instance->module);
    if (module == NULL) {
        context->call_status = TURBOWASM_INVALID_ARGUMENT;
        context->call_trap = TURBOWASM_TRAP_NONE;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    type = turbowasm_validation_context_function_type(
        &module->validation, function_index);
    if (type == NULL || !type->defined ||
        argument_count != type->param_count) {
        context->call_status = TURBOWASM_INVALID_ARGUMENT;
        context->call_trap = TURBOWASM_TRAP_NONE;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    for (index = 0u; index < argument_count; ++index) {
        if (!turbowasm_value_matches_type(
                &arguments[index], type->params[index])) {
            context->call_status = TURBOWASM_TYPE_MISMATCH;
            context->call_trap = TURBOWASM_TRAP_NONE;
            return TURBOWASM_TYPE_MISMATCH;
        }
        context->tail_arguments[index] = arguments[index];
    }

    context->tail_function_index = function_index;
    context->tail_argument_count = argument_count;
    context->tail_call_pending = true;
    context->call_status = TURBOWASM_OK;
    context->call_trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
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
        out->as.funcref.owner = NULL;
    } else if (kind == TURBOWASM_VALUE_EXTERNREF) {
        out->as.externref.is_null = true;
        out->as.externref.token = 0u;
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
    if (turbowasm_reader_remaining(reader) == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    first = *reader->cursor;
    if (first == 0x40u) {
        if (!turbowasm_reader_u8(reader, &first))
            return TURBOWASM_MALFORMED_MODULE;
        return TURBOWASM_OK;
    }

    if (turbowasm_exec_valtype(first) ||
        first == 0x63u || first == 0x64u) {
        turbowasm_validation_value_type type;
        bool generalized = false;
        turbowasm_status status =
            turbowasm_validation_read_valtype(
                reader, &type, &generalized);

        if (status != TURBOWASM_OK)
            return status;
        if (type.heap_kind ==
                TURBOWASM_VALIDATION_HEAP_TYPE_INDEX &&
            type.type_index >= context->type_count)
            return TURBOWASM_MALFORMED_MODULE;

        signature->inline_end_type = type.carrier;
        signature->has_inline_end_type = true;
        return TURBOWASM_OK;
    }

    if (!turbowasm_reader_u8(reader, &first))
        return TURBOWASM_MALFORMED_MODULE;

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
    turbowasm_jit_execution_control *execution,
    uint32_t depth);

static turbowasm_status turbowasm_dispatch_function(
    turbowasm_instance_impl *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    turbowasm_jit_execution_control *execution,
    uint32_t depth) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_function *function;
    turbowasm_jit_function_state *entry;
    turbowasm_value tail_arguments[TURBOWASM_JIT_TAIL_ARGUMENT_LIMIT];
    turbowasm_status status;

    if (instance == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

dispatch_again:
    if (!instance->jit_backend_attached ||
        instance->jit_functions == NULL ||
        function_index >= instance->jit_function_count ||
        (execution != NULL &&
         !instance->jit_backend.supports_execution_control)) {
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    entry = &instance->jit_functions[function_index];

    if (entry->state == TURBOWASM_JIT_COMPILED)
        goto invoke_compiled;

    if (entry->state == TURBOWASM_JIT_INTERPRET_ONLY) {
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    if (entry->call_count != UINT32_MAX)
        ++entry->call_count;

    if (entry->call_count < instance->jit_hot_threshold) {
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL) {
        entry->state = TURBOWASM_JIT_INTERPRET_ONLY;
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    function = turbowasm_validation_context_function(
        &module->validation, function_index);
    if (function == NULL || function->imported ||
        !instance->jit_backend.is_function_eligible(
            instance->jit_backend.context,
            &module->validation,
            function_index,
            function)) {
        entry->state = TURBOWASM_JIT_INTERPRET_ONLY;
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    status = instance->jit_backend.compile_function(
        instance->jit_backend.context,
        &module->validation,
        function_index,
        function,
        &entry->compiled);
    if (status != TURBOWASM_OK ||
        entry->compiled.impl == NULL) {
        if (entry->compiled.impl != NULL) {
            instance->jit_backend.destroy_function(
                instance->jit_backend.context,
                &entry->compiled);
        }
        entry->state = TURBOWASM_JIT_INTERPRET_ONLY;
        return turbowasm_exec_function(
            instance, function_index,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap,
            execution, depth);
    }

    entry->state = TURBOWASM_JIT_COMPILED;

invoke_compiled:
    {
        turbowasm_jit_invocation_context context = {
            instance, execution, depth,
            TURBOWASM_OK, TURBOWASM_TRAP_NONE
        };

        status = instance->jit_backend.invoke(
            &entry->compiled,
            &context,
            arguments, argument_count,
            results, result_capacity,
            result_count, trap);
        if (status != TURBOWASM_OK ||
            !context.tail_call_pending)
            return status;

        if (context.tail_argument_count >
                TURBOWASM_JIT_TAIL_ARGUMENT_LIMIT) {
            return TURBOWASM_UNSUPPORTED;
        }

        if (context.tail_argument_count != 0u) {
            memcpy(
                tail_arguments,
                context.tail_arguments,
                context.tail_argument_count *
                    sizeof(tail_arguments[0]));
            arguments = tail_arguments;
        } else {
            arguments = NULL;
        }

        function_index = context.tail_function_index;
        argument_count = context.tail_argument_count;
        if (result_count != NULL)
            *result_count = 0u;
        if (trap != NULL)
            *trap = TURBOWASM_TRAP_NONE;

        /*
         * The generated caller has fully unwound before this jump.  Re-enter
         * dispatch at the same logical depth, so a compiled tail-call chain
         * is a trampoline rather than recursive C/native calls.
         */
        goto dispatch_again;
    }
}

turbowasm_status turbowasm_jit_instance_attach_backend(
    turbowasm_instance_impl *instance,
    turbowasm_jit_backend *backend,
    uint32_t hot_threshold) {
    const turbowasm_module_impl *module;
    turbowasm_jit_function_state *states = NULL;

    if (instance == NULL || backend == NULL ||
        backend->context == NULL ||
        backend->is_function_eligible == NULL ||
        backend->compile_function == NULL ||
        backend->invoke == NULL ||
        backend->destroy_function == NULL ||
        backend->destroy_backend == NULL ||
        hot_threshold == 0u ||
        instance->jit_backend_attached)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (module->validation.function_count != 0u) {
        states = (turbowasm_jit_function_state *)calloc(
            (size_t)module->validation.function_count,
            sizeof(*states));
        if (states == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    instance->jit_backend = *backend;
    memset(backend, 0, sizeof(*backend));
    instance->jit_functions = states;
    instance->jit_function_count =
        module->validation.function_count;
    instance->jit_hot_threshold = hot_threshold;
    instance->jit_backend_attached = true;
    return TURBOWASM_OK;
}

void turbowasm_jit_instance_detach_backend(
    turbowasm_instance_impl *instance) {
    uint32_t index;

    if (instance == NULL || !instance->jit_backend_attached)
        return;

    if (instance->jit_functions != NULL) {
        for (index = 0u;
             index < instance->jit_function_count;
             ++index) {
            turbowasm_jit_function_state *entry =
                &instance->jit_functions[index];
            if (entry->compiled.impl != NULL) {
                instance->jit_backend.destroy_function(
                    instance->jit_backend.context,
                    &entry->compiled);
            }
        }
    }

    free(instance->jit_functions);
    instance->jit_functions = NULL;
    instance->jit_function_count = 0u;
    instance->jit_hot_threshold = 0u;

    instance->jit_backend.destroy_backend(
        instance->jit_backend.context);
    memset(&instance->jit_backend, 0,
           sizeof(instance->jit_backend));
    instance->jit_backend_attached = false;
}

turbowasm_status turbowasm_jit_direct_call(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_func_type *type;

    if (context == NULL || context->instance == NULL ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(context->instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_validation_context_function_type(
        &module->validation, function_index);
    if (type == NULL || !type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    if (argument_count != type->param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count > result_capacity)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->param_count != 0u && arguments == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count != 0u && results == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_dispatch_function(
        context->instance,
        function_index,
        arguments,
        argument_count,
        results,
        result_capacity,
        result_count,
        trap,
        context->execution,
        context->depth + 1u);
}

static turbowasm_status turbowasm_instance_create_internal(
    turbowasm_instance *instance,
    const turbowasm_module *module,
    const turbowasm_linker *linker,
    bool resolve_imports) {
    const turbowasm_module_impl *module_impl;
    turbowasm_instance_impl *impl;
    turbowasm_status status;

    if (instance == NULL || module == NULL ||
        module->impl == NULL || instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (resolve_imports &&
        (linker == NULL || linker->impl == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    module_impl = turbowasm_module_impl_get(module);
    if (module_impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->module = module;

    if (resolve_imports) {
        status = turbowasm_linker_bind_instance(
            impl, module_impl, linker);
        if (status != TURBOWASM_OK) {
            free(impl->linked_functions);
            free(impl->linked_globals);
            free(impl->linked_memories);
            free(impl->linked_tables);
            free(impl);
            return status;
        }
    }

    status = turbowasm_instance_state_init(impl, module_impl);
    if (status != TURBOWASM_OK) {
        free(impl->linked_functions);
        free(impl->linked_globals);
        free(impl->linked_memories);
        free(impl->linked_tables);
        free(impl);
        return status;
    }

    instance->impl = impl;

    if (module_impl->summary.has_start) {
        size_t result_count = 0u;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;

        status = turbowasm_instance_invoke(
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

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module) {
    return turbowasm_instance_create_internal(
        instance, module, NULL, false);
}

turbowasm_status turbowasm_instance_create_linked(
    turbowasm_instance *instance,
    const turbowasm_module *module,
    const struct turbowasm_linker *linker) {
    return turbowasm_instance_create_internal(
        instance, module, linker, true);
}

void turbowasm_instance_destroy(turbowasm_instance *instance) {
    turbowasm_instance_impl *impl;
    if (instance == NULL || instance->impl == NULL)
        return;
    impl = (turbowasm_instance_impl *)instance->impl;
    turbowasm_jit_instance_detach_backend(impl);
    turbowasm_instance_state_destroy(impl);
    free(impl->linked_functions);
    impl->linked_functions = NULL;
    impl->linked_function_count = 0u;
    free(impl->linked_globals);
    impl->linked_globals = NULL;
    impl->linked_global_count = 0u;
    free(impl->linked_memories);
    impl->linked_memories = NULL;
    impl->linked_memory_count = 0u;
    free(impl->linked_tables);
    impl->linked_tables = NULL;
    impl->linked_table_count = 0u;
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
    return turbowasm_instance_invoke_with_options(
        instance,
        function_index,
        arguments,
        argument_count,
        results,
        result_capacity,
        result_count,
        trap,
        NULL);
}

turbowasm_status turbowasm_instance_invoke_with_options(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    const turbowasm_execution_options *options) {
    turbowasm_instance_impl *impl;
    turbowasm_jit_execution_control execution = {0};
    turbowasm_jit_execution_control *execution_ptr = NULL;

    if (instance == NULL || instance->impl == NULL ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    if (options != NULL) {
        execution.fuel_remaining = options->fuel;
        execution.fuel_limited = options->has_fuel_limit;
        execution.should_interrupt = options->should_interrupt;
        execution.interrupt_context = options->interrupt_context;
        execution_ptr = &execution;
    }

    impl = (turbowasm_instance_impl *)instance->impl;

    return turbowasm_dispatch_function(
        impl,
        function_index,
        arguments,
        argument_count,
        results,
        result_capacity,
        result_count,
        trap,
        execution_ptr,
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
        case TURBOWASM_TRAP_INVALID_CONVERSION_TO_INTEGER:
            return "invalid_conversion_to_integer";
        default: return "unknown";
    }
}
