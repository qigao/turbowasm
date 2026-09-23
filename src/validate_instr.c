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
    bool unreachable;
} turbowasm_type_stack;

static bool turbowasm_instr_valtype(uint8_t type) {
    return type == TW_I32 || type == TW_I64 ||
           type == TW_F32 || type == TW_F64 ||
           type == TW_V128 || type == TW_FUNCREF ||
           type == TW_EXTERNREF;
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
    if (stack->size == 0u) {
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

    while (turbowasm_reader_remaining(body) != 0u) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(body, &opcode)) {
            result = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }

        switch (opcode) {
            case 0x00u: /* unreachable */
                stack.size = 0u;
                stack.unreachable = true;
                break;
            case 0x01u: /* nop */
                break;
            case 0x0bu: /* end */
                if (turbowasm_reader_remaining(body) != 0u) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto done;
                }
                result = turbowasm_validate_result_stack(
                    &stack, function_type);
                goto done;
            case 0x0fu: /* return */
                result = turbowasm_pop_results(
                    &stack, function_type);
                if (result != TURBOWASM_OK)
                    goto done;
                stack.size = 0u;
                stack.unreachable = true;
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
            case 0x02u: /* block */
            case 0x03u: /* loop */
            case 0x04u: /* if */
            case 0x05u: /* else */
            case 0x0cu: /* br */
            case 0x0du: /* br_if */
                result = TURBOWASM_UNSUPPORTED;
                goto done;
            default:
                result = TURBOWASM_UNSUPPORTED;
                goto done;
        }
    }

    result = TURBOWASM_MALFORMED_MODULE;

done:
    free(locals);
    free(stack.values);
    return result;
}
