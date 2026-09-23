#include <turbowasm/instance.h>

#include "module_internal.h"
#include "reader.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_DEFAULT_MAX_CALL_DEPTH = 1024u
};

typedef struct turbowasm_instance_impl {
    const turbowasm_module *module;
    uint32_t max_call_depth;
} turbowasm_instance_impl;

typedef struct turbowasm_exec_value_stack {
    turbowasm_value *values;
    uint32_t size;
    uint32_t capacity;
} turbowasm_exec_value_stack;

typedef struct turbowasm_exec_frame {
    uint32_t function_index;
    const turbowasm_validation_func_type *type;
    const turbowasm_validation_function *metadata;

    const uint8_t *pc;
    const uint8_t *end;

    turbowasm_value *locals;
    uint32_t local_count;

    uint32_t value_base;
} turbowasm_exec_frame;

typedef struct turbowasm_exec_frame_stack {
    turbowasm_exec_frame *frames;
    uint32_t size;
    uint32_t capacity;
} turbowasm_exec_frame_stack;

typedef struct turbowasm_exec_context {
    const turbowasm_module_impl *module;
    turbowasm_exec_value_stack values;
    turbowasm_exec_frame_stack frames;
    uint32_t max_call_depth;
    turbowasm_trap trap;
} turbowasm_exec_context;

static bool turbowasm_value_kind_for_type(
    uint8_t type,
    turbowasm_value_kind *out_kind) {
    turbowasm_value_kind kind;

    switch (type) {
        case 0x7fu: kind = TURBOWASM_VALUE_I32; break;
        case 0x7eu: kind = TURBOWASM_VALUE_I64; break;
        case 0x7du: kind = TURBOWASM_VALUE_F32; break;
        case 0x7cu: kind = TURBOWASM_VALUE_F64; break;
        case 0x7bu: kind = TURBOWASM_VALUE_V128; break;
        default:
            return false;
    }

    if (out_kind != NULL)
        *out_kind = kind;
    return true;
}

static bool turbowasm_value_matches_type(
    const turbowasm_value *value,
    uint8_t type) {
    turbowasm_value_kind expected;

    return value != NULL &&
           turbowasm_value_kind_for_type(type, &expected) &&
           value->kind == expected;
}

static bool turbowasm_value_zero(
    uint8_t type,
    turbowasm_value *out) {
    turbowasm_value_kind kind;

    if (out == NULL ||
        !turbowasm_value_kind_for_type(type, &kind))
        return false;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_VALUE_V128)
        out->as.v128.shape = TURBOWASM_V128_RAW;
    return true;
}

static bool turbowasm_exec_value_reserve(
    turbowasm_exec_value_stack *stack,
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

static turbowasm_status turbowasm_exec_push(
    turbowasm_exec_value_stack *stack,
    turbowasm_value value) {
    if (stack == NULL || stack->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_exec_value_reserve(stack, stack->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;
    stack->values[stack->size++] = value;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_pop(
    turbowasm_exec_value_stack *stack,
    turbowasm_value *out) {
    if (stack == NULL || stack->size == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    --stack->size;
    if (out != NULL)
        *out = stack->values[stack->size];
    return TURBOWASM_OK;
}

static bool turbowasm_exec_frame_reserve(
    turbowasm_exec_frame_stack *stack,
    uint32_t required) {
    uint32_t next;
    turbowasm_exec_frame *grown;

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

    if ((uint64_t)next * sizeof(*grown) > (uint64_t)SIZE_MAX)
        return false;

    grown = (turbowasm_exec_frame *)realloc(
        stack->frames, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    memset(grown + stack->capacity, 0,
           (size_t)(next - stack->capacity) * sizeof(*grown));
    stack->frames = grown;
    stack->capacity = next;
    return true;
}

static void turbowasm_exec_frame_destroy(
    turbowasm_exec_frame *frame) {
    if (frame == NULL)
        return;
    free(frame->locals);
    memset(frame, 0, sizeof(*frame));
}

static void turbowasm_exec_context_destroy(
    turbowasm_exec_context *context) {
    uint32_t index;

    if (context == NULL)
        return;

    for (index = 0u; index < context->frames.size; ++index)
        turbowasm_exec_frame_destroy(
            &context->frames.frames[index]);

    free(context->frames.frames);
    free(context->values.values);
    memset(context, 0, sizeof(*context));
}

static turbowasm_exec_frame *turbowasm_exec_top_frame(
    turbowasm_exec_context *context) {
    if (context == NULL || context->frames.size == 0u)
        return NULL;
    return &context->frames.frames[context->frames.size - 1u];
}

static turbowasm_status turbowasm_exec_make_frame(
    turbowasm_exec_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    uint32_t argument_count,
    uint32_t value_base,
    turbowasm_exec_frame *out) {
    const turbowasm_validation_function *metadata;
    const turbowasm_validation_func_type *type;
    turbowasm_value *locals = NULL;
    uint32_t index;

    if (context == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    metadata = turbowasm_validation_context_function(
        &context->module->validation, function_index);
    type = turbowasm_validation_context_function_type(
        &context->module->validation, function_index);

    if (metadata == NULL || type == NULL || !type->defined)
        return TURBOWASM_MALFORMED_MODULE;
    if (metadata->imported)
        return TURBOWASM_UNSUPPORTED;
    if (metadata->code == NULL || metadata->local_types == NULL &&
        metadata->local_count != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    if (argument_count != type->param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (metadata->local_count < type->param_count)
        return TURBOWASM_MALFORMED_MODULE;
    if (argument_count != 0u && arguments == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (metadata->local_count != 0u) {
        if ((uint64_t)metadata->local_count * sizeof(*locals) >
            (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        locals = (turbowasm_value *)calloc(
            (size_t)metadata->local_count, sizeof(*locals));
        if (locals == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < metadata->local_count; ++index) {
        if (!turbowasm_value_zero(
                metadata->local_types[index], &locals[index])) {
            free(locals);
            return TURBOWASM_UNSUPPORTED;
        }
    }

    for (index = 0u; index < argument_count; ++index) {
        if (!turbowasm_value_matches_type(
                &arguments[index], type->params[index])) {
            free(locals);
            return TURBOWASM_TYPE_MISMATCH;
        }
        locals[index] = arguments[index];
    }

    memset(out, 0, sizeof(*out));
    out->function_index = function_index;
    out->type = type;
    out->metadata = metadata;
    out->pc = metadata->code;
    out->end = metadata->code + metadata->code_size;
    out->locals = locals;
    out->local_count = metadata->local_count;
    out->value_base = value_base;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_push_frame(
    turbowasm_exec_context *context,
    uint32_t function_index,
    const turbowasm_value *arguments,
    uint32_t argument_count,
    uint32_t value_base) {
    turbowasm_exec_frame frame = {0};
    turbowasm_status status;

    if (context->frames.size >= context->max_call_depth) {
        context->trap = TURBOWASM_TRAP_CALL_STACK_EXHAUSTED;
        return TURBOWASM_TRAPPED;
    }

    status = turbowasm_exec_make_frame(
        context, function_index,
        arguments, argument_count,
        value_base, &frame);
    if (status != TURBOWASM_OK)
        return status;

    if (!turbowasm_exec_frame_reserve(
            &context->frames, context->frames.size + 1u)) {
        turbowasm_exec_frame_destroy(&frame);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    context->frames.frames[context->frames.size++] = frame;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_push_call_from_stack(
    turbowasm_exec_context *context,
    uint32_t function_index) {
    const turbowasm_validation_func_type *type;
    uint32_t argument_count;
    uint32_t argument_base;
    turbowasm_status status;

    type = turbowasm_validation_context_function_type(
        &context->module->validation, function_index);
    if (type == NULL || !type->defined)
        return TURBOWASM_MALFORMED_MODULE;

    argument_count = type->param_count;
    if (context->values.size < argument_count)
        return TURBOWASM_MALFORMED_MODULE;

    argument_base = context->values.size - argument_count;

    status = turbowasm_exec_push_frame(
        context,
        function_index,
        argument_count == 0u
            ? NULL
            : &context->values.values[argument_base],
        argument_count,
        argument_base);
    if (status != TURBOWASM_OK)
        return status;

    context->values.size = argument_base;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_finish_frame(
    turbowasm_exec_context *context) {
    turbowasm_exec_frame *frame = turbowasm_exec_top_frame(context);
    uint32_t index;

    if (frame == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    if (context->values.size !=
        frame->value_base + frame->type->result_count)
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < frame->type->result_count; ++index) {
        if (!turbowasm_value_matches_type(
                &context->values.values[frame->value_base + index],
                frame->type->results[index]))
            return TURBOWASM_MALFORMED_MODULE;
    }

    turbowasm_exec_frame_destroy(frame);
    --context->frames.size;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_exec_binary_i32(
    turbowasm_exec_context *context,
    uint8_t opcode) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value result = {0};
    uint32_t a;
    uint32_t b;
    turbowasm_status status;

    status = turbowasm_exec_pop(&context->values, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_exec_pop(&context->values, &left);
    if (status != TURBOWASM_OK) return status;
    if (left.kind != TURBOWASM_VALUE_I32 ||
        right.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_MALFORMED_MODULE;

    a = (uint32_t)left.as.i32;
    b = (uint32_t)right.as.i32;
    result.kind = TURBOWASM_VALUE_I32;

    switch (opcode) {
        case 0x6au: result.as.i32 = (int32_t)(a + b); break;
        case 0x6bu: result.as.i32 = (int32_t)(a - b); break;
        case 0x6cu: result.as.i32 = (int32_t)(a * b); break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_exec_push(&context->values, result);
}

static turbowasm_status turbowasm_exec_binary_i64(
    turbowasm_exec_context *context,
    uint8_t opcode) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value result = {0};
    uint64_t a;
    uint64_t b;
    turbowasm_status status;

    status = turbowasm_exec_pop(&context->values, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_exec_pop(&context->values, &left);
    if (status != TURBOWASM_OK) return status;
    if (left.kind != TURBOWASM_VALUE_I64 ||
        right.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_MALFORMED_MODULE;

    a = (uint64_t)left.as.i64;
    b = (uint64_t)right.as.i64;
    result.kind = TURBOWASM_VALUE_I64;

    switch (opcode) {
        case 0x7cu: result.as.i64 = (int64_t)(a + b); break;
        case 0x7du: result.as.i64 = (int64_t)(a - b); break;
        case 0x7eu: result.as.i64 = (int64_t)(a * b); break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_exec_push(&context->values, result);
}

static turbowasm_status turbowasm_exec_binary_f32(
    turbowasm_exec_context *context,
    uint8_t opcode) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value result = {0};
    turbowasm_status status;

    status = turbowasm_exec_pop(&context->values, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_exec_pop(&context->values, &left);
    if (status != TURBOWASM_OK) return status;
    if (left.kind != TURBOWASM_VALUE_F32 ||
        right.kind != TURBOWASM_VALUE_F32)
        return TURBOWASM_MALFORMED_MODULE;

    result.kind = TURBOWASM_VALUE_F32;
    switch (opcode) {
        case 0x92u: result.as.f32 = left.as.f32 + right.as.f32; break;
        case 0x93u: result.as.f32 = left.as.f32 - right.as.f32; break;
        case 0x94u: result.as.f32 = left.as.f32 * right.as.f32; break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_exec_push(&context->values, result);
}

static turbowasm_status turbowasm_exec_binary_f64(
    turbowasm_exec_context *context,
    uint8_t opcode) {
    turbowasm_value right;
    turbowasm_value left;
    turbowasm_value result = {0};
    turbowasm_status status;

    status = turbowasm_exec_pop(&context->values, &right);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_exec_pop(&context->values, &left);
    if (status != TURBOWASM_OK) return status;
    if (left.kind != TURBOWASM_VALUE_F64 ||
        right.kind != TURBOWASM_VALUE_F64)
        return TURBOWASM_MALFORMED_MODULE;

    result.kind = TURBOWASM_VALUE_F64;
    switch (opcode) {
        case 0xa0u: result.as.f64 = left.as.f64 + right.as.f64; break;
        case 0xa1u: result.as.f64 = left.as.f64 - right.as.f64; break;
        case 0xa2u: result.as.f64 = left.as.f64 * right.as.f64; break;
        default: return TURBOWASM_UNSUPPORTED;
    }

    return turbowasm_exec_push(&context->values, result);
}

static turbowasm_status turbowasm_exec_step(
    turbowasm_exec_context *context) {
    turbowasm_exec_frame *frame = turbowasm_exec_top_frame(context);
    turbowasm_reader reader;
    uint8_t opcode;
    turbowasm_status status;

    if (frame == NULL || frame->pc == NULL || frame->pc > frame->end)
        return TURBOWASM_MALFORMED_MODULE;

    reader.cursor = frame->pc;
    reader.end = frame->end;

    if (!turbowasm_reader_u8(&reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    frame->pc = reader.cursor;

    switch (opcode) {
        case 0x00u: /* unreachable */
            context->trap = TURBOWASM_TRAP_UNREACHABLE;
            return TURBOWASM_TRAPPED;

        case 0x01u: /* nop */
            return TURBOWASM_OK;

        case 0x0bu: /* end */
            if (turbowasm_reader_remaining(&reader) != 0u)
                return TURBOWASM_UNSUPPORTED;
            return turbowasm_exec_finish_frame(context);

        case 0x0fu: /* return */
            return turbowasm_exec_finish_frame(context);

        case 0x10u: { /* call */
            uint32_t callee;
            if (!turbowasm_reader_uleb32(&reader, &callee))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            return turbowasm_exec_push_call_from_stack(
                context, callee);
        }

        case 0x1au: /* drop */
            return turbowasm_exec_pop(&context->values, NULL);

        case 0x20u: /* local.get */
        case 0x21u: /* local.set */
        case 0x22u: { /* local.tee */
            uint32_t index;
            turbowasm_value value;

            if (!turbowasm_reader_uleb32(&reader, &index))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            if (index >= frame->local_count)
                return TURBOWASM_MALFORMED_MODULE;

            if (opcode == 0x20u)
                return turbowasm_exec_push(
                    &context->values, frame->locals[index]);

            status = turbowasm_exec_pop(
                &context->values, &value);
            if (status != TURBOWASM_OK)
                return status;
            if (value.kind != frame->locals[index].kind)
                return TURBOWASM_MALFORMED_MODULE;

            frame->locals[index] = value;
            if (opcode == 0x22u)
                return turbowasm_exec_push(
                    &context->values, value);
            return TURBOWASM_OK;
        }

        case 0x41u: { /* i32.const */
            turbowasm_value value = {0};
            if (!turbowasm_reader_sleb32(
                    &reader, &value.as.i32))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            value.kind = TURBOWASM_VALUE_I32;
            return turbowasm_exec_push(&context->values, value);
        }

        case 0x42u: { /* i64.const */
            turbowasm_value value = {0};
            if (!turbowasm_reader_sleb64(
                    &reader, &value.as.i64))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            value.kind = TURBOWASM_VALUE_I64;
            return turbowasm_exec_push(&context->values, value);
        }

        case 0x43u: { /* f32.const */
            turbowasm_value value = {0};
            uint32_t bits;
            if (!turbowasm_reader_u32le(&reader, &bits))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            value.kind = TURBOWASM_VALUE_F32;
            memcpy(&value.as.f32, &bits, sizeof(bits));
            return turbowasm_exec_push(&context->values, value);
        }

        case 0x44u: { /* f64.const */
            turbowasm_value value = {0};
            uint32_t low;
            uint32_t high;
            uint64_t bits;
            if (!turbowasm_reader_u32le(&reader, &low) ||
                !turbowasm_reader_u32le(&reader, &high))
                return TURBOWASM_MALFORMED_MODULE;
            frame->pc = reader.cursor;
            bits = (uint64_t)low | ((uint64_t)high << 32u);
            value.kind = TURBOWASM_VALUE_F64;
            memcpy(&value.as.f64, &bits, sizeof(bits));
            return turbowasm_exec_push(&context->values, value);
        }

        case 0x6au: /* i32.add */
        case 0x6bu: /* i32.sub */
        case 0x6cu: /* i32.mul */
            return turbowasm_exec_binary_i32(context, opcode);

        case 0x7cu: /* i64.add */
        case 0x7du: /* i64.sub */
        case 0x7eu: /* i64.mul */
            return turbowasm_exec_binary_i64(context, opcode);

        case 0x92u: /* f32.add */
        case 0x93u: /* f32.sub */
        case 0x94u: /* f32.mul */
            return turbowasm_exec_binary_f32(context, opcode);

        case 0xa0u: /* f64.add */
        case 0xa1u: /* f64.sub */
        case 0xa2u: /* f64.mul */
            return turbowasm_exec_binary_f64(context, opcode);

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module) {
    const turbowasm_module_impl *module_impl;
    turbowasm_instance_impl *impl;

    if (instance == NULL || module == NULL ||
        instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module_impl = turbowasm_module_impl_get(module);
    if (module_impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (module_impl->summary.has_start)
        return TURBOWASM_UNSUPPORTED;

    impl = (turbowasm_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->module = module;
    impl->max_call_depth = TURBOWASM_DEFAULT_MAX_CALL_DEPTH;
    instance->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_instance_destroy(turbowasm_instance *instance) {
    if (instance == NULL)
        return;
    free(instance->impl);
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

turbowasm_status turbowasm_instance_call(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    uint32_t argument_count,
    turbowasm_value *results,
    uint32_t result_capacity,
    uint32_t *out_result_count,
    turbowasm_trap *out_trap) {
    turbowasm_instance_impl *instance_impl;
    const turbowasm_module_impl *module_impl;
    const turbowasm_validation_func_type *type;
    turbowasm_exec_context context = {0};
    turbowasm_status status;
    uint32_t index;

    if (out_result_count != NULL)
        *out_result_count = 0u;
    if (out_trap != NULL)
        *out_trap = TURBOWASM_TRAP_NONE;

    if (instance == NULL || instance->impl == NULL ||
        out_result_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    instance_impl = (turbowasm_instance_impl *)instance->impl;
    module_impl = turbowasm_module_impl_get(instance_impl->module);
    if (module_impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_validation_context_function_type(
        &module_impl->validation, function_index);
    if (type == NULL || !type->defined)
        return TURBOWASM_INVALID_ARGUMENT;

    if (argument_count != type->param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count > result_capacity)
        return TURBOWASM_INVALID_ARGUMENT;
    if (type->result_count != 0u && results == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < argument_count; ++index) {
        if (!turbowasm_value_matches_type(
                &arguments[index], type->params[index]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    context.module = module_impl;
    context.max_call_depth = instance_impl->max_call_depth;
    context.trap = TURBOWASM_TRAP_NONE;

    status = turbowasm_exec_push_frame(
        &context, function_index,
        arguments, argument_count, 0u);

    while (status == TURBOWASM_OK &&
           context.frames.size != 0u) {
        status = turbowasm_exec_step(&context);
    }

    if (status == TURBOWASM_OK) {
        if (context.values.size != type->result_count) {
            status = TURBOWASM_MALFORMED_MODULE;
        } else {
            for (index = 0u; index < type->result_count; ++index)
                results[index] = context.values.values[index];
            *out_result_count = type->result_count;
        }
    }

    if (out_trap != NULL)
        *out_trap = context.trap;

    turbowasm_exec_context_destroy(&context);
    return status;
}

const char *turbowasm_trap_string(turbowasm_trap trap) {
    switch (trap) {
        case TURBOWASM_TRAP_NONE: return "none";
        case TURBOWASM_TRAP_UNREACHABLE: return "unreachable";
        case TURBOWASM_TRAP_CALL_STACK_EXHAUSTED:
            return "call_stack_exhausted";
        default: return "unknown";
    }
}
