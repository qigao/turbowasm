#include <turbowasm/instance.h>

#include "module_internal.h"
#include "reader.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_EXEC_MAX_CALL_DEPTH = 256u
};

typedef struct turbowasm_instance_impl {
    const turbowasm_module *module;
} turbowasm_instance_impl;

typedef struct turbowasm_value_stack {
    turbowasm_value *values;
    uint32_t size;
    uint32_t capacity;
} turbowasm_value_stack;

static turbowasm_value_kind turbowasm_kind_from_valtype(uint8_t type) {
    switch (type) {
        case 0x7fu: return TURBOWASM_VALUE_I32;
        case 0x7eu: return TURBOWASM_VALUE_I64;
        case 0x7du: return TURBOWASM_VALUE_F32;
        case 0x7cu: return TURBOWASM_VALUE_F64;
        case 0x7bu: return TURBOWASM_VALUE_V128;
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
    if (kind == TURBOWASM_VALUE_V128)
        out->as.v128.shape = TURBOWASM_V128_RAW;
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
    const turbowasm_module_impl *module,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    uint32_t depth);

static turbowasm_status turbowasm_exec_direct_call(
    const turbowasm_module_impl *module,
    turbowasm_reader *reader,
    turbowasm_value_stack *stack,
    turbowasm_trap *trap,
    uint32_t depth) {
    uint32_t function_index;
    const turbowasm_validation_func_type *type;
    turbowasm_value *arguments = NULL;
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    uint32_t index;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(reader, &function_index))
        return TURBOWASM_MALFORMED_MODULE;

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
        module,
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

static turbowasm_status turbowasm_exec_function(
    const turbowasm_module_impl *module,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    uint32_t depth) {
    const turbowasm_validation_context *context;
    const turbowasm_validation_function *function;
    const turbowasm_validation_func_type *type;
    turbowasm_value *locals = NULL;
    turbowasm_value_stack stack = {0};
    turbowasm_reader reader;
    uint32_t index;
    turbowasm_status status = TURBOWASM_OK;
    bool finished = false;

    if (module == NULL || result_count == NULL || trap == NULL)
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
            case 0x0bu: /* end */
            case 0x0fu: /* return */
                finished = true;
                break;
            case 0x10u: /* call */
                status = turbowasm_exec_direct_call(
                    module, &reader, &stack, trap, depth);
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

            default:
                status = TURBOWASM_UNSUPPORTED;
                goto done;
        }
    }

    if (!finished) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    if (stack.size != type->result_count) {
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

turbowasm_status turbowasm_instance_invoke(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    const turbowasm_instance_impl *impl;
    const turbowasm_module_impl *module;

    if (instance == NULL || instance->impl == NULL ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    impl = (const turbowasm_instance_impl *)instance->impl;
    module = turbowasm_module_impl_get(impl->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_exec_function(
        module,
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
        default: return "unknown";
    }
}
