#include "mir_backend.h"

#include "../instance_internal.h"

#include "../reader.h"
#include "../validation_context.h"

#include <mir-gen.h>
#include <mir.h>

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_mir_backend_context {
    MIR_context_t mir;
    uint32_t next_module_id;
} turbowasm_mir_backend_context;

typedef struct turbowasm_mir_compiled {
    void *generated;
    uint8_t result_type;
    uint8_t param_count;
    uint8_t param_types[2];
} turbowasm_mir_compiled;

typedef struct turbowasm_mir_stack_value {
    uint32_t reg;
    uint8_t type;
} turbowasm_mir_stack_value;

typedef struct turbowasm_mir_text {
    char *data;
    size_t size;
    size_t capacity;
} turbowasm_mir_text;

static bool turbowasm_mir_text_reserve(
    turbowasm_mir_text *text,
    size_t extra) {
    size_t required;
    size_t capacity;
    char *next;

    if (text == NULL || extra > SIZE_MAX - text->size - 1u)
        return false;

    required = text->size + extra + 1u;
    if (required <= text->capacity)
        return true;

    capacity = text->capacity == 0u ? 256u : text->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u)
            return false;
        capacity *= 2u;
    }

    next = (char *)realloc(text->data, capacity);
    if (next == NULL)
        return false;

    text->data = next;
    text->capacity = capacity;
    return true;
}

static bool turbowasm_mir_text_appendf(
    turbowasm_mir_text *text,
    const char *format,
    ...) {
    va_list args;
    va_list copy;
    int length;

    if (text == NULL || format == NULL)
        return false;

    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0 ||
        !turbowasm_mir_text_reserve(text, (size_t)length)) {
        va_end(args);
        return false;
    }

    vsnprintf(
        text->data + text->size,
        text->capacity - text->size,
        format,
        args);
    va_end(args);
    text->size += (size_t)length;
    return true;
}

static bool turbowasm_mir_integer_type(uint8_t type) {
    return type == 0x7fu || type == 0x7eu;
}

static bool turbowasm_mir_float_type(uint8_t type) {
    return type == 0x7du || type == 0x7cu;
}

static bool turbowasm_mir_scalar_type(uint8_t type) {
    return turbowasm_mir_integer_type(type) ||
           turbowasm_mir_float_type(type);
}

static const char *turbowasm_mir_reg_prefix(uint8_t type) {
    switch (type) {
        case 0x7fu:
        case 0x7eu:
            return "r";
        case 0x7du:
            return "f";
        case 0x7cu:
            return "d";
        default:
            return NULL;
    }
}

static const char *turbowasm_mir_zero_literal(uint8_t type) {
    switch (type) {
        case 0x7fu:
        case 0x7eu:
            return "0";
        case 0x7du:
            return "0.0f";
        case 0x7cu:
            return "0.0";
        default:
            return NULL;
    }
}

static const char *turbowasm_mir_type_name(uint8_t type) {
    switch (type) {
        case 0x7fu:
        case 0x7eu:
            return "i64";
        case 0x7du:
            return "f";
        case 0x7cu:
            return "d";
        default:
            return NULL;
    }
}

static const char *turbowasm_mir_move_name(uint8_t type) {
    switch (type) {
        case 0x7fu:
        case 0x7eu:
            return "mov";
        case 0x7du:
            return "fmov";
        case 0x7cu:
            return "dmov";
        default:
            return NULL;
    }
}

static uint8_t turbowasm_mir_binary_type(uint8_t opcode) {
    if (opcode >= 0x6au && opcode <= 0x6cu)
        return 0x7fu;
    if (opcode >= 0x7cu && opcode <= 0x7eu)
        return 0x7eu;
    if (opcode >= 0x92u && opcode <= 0x95u)
        return 0x7du;
    if (opcode >= 0xa0u && opcode <= 0xa3u)
        return 0x7cu;
    return 0u;
}


static bool turbowasm_mir_call_signature_supported(
    const turbowasm_validation_func_type *type) {
    uint32_t index;

    if (type == NULL || !type->defined ||
        type->result_count != 1u)
        return false;

    if (turbowasm_mir_integer_type(type->results[0])) {
        if (type->param_count > 2u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (!turbowasm_mir_integer_type(type->params[index]))
                return false;
        }
        return true;
    }

    if (turbowasm_mir_float_type(type->results[0])) {
        if (type->param_count > 1u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (type->params[index] != type->results[0])
                return false;
        }
        return true;
    }

    return false;
}

static void turbowasm_mir_record_call(
    turbowasm_jit_invocation_context *context,
    turbowasm_status status,
    turbowasm_trap trap) {
    if (context == NULL)
        return;
    context->call_status = status;
    context->call_trap = trap;
}

static const turbowasm_validation_func_type *
turbowasm_mir_context_function_type(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index) {
    const turbowasm_module_impl *module;

    if (context == NULL || context->instance == NULL)
        return NULL;

    module = turbowasm_module_impl_get(context->instance->module);
    if (module == NULL)
        return NULL;

    return turbowasm_validation_context_function_type(
        &module->validation, function_index);
}

static int64_t turbowasm_mir_call_integer(
    turbowasm_jit_invocation_context *context,
    uint32_t function_index,
    const int64_t *raw_arguments,
    uint32_t raw_count) {
    const turbowasm_validation_func_type *type;
    turbowasm_value arguments[2] = {{0}};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;
    uint32_t index;
    int64_t raw_result = 0;

    type = turbowasm_mir_context_function_type(
        context, function_index);
    if (!turbowasm_mir_call_signature_supported(type) ||
        !turbowasm_mir_integer_type(type->results[0]) ||
        type->param_count != raw_count) {
        turbowasm_mir_record_call(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0;
    }

    for (index = 0u; index < type->param_count; ++index) {
        if (type->params[index] == 0x7fu) {
            arguments[index].kind = TURBOWASM_VALUE_I32;
            arguments[index].as.i32 =
                (int32_t)(uint32_t)raw_arguments[index];
        } else {
            arguments[index].kind = TURBOWASM_VALUE_I64;
            arguments[index].as.i64 = raw_arguments[index];
        }
    }

    status = turbowasm_jit_direct_call(
        context,
        function_index,
        arguments,
        type->param_count,
        &result,
        1u,
        &result_count,
        &trap);
    turbowasm_mir_record_call(context, status, trap);

    if (status != TURBOWASM_OK || result_count != 1u)
        return 0;

    if (type->results[0] == 0x7fu &&
        result.kind == TURBOWASM_VALUE_I32) {
        raw_result = (int64_t)result.as.i32;
    } else if (type->results[0] == 0x7eu &&
               result.kind == TURBOWASM_VALUE_I64) {
        raw_result = result.as.i64;
    } else {
        turbowasm_mir_record_call(
            context, TURBOWASM_TYPE_MISMATCH,
            TURBOWASM_TRAP_NONE);
        return 0;
    }

    return raw_result;
}

static int64_t turbowasm_mir_call_i64_0(
    turbowasm_jit_invocation_context *context,
    int64_t function_index) {
    return turbowasm_mir_call_integer(
        context, (uint32_t)function_index, NULL, 0u);
}

static int64_t turbowasm_mir_call_i64_1(
    turbowasm_jit_invocation_context *context,
    int64_t function_index,
    int64_t a0) {
    const int64_t args[1] = {a0};
    return turbowasm_mir_call_integer(
        context, (uint32_t)function_index, args, 1u);
}

static int64_t turbowasm_mir_call_i64_2(
    turbowasm_jit_invocation_context *context,
    int64_t function_index,
    int64_t a0,
    int64_t a1) {
    const int64_t args[2] = {a0, a1};
    return turbowasm_mir_call_integer(
        context, (uint32_t)function_index, args, 2u);
}

static float turbowasm_mir_call_f32_0(
    turbowasm_jit_invocation_context *context,
    int64_t function_index) {
    const turbowasm_validation_func_type *type;
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    type = turbowasm_mir_context_function_type(
        context, (uint32_t)function_index);
    if (!turbowasm_mir_call_signature_supported(type) ||
        type->result_count != 1u ||
        type->results[0] != 0x7du ||
        type->param_count != 0u) {
        turbowasm_mir_record_call(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0.0f;
    }

    status = turbowasm_jit_direct_call(
        context, (uint32_t)function_index,
        NULL, 0u,
        &result, 1u,
        &result_count, &trap);
    turbowasm_mir_record_call(context, status, trap);

    if (status != TURBOWASM_OK || result_count != 1u ||
        result.kind != TURBOWASM_VALUE_F32) {
        if (status == TURBOWASM_OK)
            turbowasm_mir_record_call(
                context, TURBOWASM_TYPE_MISMATCH,
                TURBOWASM_TRAP_NONE);
        return 0.0f;
    }
    return result.as.f32;
}

static float turbowasm_mir_call_f32_1(
    turbowasm_jit_invocation_context *context,
    int64_t function_index,
    float a0) {
    const turbowasm_validation_func_type *type;
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    type = turbowasm_mir_context_function_type(
        context, (uint32_t)function_index);
    if (!turbowasm_mir_call_signature_supported(type) ||
        type->result_count != 1u ||
        type->results[0] != 0x7du ||
        type->param_count != 1u ||
        type->params[0] != 0x7du) {
        turbowasm_mir_record_call(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0.0f;
    }

    argument.kind = TURBOWASM_VALUE_F32;
    argument.as.f32 = a0;

    status = turbowasm_jit_direct_call(
        context, (uint32_t)function_index,
        &argument, 1u,
        &result, 1u,
        &result_count, &trap);
    turbowasm_mir_record_call(context, status, trap);

    if (status != TURBOWASM_OK || result_count != 1u ||
        result.kind != TURBOWASM_VALUE_F32) {
        if (status == TURBOWASM_OK)
            turbowasm_mir_record_call(
                context, TURBOWASM_TYPE_MISMATCH,
                TURBOWASM_TRAP_NONE);
        return 0.0f;
    }
    return result.as.f32;
}

static double turbowasm_mir_call_f64_0(
    turbowasm_jit_invocation_context *context,
    int64_t function_index) {
    const turbowasm_validation_func_type *type;
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    type = turbowasm_mir_context_function_type(
        context, (uint32_t)function_index);
    if (!turbowasm_mir_call_signature_supported(type) ||
        type->result_count != 1u ||
        type->results[0] != 0x7cu ||
        type->param_count != 0u) {
        turbowasm_mir_record_call(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0.0;
    }

    status = turbowasm_jit_direct_call(
        context, (uint32_t)function_index,
        NULL, 0u,
        &result, 1u,
        &result_count, &trap);
    turbowasm_mir_record_call(context, status, trap);

    if (status != TURBOWASM_OK || result_count != 1u ||
        result.kind != TURBOWASM_VALUE_F64) {
        if (status == TURBOWASM_OK)
            turbowasm_mir_record_call(
                context, TURBOWASM_TYPE_MISMATCH,
                TURBOWASM_TRAP_NONE);
        return 0.0;
    }
    return result.as.f64;
}

static double turbowasm_mir_call_f64_1(
    turbowasm_jit_invocation_context *context,
    int64_t function_index,
    double a0) {
    const turbowasm_validation_func_type *type;
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    type = turbowasm_mir_context_function_type(
        context, (uint32_t)function_index);
    if (!turbowasm_mir_call_signature_supported(type) ||
        type->result_count != 1u ||
        type->results[0] != 0x7cu ||
        type->param_count != 1u ||
        type->params[0] != 0x7cu) {
        turbowasm_mir_record_call(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0.0;
    }

    argument.kind = TURBOWASM_VALUE_F64;
    argument.as.f64 = a0;

    status = turbowasm_jit_direct_call(
        context, (uint32_t)function_index,
        &argument, 1u,
        &result, 1u,
        &result_count, &trap);
    turbowasm_mir_record_call(context, status, trap);

    if (status != TURBOWASM_OK || result_count != 1u ||
        result.kind != TURBOWASM_VALUE_F64) {
        if (status == TURBOWASM_OK)
            turbowasm_mir_record_call(
                context, TURBOWASM_TYPE_MISMATCH,
                TURBOWASM_TRAP_NONE);
        return 0.0;
    }
    return result.as.f64;
}

static int64_t turbowasm_mir_call_status(
    turbowasm_jit_invocation_context *context) {
    return context == NULL
        ? (int64_t)TURBOWASM_INVALID_ARGUMENT
        : (int64_t)context->call_status;
}

static int64_t turbowasm_mir_checkpoint(
    turbowasm_jit_invocation_context *context) {
    return (int64_t)turbowasm_jit_execution_checkpoint(context);
}

static bool turbowasm_mir_scan_scalar_locals(
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    uint32_t *out_register_count,
    uint8_t *out_result_type) {
    const turbowasm_validation_func_type *type;
    turbowasm_reader reader;
    uint8_t *types = NULL;
    uint32_t stack_size = 0u;
    uint32_t register_count = 0u;
    uint32_t index;
    uint8_t result_type;
    bool float_function;
    bool ok = false;

    if (validation == NULL || function == NULL ||
        function->imported || function->code == NULL)
        return false;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL || !type->defined ||
        function->local_count < type->param_count ||
        type->result_count != 1u)
        return false;

    result_type = type->results[0];
    if (!turbowasm_mir_integer_type(result_type) &&
        !turbowasm_mir_float_type(result_type))
        return false;

    float_function = turbowasm_mir_float_type(result_type);

    if (float_function) {
        if (type->param_count > 1u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (type->params[index] != result_type)
                return false;
        }
        for (index = 0u; index < function->local_count; ++index) {
            if (function->local_types[index] != result_type)
                return false;
        }
    } else {
        if (type->param_count > 2u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (!turbowasm_mir_integer_type(type->params[index]))
                return false;
        }
        for (index = 0u; index < function->local_count; ++index) {
            if (!turbowasm_mir_integer_type(
                    function->local_types[index]))
                return false;
        }
    }

    if (function->code_size == 0u)
        return false;

    types = (uint8_t *)calloc(
        (size_t)function->code_size + 1u,
        sizeof(*types));
    if (types == NULL)
        return false;

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&reader, &opcode))
            goto done;

        switch (opcode) {
            case 0x10u: { /* call */
                uint32_t callee_index;
                const turbowasm_validation_function *callee;
                const turbowasm_validation_func_type *callee_type;
                uint32_t arg_index;
                uint32_t base;

                if (!turbowasm_reader_uleb32(
                        &reader, &callee_index))
                    goto done;

                callee = turbowasm_validation_context_function(
                    validation, callee_index);
                callee_type =
                    turbowasm_validation_context_function_type(
                        validation, callee_index);
                if (callee == NULL || callee->imported ||
                    !turbowasm_mir_call_signature_supported(
                        callee_type))
                    goto done;

                if (float_function) {
                    if (callee_type->results[0] != result_type)
                        goto done;
                    for (arg_index = 0u;
                         arg_index < callee_type->param_count;
                         ++arg_index) {
                        if (callee_type->params[arg_index] !=
                            result_type)
                            goto done;
                    }
                } else {
                    if (!turbowasm_mir_integer_type(
                            callee_type->results[0]))
                        goto done;
                }

                if (stack_size < callee_type->param_count)
                    goto done;
                base = stack_size - callee_type->param_count;
                for (arg_index = 0u;
                     arg_index < callee_type->param_count;
                     ++arg_index) {
                    if (types[base + arg_index] !=
                        callee_type->params[arg_index])
                        goto done;
                }

                stack_size = base;
                types[stack_size++] =
                    callee_type->results[0];
                ++register_count;
                break;
            }
            case 0x20u: { /* local.get */
                uint32_t local_index;
                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count)
                    goto done;
                types[stack_size++] =
                    function->local_types[local_index];
                ++register_count;
                break;
            }
            case 0x21u: /* local.set */
            case 0x22u: { /* local.tee */
                uint32_t local_index;
                uint8_t local_type;
                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count ||
                    stack_size == 0u)
                    goto done;
                local_type = function->local_types[local_index];
                if (types[stack_size - 1u] != local_type)
                    goto done;
                if (opcode == 0x21u)
                    --stack_size;
                break;
            }
            case 0x41u: { /* i32.const */
                int32_t value;
                if (float_function ||
                    !turbowasm_reader_sleb32(&reader, &value))
                    goto done;
                (void)value;
                types[stack_size++] = 0x7fu;
                ++register_count;
                break;
            }
            case 0x42u: { /* i64.const */
                int64_t value;
                if (float_function ||
                    !turbowasm_reader_sleb64(&reader, &value))
                    goto done;
                (void)value;
                types[stack_size++] = 0x7eu;
                ++register_count;
                break;
            }
            case 0x43u: { /* f32.const */
                uint32_t bits;
                float value;
                if (!float_function || result_type != 0x7du ||
                    !turbowasm_reader_u32le(&reader, &bits))
                    goto done;
                memcpy(&value, &bits, sizeof(value));
                if (!isfinite(value))
                    goto done;
                types[stack_size++] = 0x7du;
                ++register_count;
                break;
            }
            case 0x44u: { /* f64.const */
                turbowasm_reader bytes;
                uint64_t bits = 0u;
                double value;
                if (!float_function || result_type != 0x7cu ||
                    !turbowasm_reader_slice(&reader, 8u, &bytes))
                    goto done;
                for (index = 0u; index < 8u; ++index)
                    bits |= (uint64_t)bytes.cursor[index] << (8u * index);
                memcpy(&value, &bits, sizeof(value));
                if (!isfinite(value))
                    goto done;
                types[stack_size++] = 0x7cu;
                ++register_count;
                break;
            }
            case 0x6au: case 0x6bu: case 0x6cu:
            case 0x7cu: case 0x7du: case 0x7eu:
            case 0x92u: case 0x93u: case 0x94u: case 0x95u:
            case 0xa0u: case 0xa1u: case 0xa2u: case 0xa3u: {
                uint8_t expected =
                    turbowasm_mir_binary_type(opcode);
                if (expected == 0u ||
                    (float_function
                        ? expected != result_type
                        : !turbowasm_mir_integer_type(expected)) ||
                    stack_size < 2u ||
                    types[stack_size - 1u] != expected ||
                    types[stack_size - 2u] != expected)
                    goto done;
                --stack_size;
                types[stack_size - 1u] = expected;
                ++register_count;
                break;
            }
            case 0x0bu: /* end */
                if (turbowasm_reader_remaining(&reader) != 0u ||
                    stack_size != 1u ||
                    types[0] != result_type)
                    goto done;
                if (out_register_count != NULL)
                    *out_register_count = register_count;
                if (out_result_type != NULL)
                    *out_result_type = result_type;
                ok = true;
                goto done;
            default:
                goto done;
        }
    }

done:
    free(types);
    return ok;
}


typedef struct turbowasm_mir_scan_control {
    const turbowasm_validation_control *annotation;
} turbowasm_mir_scan_control;

static bool turbowasm_mir_scalar_control_signature(
    const turbowasm_validation_context *validation,
    const turbowasm_validation_control *annotation,
    uint8_t function_result_type,
    const uint8_t **out_start_types,
    uint32_t *out_start_count,
    const uint8_t **out_end_types,
    uint32_t *out_end_count) {
    const uint8_t *start_types = NULL;
    const uint8_t *end_types = NULL;
    uint32_t start_count = 0u;
    uint32_t end_count = 0u;
    uint32_t index;
    bool float_function;

    if (!turbowasm_mir_scalar_type(function_result_type) ||
        !turbowasm_validation_control_signature(
            validation, annotation,
            &start_types, &start_count,
            &end_types, &end_count))
        return false;

    float_function = turbowasm_mir_float_type(function_result_type);

    for (index = 0u; index < start_count; ++index) {
        if (float_function) {
            if (start_types[index] != function_result_type)
                return false;
        } else if (!turbowasm_mir_integer_type(start_types[index])) {
            return false;
        }
    }
    for (index = 0u; index < end_count; ++index) {
        if (float_function) {
            if (end_types[index] != function_result_type)
                return false;
        } else if (!turbowasm_mir_integer_type(end_types[index])) {
            return false;
        }
    }

    if (out_start_types != NULL)
        *out_start_types = start_types;
    if (out_start_count != NULL)
        *out_start_count = start_count;
    if (out_end_types != NULL)
        *out_end_types = end_types;
    if (out_end_count != NULL)
        *out_end_count = end_count;
    return true;
}

static bool turbowasm_mir_scalar_function_shape(
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    uint8_t *out_result_type) {
    const turbowasm_validation_func_type *type;
    uint32_t index;
    uint8_t result_type;

    if (validation == NULL || function == NULL ||
        function->imported || function->code == NULL)
        return false;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL || !type->defined ||
        type->result_count != 1u ||
        function->local_count < type->param_count)
        return false;

    result_type = type->results[0];
    if (!turbowasm_mir_scalar_type(result_type))
        return false;

    if (turbowasm_mir_float_type(result_type)) {
        if (type->param_count > 1u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (type->params[index] != result_type)
                return false;
        }
        for (index = 0u; index < function->local_count; ++index) {
            if (function->local_types[index] != result_type)
                return false;
        }
    } else {
        if (type->param_count > 2u)
            return false;
        for (index = 0u; index < type->param_count; ++index) {
            if (!turbowasm_mir_integer_type(type->params[index]))
                return false;
        }
        for (index = 0u; index < function->local_count; ++index) {
            if (!turbowasm_mir_integer_type(
                    function->local_types[index]))
                return false;
        }
    }

    if (out_result_type != NULL)
        *out_result_type = result_type;
    return true;
}

static bool turbowasm_mir_scan_structured_scalar(
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    uint8_t *out_result_type) {
    const turbowasm_validation_func_type *type;
    turbowasm_mir_scan_control *controls = NULL;
    turbowasm_reader reader;
    uint32_t control_size = 0u;
    uint8_t result_type = 0u;
    bool saw_structured = false;
    bool ok = false;

    if (!turbowasm_mir_scalar_function_shape(
            validation, function_index, function, &result_type) ||
        function->code_size == 0u)
        return false;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL)
        return false;

    if (function->control_count != 0u) {
        controls = (turbowasm_mir_scan_control *)calloc(
            (size_t)function->control_count, sizeof(*controls));
        if (controls == NULL)
            return false;
    }

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;
        uint32_t opcode_offset;

        if (!turbowasm_reader_u8(&reader, &opcode) ||
            reader.cursor <= function->code ||
            (size_t)(reader.cursor - function->code - 1u) > UINT32_MAX)
            goto done;

        opcode_offset =
            (uint32_t)(reader.cursor - function->code - 1u);

        switch (opcode) {
            case 0x01u: /* nop */
                break;

            case 0x02u: /* block */
            case 0x03u: /* loop */
            case 0x04u: { /* if */
                const turbowasm_validation_control *annotation;
                uint32_t current_offset;

                annotation =
                    turbowasm_validation_function_control_at(
                        function, opcode_offset);
                current_offset =
                    (uint32_t)(reader.cursor - function->code);

                if (annotation == NULL ||
                    annotation->end_offset == UINT32_MAX ||
                    control_size >= function->control_count ||
                    annotation->body_offset < current_offset ||
                    annotation->body_offset > function->code_size ||
                    !turbowasm_mir_scalar_control_signature(
                        validation, annotation, result_type,
                        NULL, NULL, NULL, NULL))
                    goto done;

                if ((opcode == 0x02u &&
                     annotation->kind !=
                         TURBOWASM_VALIDATION_CONTROL_BLOCK) ||
                    (opcode == 0x03u &&
                     annotation->kind !=
                         TURBOWASM_VALIDATION_CONTROL_LOOP) ||
                    (opcode == 0x04u &&
                     annotation->kind !=
                         TURBOWASM_VALIDATION_CONTROL_IF))
                    goto done;

                /*
                 * The validator already parsed and proved the blocktype.
                 * Advance directly to the retained body offset instead of
                 * reconstructing s33/type-index validation here.
                 */
                reader.cursor =
                    function->code + annotation->body_offset;

                controls[control_size++].annotation = annotation;
                saw_structured = true;
                break;
            }

            case 0x05u: { /* else */
                const turbowasm_validation_control *annotation;
                if (control_size == 0u)
                    goto done;
                annotation = controls[control_size - 1u].annotation;
                if (annotation == NULL ||
                    annotation->kind !=
                        TURBOWASM_VALIDATION_CONTROL_IF ||
                    annotation->else_offset != opcode_offset)
                    goto done;
                break;
            }

            case 0x0bu: /* end */
                if (control_size == 0u) {
                    if (turbowasm_reader_remaining(&reader) != 0u ||
                        !saw_structured)
                        goto done;
                    ok = true;
                    goto done;
                } else {
                    const turbowasm_validation_control *annotation =
                        controls[control_size - 1u].annotation;
                    if (annotation == NULL ||
                        annotation->end_offset != opcode_offset)
                        goto done;
                    --control_size;
                }
                break;

            case 0x0cu: /* br */
            case 0x0du: { /* br_if */
                uint32_t depth;
                if (!turbowasm_reader_uleb32(&reader, &depth) ||
                    depth > control_size)
                    goto done;
                break;
            }

            case 0x0eu: { /* br_table */
                uint32_t count;
                uint32_t index;
                uint32_t depth;

                if (!turbowasm_reader_uleb32(&reader, &count))
                    goto done;

                for (index = 0u; index < count; ++index) {
                    if (!turbowasm_reader_uleb32(&reader, &depth) ||
                        depth > control_size)
                        goto done;
                }

                if (!turbowasm_reader_uleb32(&reader, &depth) ||
                    depth > control_size)
                    goto done;
                break;
            }

            case 0x0fu: /* return */
                break;

            case 0x10u: { /* call */
                uint32_t callee_index;
                const turbowasm_validation_function *callee;
                const turbowasm_validation_func_type *callee_type;
                if (!turbowasm_reader_uleb32(
                        &reader, &callee_index))
                    goto done;
                callee = turbowasm_validation_context_function(
                    validation, callee_index);
                callee_type =
                    turbowasm_validation_context_function_type(
                        validation, callee_index);
                if (callee == NULL || callee->imported ||
                    !turbowasm_mir_call_signature_supported(callee_type))
                    goto done;
                if (turbowasm_mir_float_type(result_type)) {
                    uint32_t arg_index;
                    if (callee_type->results[0] != result_type)
                        goto done;
                    for (arg_index = 0u;
                         arg_index < callee_type->param_count;
                         ++arg_index) {
                        if (callee_type->params[arg_index] != result_type)
                            goto done;
                    }
                } else if (!turbowasm_mir_integer_type(
                               callee_type->results[0])) {
                    goto done;
                }
                break;
            }

            case 0x1au: /* drop */
                break;

            case 0x20u: /* local.get */
            case 0x21u: /* local.set */
            case 0x22u: { /* local.tee */
                uint32_t local_index;
                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count)
                    goto done;
                break;
            }

            case 0x41u: {
                int32_t value;
                if (!turbowasm_reader_sleb32(&reader, &value))
                    goto done;
                (void)value;
                break;
            }

            case 0x42u: {
                int64_t value;
                if (turbowasm_mir_float_type(result_type) ||
                    !turbowasm_reader_sleb64(&reader, &value))
                    goto done;
                (void)value;
                break;
            }

            case 0x43u: {
                uint32_t bits;
                float value;
                if (result_type != 0x7du ||
                    !turbowasm_reader_u32le(&reader, &bits))
                    goto done;
                memcpy(&value, &bits, sizeof(value));
                if (!isfinite(value))
                    goto done;
                break;
            }

            case 0x44u: {
                turbowasm_reader bytes;
                uint64_t bits = 0u;
                double value;
                uint32_t index;
                if (result_type != 0x7cu ||
                    !turbowasm_reader_slice(&reader, 8u, &bytes))
                    goto done;
                for (index = 0u; index < 8u; ++index)
                    bits |= (uint64_t)bytes.cursor[index] << (8u * index);
                memcpy(&value, &bits, sizeof(value));
                if (!isfinite(value))
                    goto done;
                break;
            }

            case 0x6au: case 0x6bu: case 0x6cu:
            case 0x7cu: case 0x7du: case 0x7eu:
                if (turbowasm_mir_float_type(result_type))
                    goto done;
                break;

            case 0x92u: case 0x93u: case 0x94u: case 0x95u:
                if (result_type != 0x7du)
                    goto done;
                break;

            case 0xa0u: case 0xa1u: case 0xa2u: case 0xa3u:
                if (result_type != 0x7cu)
                    goto done;
                break;

            default:
                goto done;
        }
    }

done:
    free(controls);
    if (ok && out_result_type != NULL)
        *out_result_type = result_type;
    return ok;
}

static bool turbowasm_mir_is_function_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    (void)context;
    return turbowasm_mir_scan_scalar_locals(
               validation, function_index, function, NULL, NULL) ||
           turbowasm_mir_scan_structured_scalar(
               validation, function_index, function, NULL);
}

static const char *turbowasm_mir_binary_name(uint8_t opcode) {
    switch (opcode) {
        case 0x6au: return "adds";
        case 0x6bu: return "subs";
        case 0x6cu: return "muls";
        case 0x7cu: return "add";
        case 0x7du: return "sub";
        case 0x7eu: return "mul";
        case 0x92u: return "fadd";
        case 0x93u: return "fsub";
        case 0x94u: return "fmul";
        case 0x95u: return "fdiv";
        case 0xa0u: return "dadd";
        case 0xa1u: return "dsub";
        case 0xa2u: return "dmul";
        case 0xa3u: return "ddiv";
        default: return NULL;
    }
}


typedef enum turbowasm_mir_control_kind {
    TURBOWASM_MIR_CONTROL_FUNCTION = 0,
    TURBOWASM_MIR_CONTROL_BLOCK,
    TURBOWASM_MIR_CONTROL_LOOP,
    TURBOWASM_MIR_CONTROL_IF
} turbowasm_mir_control_kind;

typedef struct turbowasm_mir_control_frame {
    turbowasm_mir_control_kind kind;
    const turbowasm_validation_control *annotation;
    uint32_t height;
    bool else_seen;
    bool end_incoming;
    bool end_opcode_incoming;

    const uint8_t *start_types;
    uint32_t start_count;
    const uint8_t *end_types;
    uint32_t end_count;

    uint32_t start_reg_base;
    uint32_t end_reg_base;
} turbowasm_mir_control_frame;

static bool turbowasm_mir_emit_checkpoint_text(
    turbowasm_mir_text *text) {
    return turbowasm_mir_text_appendf(
        text,
        "call tw_checkpoint_p, tw_jit_checkpoint, "
        "jit_status, jit_ctx\n"
        "bne jit_fail, jit_status, 0\n");
}

static bool turbowasm_mir_emit_control_label(
    turbowasm_mir_text *text,
    uint32_t opcode_offset,
    const char *suffix) {
    return text != NULL && suffix != NULL &&
           turbowasm_mir_text_appendf(
               text, "c_%u_%s:\n", opcode_offset, suffix);
}

static bool turbowasm_mir_emit_control_jump(
    turbowasm_mir_text *text,
    uint32_t opcode_offset,
    const char *suffix) {
    return text != NULL && suffix != NULL &&
           turbowasm_mir_text_appendf(
               text, "jmp c_%u_%s\n", opcode_offset, suffix);
}

static bool turbowasm_mir_emit_control_branch_true(
    turbowasm_mir_text *text,
    uint32_t opcode_offset,
    const char *suffix,
    uint32_t condition_reg) {
    return text != NULL && suffix != NULL &&
           turbowasm_mir_text_appendf(
               text, "bt c_%u_%s, r%u\n",
               opcode_offset, suffix, condition_reg);
}

static bool turbowasm_mir_emit_control_branch_false(
    turbowasm_mir_text *text,
    uint32_t opcode_offset,
    const char *suffix,
    uint32_t condition_reg) {
    return text != NULL && suffix != NULL &&
           turbowasm_mir_text_appendf(
               text, "bf c_%u_%s, r%u\n",
               opcode_offset, suffix, condition_reg);
}

static turbowasm_mir_control_kind turbowasm_mir_control_kind_from_annotation(
    const turbowasm_validation_control *annotation) {
    if (annotation == NULL)
        return TURBOWASM_MIR_CONTROL_FUNCTION;
    switch (annotation->kind) {
        case TURBOWASM_VALIDATION_CONTROL_BLOCK:
            return TURBOWASM_MIR_CONTROL_BLOCK;
        case TURBOWASM_VALIDATION_CONTROL_LOOP:
            return TURBOWASM_MIR_CONTROL_LOOP;
        case TURBOWASM_VALIDATION_CONTROL_IF:
            return TURBOWASM_MIR_CONTROL_IF;
        default:
            return TURBOWASM_MIR_CONTROL_FUNCTION;
    }
}

static bool turbowasm_mir_stack_matches_types(
    const turbowasm_mir_stack_value *stack,
    uint32_t stack_size,
    const uint8_t *types,
    uint32_t count) {
    uint32_t base;
    uint32_t index;

    if (count > stack_size)
        return false;
    base = stack_size - count;

    for (index = 0u; index < count; ++index) {
        if (stack[base + index].type != types[index])
            return false;
    }
    return true;
}

static bool turbowasm_mir_emit_stack_to_regs(
    turbowasm_mir_text *text,
    const turbowasm_mir_stack_value *stack,
    uint32_t stack_size,
    const uint8_t *types,
    uint32_t count,
    uint32_t reg_base) {
    uint32_t base;
    uint32_t index;

    if (!turbowasm_mir_stack_matches_types(
            stack, stack_size, types, count))
        return false;

    base = stack_size - count;
    for (index = 0u; index < count; ++index) {
        if (!turbowasm_mir_text_appendf(
                text, "mov r%u, r%u\n",
                reg_base + index,
                stack[base + index].reg))
            return false;
    }
    return true;
}

static bool turbowasm_mir_push_regs(
    turbowasm_mir_stack_value *stack,
    uint32_t *stack_size,
    const uint8_t *types,
    uint32_t count,
    uint32_t reg_base) {
    uint32_t index;

    if (stack == NULL || stack_size == NULL)
        return false;

    for (index = 0u; index < count; ++index) {
        stack[*stack_size].reg = reg_base + index;
        stack[*stack_size].type = types[index];
        ++*stack_size;
    }
    return true;
}

static bool turbowasm_mir_frame_signature(
    const turbowasm_validation_context *validation,
    turbowasm_mir_control_frame *frame) {
    if (validation == NULL || frame == NULL ||
        frame->annotation == NULL)
        return false;

    return turbowasm_mir_integer_control_signature(
        validation, frame->annotation,
        &frame->start_types, &frame->start_count,
        &frame->end_types, &frame->end_count);
}

static bool turbowasm_mir_control_register_budget(
    const turbowasm_validation_context *validation,
    const turbowasm_validation_function *function,
    uint32_t *out_extra) {
    uint64_t total = 0u;
    uint32_t index;

    if (validation == NULL || function == NULL ||
        out_extra == NULL)
        return false;

    for (index = 0u; index < function->control_count; ++index) {
        const turbowasm_validation_control *control =
            &function->controls[index];
        uint32_t start_count = 0u;
        uint32_t end_count = 0u;

        if (!turbowasm_mir_integer_control_signature(
                validation, control,
                NULL, &start_count, NULL, &end_count))
            return false;

        total += (uint64_t)start_count + end_count;
        if (total > UINT32_MAX)
            return false;
    }

    *out_extra = (uint32_t)total;
    return true;
}


static bool turbowasm_mir_structured_emit_call(
    turbowasm_mir_text *text,
    const turbowasm_validation_context *validation,
    uint32_t callee_index,
    turbowasm_mir_stack_value *stack,
    uint32_t *stack_size,
    uint32_t *next_reg) {
    const turbowasm_validation_function *callee;
    const turbowasm_validation_func_type *callee_type;
    const char *proto_name = NULL;
    const char *external_name = NULL;
    uint32_t arg_index;
    uint32_t base;

    if (text == NULL || validation == NULL || stack == NULL ||
        stack_size == NULL || next_reg == NULL)
        return false;

    callee = turbowasm_validation_context_function(
        validation, callee_index);
    callee_type = turbowasm_validation_context_function_type(
        validation, callee_index);
    if (callee == NULL || callee->imported ||
        !turbowasm_mir_call_signature_supported(callee_type) ||
        !turbowasm_mir_integer_type(callee_type->results[0]) ||
        *stack_size < callee_type->param_count)
        return false;

    base = *stack_size - callee_type->param_count;
    for (arg_index = 0u;
         arg_index < callee_type->param_count;
         ++arg_index) {
        if (stack[base + arg_index].type !=
            callee_type->params[arg_index])
            return false;
    }

    if (callee_type->param_count == 0u) {
        proto_name = "tw_call_i64_0_p";
        external_name = "tw_jit_call_i64_0";
    } else if (callee_type->param_count == 1u) {
        proto_name = "tw_call_i64_1_p";
        external_name = "tw_jit_call_i64_1";
    } else if (callee_type->param_count == 2u) {
        proto_name = "tw_call_i64_2_p";
        external_name = "tw_jit_call_i64_2";
    } else {
        return false;
    }

    if (!turbowasm_mir_text_appendf(
            text,
            "call %s, %s, r%u, jit_ctx, %u",
            proto_name, external_name,
            *next_reg, callee_index))
        return false;

    for (arg_index = 0u;
         arg_index < callee_type->param_count;
         ++arg_index) {
        if (!turbowasm_mir_text_appendf(
                text, ", r%u",
                stack[base + arg_index].reg))
            return false;
    }

    if (!turbowasm_mir_text_appendf(
            text,
            "\n"
            "call tw_call_status_p, tw_jit_call_status, "
            "jit_status, jit_ctx\n"
            "bne jit_fail, jit_status, 0\n"))
        return false;

    *stack_size = base;
    stack[*stack_size].reg = (*next_reg)++;
    stack[*stack_size].type = callee_type->results[0];
    ++*stack_size;
    return true;
}

static bool turbowasm_mir_materialize_branch_target(
    turbowasm_mir_text *text,
    turbowasm_mir_control_frame *controls,
    uint32_t control_size,
    uint32_t depth,
    const turbowasm_mir_stack_value *stack,
    uint32_t stack_size,
    uint8_t function_result_type,
    turbowasm_mir_control_frame **out_target) {
    uint32_t target_index;
    turbowasm_mir_control_frame *target;

    if (text == NULL || controls == NULL ||
        stack == NULL || depth >= control_size)
        return false;

    target_index = control_size - 1u - depth;
    target = &controls[target_index];

    if (target->kind == TURBOWASM_MIR_CONTROL_FUNCTION) {
        if (stack_size == 0u ||
            stack[stack_size - 1u].type != function_result_type ||
            !turbowasm_mir_text_appendf(
                text, "mov jit_return_value, r%u\n",
                stack[stack_size - 1u].reg))
            return false;
    } else if (target->kind == TURBOWASM_MIR_CONTROL_LOOP) {
        if (target->annotation == NULL ||
            !turbowasm_mir_emit_stack_to_regs(
                text, stack, stack_size,
                target->start_types, target->start_count,
                target->start_reg_base))
            return false;
    } else {
        if (target->annotation == NULL ||
            !turbowasm_mir_emit_stack_to_regs(
                text, stack, stack_size,
                target->end_types, target->end_count,
                target->end_reg_base))
            return false;
        target->end_incoming = true;
    }

    if (out_target != NULL)
        *out_target = target;
    return true;
}

static bool turbowasm_mir_emit_target_jump(
    turbowasm_mir_text *text,
    turbowasm_mir_control_frame *target,
    bool conditional,
    uint32_t condition_reg) {
    if (text == NULL || target == NULL)
        return false;

    if (target->kind == TURBOWASM_MIR_CONTROL_FUNCTION) {
        return conditional
            ? turbowasm_mir_text_appendf(
                  text, "bt jit_return, r%u\n", condition_reg)
            : turbowasm_mir_text_appendf(text, "jmp jit_return\n");
    }

    if (target->annotation == NULL)
        return false;

    if (target->kind == TURBOWASM_MIR_CONTROL_LOOP) {
        return conditional
            ? turbowasm_mir_emit_control_branch_true(
                  text, target->annotation->opcode_offset,
                  "body", condition_reg)
            : turbowasm_mir_emit_control_jump(
                  text, target->annotation->opcode_offset, "body");
    }

    return conditional
        ? turbowasm_mir_emit_control_branch_true(
              text, target->annotation->opcode_offset,
              "end", condition_reg)
        : turbowasm_mir_emit_control_jump(
              text, target->annotation->opcode_offset, "end");
}

static bool turbowasm_mir_structured_branch_target(
    turbowasm_mir_text *text,
    turbowasm_mir_control_frame *controls,
    uint32_t control_size,
    uint32_t depth,
    turbowasm_mir_stack_value *stack,
    uint32_t stack_size,
    uint8_t function_result_type,
    bool conditional,
    uint32_t condition_reg,
    uint32_t branch_opcode_offset) {
    turbowasm_mir_control_frame *target = NULL;

    if (conditional) {
        /*
         * Materializing a loop branch can overwrite start registers that are
         * still live on the false fallthrough path.  Keep all merge writes on
         * a taken-only path.
         */
        if (!turbowasm_mir_text_appendf(
                text, "bf br_if_%u_fallthrough, r%u\n",
                branch_opcode_offset, condition_reg))
            return false;
    }

    if (!turbowasm_mir_materialize_branch_target(
            text, controls, control_size, depth,
            stack, stack_size, function_result_type,
            &target) ||
        !turbowasm_mir_emit_target_jump(
            text, target, false, 0u))
        return false;

    if (conditional &&
        !turbowasm_mir_text_appendf(
            text, "br_if_%u_fallthrough:\n",
            branch_opcode_offset))
        return false;

    return true;
}

static turbowasm_status turbowasm_mir_compile_structured_integer(
    turbowasm_mir_backend_context *backend,
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    const turbowasm_validation_func_type *type;
    turbowasm_mir_compiled *compiled = NULL;
    turbowasm_mir_stack_value *stack = NULL;
    turbowasm_mir_control_frame *controls = NULL;
    turbowasm_mir_text text = {0};
    turbowasm_reader reader;
    MIR_module_t module;
    MIR_item_t function_item;
    uint32_t register_count;
    uint32_t control_register_count = 0u;
    uint32_t next_reg = 0u;
    uint32_t stack_size = 0u;
    uint32_t control_size = 0u;
    uint32_t index;
    uint8_t result_type = 0u;
    uint32_t module_id;
    char function_name[64];
    bool reachable = true;
    bool finished = false;
    turbowasm_status status = TURBOWASM_UNSUPPORTED;

    if (backend == NULL || backend->mir == NULL ||
        validation == NULL || function == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    out->impl = NULL;

    if (!turbowasm_mir_scan_structured_scalar(
            validation, function_index, function, &result_type))
        return TURBOWASM_UNSUPPORTED;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_mir_control_register_budget(
            validation, function, &control_register_count))
        return TURBOWASM_UNSUPPORTED;

    if ((uint64_t)function->code_size + 1u +
            (uint64_t)control_register_count >
        UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    register_count =
        function->code_size + 1u + control_register_count;
    stack = (turbowasm_mir_stack_value *)calloc(
        (size_t)register_count + 1u, sizeof(*stack));
    controls = (turbowasm_mir_control_frame *)calloc(
        (size_t)function->control_count + 1u, sizeof(*controls));
    if (stack == NULL || controls == NULL) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    controls[0].kind = TURBOWASM_MIR_CONTROL_FUNCTION;
    controls[0].height = 0u;
    controls[0].start_reg_base = UINT32_MAX;
    controls[0].end_reg_base = UINT32_MAX;
    control_size = 1u;

    module_id = backend->next_module_id++;
    if (snprintf(
            function_name, sizeof(function_name),
            "tw_jit_f_%u", module_id) <= 0)
        goto done;

    if (!turbowasm_mir_text_appendf(
            &text,
            "tw_jit_m_%u: module\n"
            "tw_call_i64_0_p: proto i64, p:ctx, i64:index\n"
            "tw_call_i64_1_p: proto i64, p:ctx, i64:index, i64:a0\n"
            "tw_call_i64_2_p: proto i64, p:ctx, i64:index, i64:a0, i64:a1\n"
            "tw_call_status_p: proto i64, p:ctx\n"
            "tw_checkpoint_p: proto i64, p:ctx\n"
            "import tw_jit_call_i64_0, tw_jit_call_i64_1, "
            "tw_jit_call_i64_2, tw_jit_call_status, "
            "tw_jit_checkpoint\n"
            "export %s\n"
            "%s: func i64, p:jit_ctx",
            module_id, function_name, function_name))
        goto oom;

    for (index = 0u; index < type->param_count; ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, ", i64:l%u", index))
            goto oom;
    }
    if (!turbowasm_mir_text_appendf(&text, "\n"))
        goto oom;

    for (index = type->param_count;
         index < function->local_count;
         ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, "local i64:l%u\n", index))
            goto oom;
    }
    for (index = 0u; index < register_count; ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, "local i64:r%u\n", index))
            goto oom;
    }
    if (!turbowasm_mir_text_appendf(
            &text,
            "local i64:jit_status\n"
            "local i64:jit_return_value\n"
            "local i64:jit_fail_value\n"))
        goto oom;

    for (index = type->param_count;
         index < function->local_count;
         ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, "mov l%u, 0\n", index))
            goto oom;
    }

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;
        uint32_t opcode_offset;

        if (!reachable) {
            if (control_size <= 1u) {
                if (function->code_size == 0u)
                    goto done;
                reader.cursor =
                    function->code + function->code_size - 1u;
            } else {
                turbowasm_mir_control_frame *frame =
                    &controls[control_size - 1u];
                uint32_t boundary;
                uint32_t current =
                    (uint32_t)(reader.cursor - function->code);

                if (frame->annotation == NULL ||
                    frame->annotation->end_offset == UINT32_MAX)
                    goto done;

                if (frame->kind == TURBOWASM_MIR_CONTROL_IF &&
                    !frame->else_seen &&
                    frame->annotation->else_offset != UINT32_MAX &&
                    current <= frame->annotation->else_offset) {
                    boundary = frame->annotation->else_offset;
                } else {
                    boundary = frame->annotation->end_offset;
                }

                if (boundary > function->code_size)
                    goto done;
                reader.cursor = function->code + boundary;
            }
        }

        if (!turbowasm_reader_u8(&reader, &opcode) ||
            reader.cursor <= function->code ||
            (size_t)(reader.cursor - function->code - 1u) > UINT32_MAX)
            goto done;

        opcode_offset =
            (uint32_t)(reader.cursor - function->code - 1u);

        if (opcode == 0x05u) { /* else */
            turbowasm_mir_control_frame *frame;

            if (control_size <= 1u)
                goto done;
            frame = &controls[control_size - 1u];
            if (frame->kind != TURBOWASM_MIR_CONTROL_IF ||
                frame->annotation == NULL ||
                frame->annotation->else_offset != opcode_offset)
                goto done;

            if (reachable) {
                /*
                 * The else opcode is executed only on the natural then path.
                 * Pay its checkpoint first, then materialize the then result
                 * into the shared end registers before transferring control.
                 */
                if (!turbowasm_mir_emit_checkpoint_text(&text))
                    goto oom;
                if (!turbowasm_mir_emit_stack_to_regs(
                        &text, stack, stack_size,
                        frame->end_types, frame->end_count,
                        frame->end_reg_base))
                    goto done;
                if (!turbowasm_mir_emit_control_jump(
                        &text, frame->annotation->opcode_offset,
                        "end_opcode"))
                    goto oom;
                frame->end_opcode_incoming = true;
            }

            if (!turbowasm_mir_emit_control_label(
                    &text, frame->annotation->opcode_offset,
                    "else_body"))
                goto oom;

            stack_size = frame->height;
            if (!turbowasm_mir_push_regs(
                    stack, &stack_size,
                    frame->start_types, frame->start_count,
                    frame->start_reg_base))
                goto done;

            frame->else_seen = true;
            reachable = true;
            continue;
        }

        if (opcode == 0x0bu) { /* end */
            if (control_size == 1u) {
                if (turbowasm_reader_remaining(&reader) != 0u)
                    goto done;

                if (reachable) {
                    if (!turbowasm_mir_emit_checkpoint_text(&text) ||
                        stack_size != 1u ||
                        stack[0].type != result_type ||
                        !turbowasm_mir_text_appendf(
                            &text,
                            "mov jit_return_value, r%u\n",
                            stack[0].reg))
                        goto done;
                }

                if (!turbowasm_mir_text_appendf(
                        &text,
                        "jit_return:\n"
                        "ret jit_return_value\n"
                        "jit_fail:\n"
                        "mov jit_fail_value, 0\n"
                        "ret jit_fail_value\n"
                        "endfunc\n"
                        "endmodule\n"))
                    goto oom;

                finished = true;
                break;
            } else {
                turbowasm_mir_control_frame *frame =
                    &controls[control_size - 1u];
                bool end_opcode_reachable;
                bool next_reachable;

                if (frame->annotation == NULL ||
                    frame->annotation->end_offset != opcode_offset)
                    goto done;

                /*
                 * Natural fallthrough materializes before the end label so a
                 * direct br-to-end skips these moves and keeps its own branch
                 * arguments in the same merge registers.
                 */
                if (reachable &&
                    !turbowasm_mir_emit_stack_to_regs(
                        &text, stack, stack_size,
                        frame->end_types, frame->end_count,
                        frame->end_reg_base))
                    goto done;

                end_opcode_reachable =
                    reachable || frame->end_opcode_incoming;

                if (!turbowasm_mir_emit_control_label(
                        &text, frame->annotation->opcode_offset,
                        "end_opcode"))
                    goto oom;

                if (end_opcode_reachable &&
                    !turbowasm_mir_emit_checkpoint_text(&text))
                    goto oom;

                if (!turbowasm_mir_emit_control_label(
                        &text, frame->annotation->opcode_offset,
                        "end"))
                    goto oom;

                next_reachable =
                    end_opcode_reachable || frame->end_incoming;

                stack_size = frame->height;
                if (next_reachable &&
                    !turbowasm_mir_push_regs(
                        stack, &stack_size,
                        frame->end_types, frame->end_count,
                        frame->end_reg_base))
                    goto done;

                --control_size;
                reachable = next_reachable;
                continue;
            }
        }

        if (!reachable)
            goto done;

        if (!turbowasm_mir_emit_checkpoint_text(&text))
            goto oom;

        switch (opcode) {
            case 0x01u: /* nop */
                break;

            case 0x02u: /* block */
            case 0x03u: /* loop */
            case 0x04u: { /* if */
                const turbowasm_validation_control *annotation;
                turbowasm_mir_control_frame *frame;
                const uint8_t *start_types = NULL;
                const uint8_t *end_types = NULL;
                uint32_t start_count = 0u;
                uint32_t end_count = 0u;
                uint32_t current_offset;
                uint32_t condition_reg = 0u;

                annotation =
                    turbowasm_validation_function_control_at(
                        function, opcode_offset);
                current_offset =
                    (uint32_t)(reader.cursor - function->code);

                if (annotation == NULL ||
                    annotation->end_offset == UINT32_MAX ||
                    annotation->body_offset < current_offset ||
                    annotation->body_offset > function->code_size ||
                    control_size > function->control_count ||
                    !turbowasm_mir_integer_control_signature(
                        validation, annotation,
                        &start_types, &start_count,
                        &end_types, &end_count))
                    goto done;

                if (opcode == 0x04u) {
                    if (stack_size == 0u ||
                        stack[stack_size - 1u].type != 0x7fu)
                        goto done;
                    condition_reg = stack[--stack_size].reg;
                }

                if (!turbowasm_mir_stack_matches_types(
                        stack, stack_size,
                        start_types, start_count))
                    goto done;

                frame = &controls[control_size++];
                memset(frame, 0, sizeof(*frame));
                frame->kind =
                    turbowasm_mir_control_kind_from_annotation(
                        annotation);
                frame->annotation = annotation;
                frame->start_types = start_types;
                frame->start_count = start_count;
                frame->end_types = end_types;
                frame->end_count = end_count;
                frame->start_reg_base =
                    start_count == 0u ? UINT32_MAX : next_reg;
                next_reg += start_count;
                frame->end_reg_base =
                    end_count == 0u ? UINT32_MAX : next_reg;
                next_reg += end_count;

                if (next_reg > register_count)
                    goto done;

                frame->height = stack_size - start_count;

                if (start_count != 0u &&
                    !turbowasm_mir_emit_stack_to_regs(
                        &text, stack, stack_size,
                        start_types, start_count,
                        frame->start_reg_base))
                    goto done;

                stack_size = frame->height;
                if (!turbowasm_mir_push_regs(
                        stack, &stack_size,
                        start_types, start_count,
                        frame->start_reg_base))
                    goto done;

                /*
                 * Skip the blocktype bytes using validator-retained metadata.
                 */
                reader.cursor =
                    function->code + annotation->body_offset;

                if (opcode == 0x03u) {
                    if (!turbowasm_mir_emit_control_label(
                            &text, annotation->opcode_offset,
                            "body"))
                        goto oom;
                } else if (opcode == 0x04u) {
                    if (annotation->else_offset != UINT32_MAX) {
                        if (!turbowasm_mir_emit_control_branch_false(
                                &text, annotation->opcode_offset,
                                "else_body", condition_reg))
                            goto oom;
                    } else {
                        /*
                         * Validator guarantees start/end types are equal for
                         * an if without else. Seed the false-path result before
                         * branching to the shared end-opcode checkpoint.
                         */
                        if (start_count != end_count ||
                            (start_count != 0u &&
                             memcmp(start_types, end_types, start_count) != 0))
                            goto done;
                        if (end_count != 0u &&
                            !turbowasm_mir_emit_stack_to_regs(
                                &text, stack, stack_size,
                                end_types, end_count,
                                frame->end_reg_base))
                            goto done;
                        if (!turbowasm_mir_emit_control_branch_false(
                                &text, annotation->opcode_offset,
                                "end_opcode", condition_reg))
                            goto oom;
                        frame->end_opcode_incoming = true;
                    }
                }
                break;
            }

            case 0x0cu: /* br */
            case 0x0du: { /* br_if */
                uint32_t depth;
                uint32_t condition_reg = 0u;
                bool conditional = opcode == 0x0du;

                if (!turbowasm_reader_uleb32(&reader, &depth) ||
                    depth >= control_size)
                    goto done;

                if (conditional) {
                    if (stack_size == 0u ||
                        stack[stack_size - 1u].type != 0x7fu)
                        goto done;
                    condition_reg = stack[--stack_size].reg;
                }

                if (!turbowasm_mir_structured_branch_target(
                        &text, controls, control_size, depth,
                        stack, stack_size, result_type,
                        conditional, condition_reg, opcode_offset))
                    goto oom;

                if (!conditional) {
                    stack_size =
                        controls[control_size - 1u].height;
                    reachable = false;
                }
                break;
            }

            case 0x0eu: { /* br_table */
                uint32_t count;
                uint32_t case_index;
                uint32_t selector_reg;
                uint32_t *depths = NULL;
                turbowasm_mir_control_frame *target = NULL;

                if (!turbowasm_reader_uleb32(&reader, &count) ||
                    stack_size == 0u ||
                    stack[stack_size - 1u].type != 0x7fu)
                    goto done;

                if ((uint64_t)count + 1u >
                    (uint64_t)SIZE_MAX / sizeof(*depths))
                    goto oom;

                depths = (uint32_t *)calloc(
                    (size_t)count + 1u, sizeof(*depths));
                if (depths == NULL)
                    goto oom;

                for (case_index = 0u;
                     case_index < count + 1u;
                     ++case_index) {
                    if (!turbowasm_reader_uleb32(
                            &reader, &depths[case_index]) ||
                        depths[case_index] >= control_size) {
                        free(depths);
                        goto done;
                    }
                }

                selector_reg = stack[--stack_size].reg;

                /*
                 * Dispatch first without touching any target merge register.
                 * Each taken-only trampoline performs its own materialization.
                 */
                for (case_index = 0u; case_index < count; ++case_index) {
                    if (!turbowasm_mir_text_appendf(
                            &text,
                            "beq br_table_%u_%u, r%u, %u\n",
                            opcode_offset, case_index,
                            selector_reg, case_index)) {
                        free(depths);
                        goto oom;
                    }
                }

                if (!turbowasm_mir_text_appendf(
                        &text, "jmp br_table_%u_default\n",
                        opcode_offset)) {
                    free(depths);
                    goto oom;
                }

                for (case_index = 0u; case_index < count; ++case_index) {
                    if (!turbowasm_mir_text_appendf(
                            &text, "br_table_%u_%u:\n",
                            opcode_offset, case_index) ||
                        !turbowasm_mir_materialize_branch_target(
                            &text, controls, control_size,
                            depths[case_index],
                            stack, stack_size, result_type,
                            &target) ||
                        !turbowasm_mir_emit_target_jump(
                            &text, target, false, 0u)) {
                        free(depths);
                        goto done;
                    }
                }

                if (!turbowasm_mir_text_appendf(
                        &text, "br_table_%u_default:\n",
                        opcode_offset) ||
                    !turbowasm_mir_materialize_branch_target(
                        &text, controls, control_size,
                        depths[count],
                        stack, stack_size, result_type,
                        &target) ||
                    !turbowasm_mir_emit_target_jump(
                        &text, target, false, 0u)) {
                    free(depths);
                    goto done;
                }

                free(depths);
                stack_size = controls[control_size - 1u].height;
                reachable = false;
                break;
            }

            case 0x0fu: /* return */
                if (stack_size == 0u ||
                    stack[stack_size - 1u].type != result_type ||
                    !turbowasm_mir_text_appendf(
                        &text,
                        "mov jit_return_value, r%u\n"
                        "jmp jit_return\n",
                        stack[stack_size - 1u].reg))
                    goto done;
                reachable = false;
                break;

            case 0x10u: { /* call */
                uint32_t callee_index;
                if (!turbowasm_reader_uleb32(
                        &reader, &callee_index) ||
                    !turbowasm_mir_structured_emit_call(
                        &text, validation, callee_index,
                        stack, &stack_size, &next_reg))
                    goto done;
                break;
            }

            case 0x1au: /* drop */
                if (stack_size == 0u)
                    goto done;
                --stack_size;
                break;

            case 0x20u: { /* local.get */
                uint32_t local_index;
                uint8_t local_type;
                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count)
                    goto done;
                local_type = function->local_types[local_index];
                if (!turbowasm_mir_text_appendf(
                        &text, "mov r%u, l%u\n",
                        next_reg, local_index))
                    goto oom;
                stack[stack_size].reg = next_reg++;
                stack[stack_size].type = local_type;
                ++stack_size;
                break;
            }

            case 0x21u: /* local.set */
            case 0x22u: { /* local.tee */
                uint32_t local_index;
                turbowasm_mir_stack_value value;
                if (!turbowasm_reader_uleb32(
                        &reader, &local_index) ||
                    local_index >= function->local_count ||
                    stack_size == 0u)
                    goto done;
                value = stack[stack_size - 1u];
                if (value.type != function->local_types[local_index] ||
                    !turbowasm_mir_text_appendf(
                        &text, "mov l%u, r%u\n",
                        local_index, value.reg))
                    goto done;
                if (opcode == 0x21u)
                    --stack_size;
                break;
            }

            case 0x41u: {
                int32_t value;
                if (!turbowasm_reader_sleb32(&reader, &value) ||
                    !turbowasm_mir_text_appendf(
                        &text, "mov r%u, %lld\n",
                        next_reg, (long long)value))
                    goto done;
                stack[stack_size].reg = next_reg++;
                stack[stack_size].type = 0x7fu;
                ++stack_size;
                break;
            }

            case 0x42u: {
                int64_t value;
                if (!turbowasm_reader_sleb64(&reader, &value) ||
                    !turbowasm_mir_text_appendf(
                        &text, "mov r%u, %lld\n",
                        next_reg, (long long)value))
                    goto done;
                stack[stack_size].reg = next_reg++;
                stack[stack_size].type = 0x7eu;
                ++stack_size;
                break;
            }

            case 0x6au: case 0x6bu: case 0x6cu:
            case 0x7cu: case 0x7du: case 0x7eu: {
                const char *name = turbowasm_mir_binary_name(opcode);
                uint8_t expected = turbowasm_mir_binary_type(opcode);
                turbowasm_mir_stack_value right;
                turbowasm_mir_stack_value left;

                if (name == NULL || expected == 0u ||
                    stack_size < 2u)
                    goto done;
                right = stack[--stack_size];
                left = stack[--stack_size];
                if (left.type != expected || right.type != expected)
                    goto done;
                if (!turbowasm_mir_text_appendf(
                        &text, "%s r%u, r%u, r%u\n",
                        name, next_reg,
                        left.reg, right.reg))
                    goto oom;
                stack[stack_size].reg = next_reg++;
                stack[stack_size].type = expected;
                ++stack_size;
                break;
            }

            default:
                goto done;
        }
    }

    if (!finished || text.data == NULL)
        goto done;

    MIR_scan_string(backend->mir, text.data);

    module = DLIST_TAIL(
        MIR_module_t, *MIR_get_module_list(backend->mir));
    if (module == NULL)
        goto done;

    function_item = DLIST_TAIL(MIR_item_t, module->items);
    if (function_item == NULL ||
        strcmp(
            MIR_item_name(backend->mir, function_item),
            function_name) != 0)
        goto done;

    MIR_load_module(backend->mir, module);
    MIR_link(backend->mir, MIR_set_gen_interface, NULL);

    compiled = (turbowasm_mir_compiled *)calloc(
        1u, sizeof(*compiled));
    if (compiled == NULL)
        goto oom;

    compiled->generated = MIR_gen(backend->mir, function_item);
    if (compiled->generated == NULL)
        goto done;

    compiled->result_type = result_type;
    compiled->param_count = (uint8_t)type->param_count;
    for (index = 0u; index < type->param_count; ++index)
        compiled->param_types[index] = type->params[index];

    out->impl = compiled;
    compiled = NULL;
    status = TURBOWASM_OK;
    goto done;

oom:
    status = TURBOWASM_OUT_OF_MEMORY;

done:
    free(compiled);
    free(controls);
    free(stack);
    free(text.data);
    return status;
}

static turbowasm_status turbowasm_mir_compile_function(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    turbowasm_mir_backend_context *backend =
        (turbowasm_mir_backend_context *)context;
    const turbowasm_validation_func_type *type;
    turbowasm_mir_compiled *compiled = NULL;
    turbowasm_mir_stack_value *stack = NULL;
    turbowasm_mir_text text = {0};
    turbowasm_reader reader;
    MIR_module_t module;
    MIR_item_t function_item;
    uint32_t register_count = 0u;
    uint32_t next_reg = 0u;
    uint32_t stack_size = 0u;
    uint32_t index;
    uint8_t result_type = 0u;
    uint32_t module_id;
    char function_name[64];
    bool finished = false;
    turbowasm_status status = TURBOWASM_UNSUPPORTED;
    const char *result_name;

    if (backend == NULL || backend->mir == NULL ||
        validation == NULL || function == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    out->impl = NULL;
    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_mir_scan_scalar_locals(
            validation, function_index, function,
            &register_count, &result_type)) {
        return turbowasm_mir_compile_structured_integer(
            backend, validation, function_index, function, out);
    }

    result_name = turbowasm_mir_type_name(result_type);
    if (result_name == NULL)
        return TURBOWASM_UNSUPPORTED;

    stack = (turbowasm_mir_stack_value *)calloc(
        (size_t)function->code_size + 1u,
        sizeof(*stack));
    if (stack == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    module_id = backend->next_module_id++;
    if (snprintf(
            function_name, sizeof(function_name),
            "tw_jit_f_%u", module_id) <= 0)
        goto done;

    if (!turbowasm_mir_text_appendf(
            &text,
            "tw_jit_m_%u: module\n"
            "tw_call_i64_0_p: proto i64, p:ctx, i64:index\n"
            "tw_call_i64_1_p: proto i64, p:ctx, i64:index, i64:a0\n"
            "tw_call_i64_2_p: proto i64, p:ctx, i64:index, i64:a0, i64:a1\n"
            "tw_call_f32_0_p: proto f, p:ctx, i64:index\n"
            "tw_call_f32_1_p: proto f, p:ctx, i64:index, f:a0\n"
            "tw_call_f64_0_p: proto d, p:ctx, i64:index\n"
            "tw_call_f64_1_p: proto d, p:ctx, i64:index, d:a0\n"
            "tw_call_status_p: proto i64, p:ctx\n"
            "tw_checkpoint_p: proto i64, p:ctx\n"
            "import tw_jit_call_i64_0, tw_jit_call_i64_1, "
            "tw_jit_call_i64_2, tw_jit_call_f32_0, "
            "tw_jit_call_f32_1, tw_jit_call_f64_0, "
            "tw_jit_call_f64_1, tw_jit_call_status, "
            "tw_jit_checkpoint\n"
            "export %s\n"
            "%s: func %s, p:jit_ctx",
            module_id, function_name, function_name,
            result_name))
        goto oom;

    for (index = 0u; index < type->param_count; ++index) {
        const char *param_name =
            turbowasm_mir_type_name(type->params[index]);
        if (param_name == NULL ||
            !turbowasm_mir_text_appendf(
                &text, ", %s:l%u", param_name, index))
            goto oom;
    }
    if (!turbowasm_mir_text_appendf(&text, "\n"))
        goto oom;

    for (index = type->param_count;
         index < function->local_count;
         ++index) {
        const char *local_name =
            turbowasm_mir_type_name(function->local_types[index]);
        if (local_name == NULL ||
            !turbowasm_mir_text_appendf(
                &text, "local %s:l%u\n",
                local_name, index))
            goto oom;
    }

    for (index = 0u; index < register_count; ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, "local %s:r%u\n",
                result_name, index))
            goto oom;
    }

    if (!turbowasm_mir_text_appendf(
            &text,
            "local i64:jit_status\n"
            "local %s:jit_fail_value\n",
            result_name))
        goto oom;

    for (index = type->param_count;
         index < function->local_count;
         ++index) {
        uint8_t local_type = function->local_types[index];
        const char *move_name =
            turbowasm_mir_move_name(local_type);
        const char *zero =
            local_type == 0x7du ? "0.0f" :
            local_type == 0x7cu ? "0.0" : "0";
        if (move_name == NULL ||
            !turbowasm_mir_text_appendf(
                &text, "%s l%u, %s\n",
                move_name, index, zero))
            goto oom;
    }

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&reader, &opcode))
            goto done;

        /*
         * Match the interpreter's #60 accounting exactly: one checkpoint
         * before every executed Wasm instruction, including the final end.
         */
        if (!turbowasm_mir_text_appendf(
                &text,
                "call tw_checkpoint_p, tw_jit_checkpoint, "
                "jit_status, jit_ctx\n"
                "bne jit_fail, jit_status, 0\n"))
            goto oom;

        if (opcode == 0x10u) {
            uint32_t callee_index;
            const turbowasm_validation_function *callee;
            const turbowasm_validation_func_type *callee_type;
            const char *proto_name = NULL;
            const char *external_name = NULL;
            uint32_t arg_index;
            uint32_t base;

            if (!turbowasm_reader_uleb32(
                    &reader, &callee_index))
                goto done;

            callee = turbowasm_validation_context_function(
                validation, callee_index);
            callee_type =
                turbowasm_validation_context_function_type(
                    validation, callee_index);
            if (callee == NULL || callee->imported ||
                !turbowasm_mir_call_signature_supported(
                    callee_type) ||
                stack_size < callee_type->param_count)
                goto done;

            base = stack_size - callee_type->param_count;
            for (arg_index = 0u;
                 arg_index < callee_type->param_count;
                 ++arg_index) {
                if (stack[base + arg_index].type !=
                    callee_type->params[arg_index])
                    goto done;
            }

            if (turbowasm_mir_integer_type(
                    callee_type->results[0])) {
                if (callee_type->param_count == 0u) {
                    proto_name = "tw_call_i64_0_p";
                    external_name = "tw_jit_call_i64_0";
                } else if (callee_type->param_count == 1u) {
                    proto_name = "tw_call_i64_1_p";
                    external_name = "tw_jit_call_i64_1";
                } else if (callee_type->param_count == 2u) {
                    proto_name = "tw_call_i64_2_p";
                    external_name = "tw_jit_call_i64_2";
                }
            } else if (callee_type->results[0] == 0x7du) {
                if (callee_type->param_count == 0u) {
                    proto_name = "tw_call_f32_0_p";
                    external_name = "tw_jit_call_f32_0";
                } else if (callee_type->param_count == 1u) {
                    proto_name = "tw_call_f32_1_p";
                    external_name = "tw_jit_call_f32_1";
                }
            } else if (callee_type->results[0] == 0x7cu) {
                if (callee_type->param_count == 0u) {
                    proto_name = "tw_call_f64_0_p";
                    external_name = "tw_jit_call_f64_0";
                } else if (callee_type->param_count == 1u) {
                    proto_name = "tw_call_f64_1_p";
                    external_name = "tw_jit_call_f64_1";
                }
            }

            if (proto_name == NULL || external_name == NULL)
                goto done;

            if (!turbowasm_mir_text_appendf(
                    &text,
                    "call %s, %s, r%u, jit_ctx, %u",
                    proto_name,
                    external_name,
                    next_reg,
                    callee_index))
                goto oom;

            for (arg_index = 0u;
                 arg_index < callee_type->param_count;
                 ++arg_index) {
                if (!turbowasm_mir_text_appendf(
                        &text, ", r%u",
                        stack[base + arg_index].reg))
                    goto oom;
            }

            if (!turbowasm_mir_text_appendf(
                    &text,
                    "\n"
                    "call tw_call_status_p, tw_jit_call_status, "
                    "jit_status, jit_ctx\n"
                    "bne jit_fail, jit_status, 0\n"))
                goto oom;

            stack_size = base;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type =
                callee_type->results[0];
            ++stack_size;
            continue;
        }

        if (opcode == 0x20u) {
            uint32_t local_index;
            uint8_t local_type;
            const char *move_name;
            if (!turbowasm_reader_uleb32(
                    &reader, &local_index) ||
                local_index >= function->local_count)
                goto done;
            local_type = function->local_types[local_index];
            move_name = turbowasm_mir_move_name(local_type);
            if (move_name == NULL ||
                !turbowasm_mir_text_appendf(
                    &text, "%s r%u, l%u\n",
                    move_name, next_reg, local_index))
                goto oom;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = local_type;
            ++stack_size;
            continue;
        }

        if (opcode == 0x21u || opcode == 0x22u) {
            uint32_t local_index;
            turbowasm_mir_stack_value value;
            const char *move_name;
            if (!turbowasm_reader_uleb32(
                    &reader, &local_index) ||
                local_index >= function->local_count ||
                stack_size == 0u)
                goto done;
            value = stack[stack_size - 1u];
            if (value.type != function->local_types[local_index])
                goto done;
            move_name = turbowasm_mir_move_name(value.type);
            if (move_name == NULL ||
                !turbowasm_mir_text_appendf(
                    &text, "%s l%u, r%u\n",
                    move_name, local_index, value.reg))
                goto oom;
            if (opcode == 0x21u)
                --stack_size;
            continue;
        }

        if (opcode == 0x41u) {
            int32_t value;
            if (!turbowasm_reader_sleb32(&reader, &value))
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov r%u, %lld\n",
                    next_reg, (long long)value))
                goto oom;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = 0x7fu;
            ++stack_size;
            continue;
        }

        if (opcode == 0x42u) {
            int64_t value;
            if (!turbowasm_reader_sleb64(&reader, &value))
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov r%u, %lld\n",
                    next_reg, (long long)value))
                goto oom;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = 0x7eu;
            ++stack_size;
            continue;
        }

        if (opcode == 0x43u) {
            uint32_t bits;
            float value;
            if (!turbowasm_reader_u32le(&reader, &bits))
                goto done;
            memcpy(&value, &bits, sizeof(value));
            if (!isfinite(value) ||
                !turbowasm_mir_text_appendf(
                    &text, "fmov r%u, %.*ef\n",
                    next_reg,
                    FLT_DECIMAL_DIG - 1,
                    (double)value))
                goto done;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = 0x7du;
            ++stack_size;
            continue;
        }

        if (opcode == 0x44u) {
            turbowasm_reader bytes;
            uint64_t bits = 0u;
            double value;
            if (!turbowasm_reader_slice(&reader, 8u, &bytes))
                goto done;
            for (index = 0u; index < 8u; ++index)
                bits |= (uint64_t)bytes.cursor[index] << (8u * index);
            memcpy(&value, &bits, sizeof(value));
            if (!isfinite(value) ||
                !turbowasm_mir_text_appendf(
                    &text, "dmov r%u, %.*e\n",
                    next_reg,
                    DBL_DECIMAL_DIG - 1,
                    value))
                goto done;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = 0x7cu;
            ++stack_size;
            continue;
        }

        if (turbowasm_mir_binary_type(opcode) != 0u) {
            const char *name = turbowasm_mir_binary_name(opcode);
            turbowasm_mir_stack_value right;
            turbowasm_mir_stack_value left;
            uint8_t expected =
                turbowasm_mir_binary_type(opcode);

            if (name == NULL || stack_size < 2u)
                goto done;

            right = stack[--stack_size];
            left = stack[--stack_size];
            if (left.type != expected || right.type != expected)
                goto done;

            if (!turbowasm_mir_text_appendf(
                    &text, "%s r%u, r%u, r%u\n",
                    name, next_reg, left.reg, right.reg))
                goto oom;

            stack[stack_size].reg = next_reg++;
            stack[stack_size].type = expected;
            ++stack_size;
            continue;
        }

        if (opcode == 0x0bu) {
            if (turbowasm_reader_remaining(&reader) != 0u ||
                stack_size != 1u ||
                stack[0].type != result_type)
                goto done;
            {
                const char *fail_move =
                    turbowasm_mir_move_name(result_type);
                const char *fail_zero =
                    result_type == 0x7du ? "0.0f" :
                    result_type == 0x7cu ? "0.0" : "0";
                if (fail_move == NULL ||
                    !turbowasm_mir_text_appendf(
                        &text,
                        "ret r%u\n"
                        "jit_fail:\n"
                        "%s jit_fail_value, %s\n"
                        "ret jit_fail_value\n"
                        "endfunc\n"
                        "endmodule\n",
                        stack[0].reg,
                        fail_move,
                        fail_zero))
                    goto oom;
            }
            finished = true;
            break;
        }

        goto done;
    }

    if (!finished || text.data == NULL)
        goto done;

    MIR_scan_string(backend->mir, text.data);

    module = DLIST_TAIL(
        MIR_module_t, *MIR_get_module_list(backend->mir));
    if (module == NULL)
        goto done;

    function_item = DLIST_TAIL(MIR_item_t, module->items);
    if (function_item == NULL ||
        strcmp(
            MIR_item_name(backend->mir, function_item),
            function_name) != 0)
        goto done;

    MIR_load_module(backend->mir, module);
    MIR_link(backend->mir, MIR_set_gen_interface, NULL);

    compiled = (turbowasm_mir_compiled *)calloc(
        1u, sizeof(*compiled));
    if (compiled == NULL)
        goto oom;

    compiled->generated = MIR_gen(backend->mir, function_item);
    if (compiled->generated == NULL)
        goto done;

    compiled->result_type = result_type;
    compiled->param_count = (uint8_t)type->param_count;
    for (index = 0u; index < type->param_count; ++index)
        compiled->param_types[index] = type->params[index];

    out->impl = compiled;
    compiled = NULL;
    status = TURBOWASM_OK;
    goto done;

oom:
    status = TURBOWASM_OUT_OF_MEMORY;

done:
    free(compiled);
    free(stack);
    free(text.data);
    return status;
}

static bool turbowasm_mir_argument_raw(
    const turbowasm_value *value,
    uint8_t type,
    int64_t *out) {
    if (value == NULL || out == NULL)
        return false;

    if (type == 0x7fu &&
        value->kind == TURBOWASM_VALUE_I32) {
        *out = (int64_t)value->as.i32;
        return true;
    }

    if (type == 0x7eu &&
        value->kind == TURBOWASM_VALUE_I64) {
        *out = value->as.i64;
        return true;
    }

    return false;
}

static bool turbowasm_mir_argument_matches(
    const turbowasm_value *value,
    uint8_t type) {
    if (value == NULL)
        return false;

    switch (type) {
        case 0x7fu: return value->kind == TURBOWASM_VALUE_I32;
        case 0x7eu: return value->kind == TURBOWASM_VALUE_I64;
        case 0x7du: return value->kind == TURBOWASM_VALUE_F32;
        case 0x7cu: return value->kind == TURBOWASM_VALUE_F64;
        default: return false;
    }
}

static turbowasm_status turbowasm_mir_invoke_compiled(
    const turbowasm_compiled_function *compiled,
    turbowasm_jit_invocation_context *context,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    const turbowasm_mir_compiled *function;
    void *generated;
    uint32_t index;

    if (compiled == NULL || compiled->impl == NULL ||
        context == NULL || context->instance == NULL ||
        results == NULL || result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;
    context->call_status = TURBOWASM_OK;
    context->call_trap = TURBOWASM_TRAP_NONE;

    if (result_capacity < 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    function = (const turbowasm_mir_compiled *)compiled->impl;
    if (argument_count != function->param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < function->param_count; ++index) {
        if (!turbowasm_mir_argument_matches(
                &arguments[index],
                function->param_types[index]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    generated = function->generated;
    if (generated == NULL)
        return TURBOWASM_UNSUPPORTED;

    if (function->result_type == 0x7fu ||
        function->result_type == 0x7eu) {
        int64_t raw_args[2] = {0, 0};
        int64_t raw;

        for (index = 0u; index < function->param_count; ++index) {
            if (!turbowasm_mir_argument_raw(
                    &arguments[index],
                    function->param_types[index],
                    &raw_args[index]))
                return TURBOWASM_TYPE_MISMATCH;
        }

        if (function->param_count == 0u) {
            int64_t (*entry)(turbowasm_jit_invocation_context *);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context);
        } else if (function->param_count == 1u) {
            int64_t (*entry)(
                turbowasm_jit_invocation_context *,
                int64_t);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context, raw_args[0]);
        } else if (function->param_count == 2u) {
            int64_t (*entry)(
                turbowasm_jit_invocation_context *,
                int64_t,
                int64_t);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context, raw_args[0], raw_args[1]);
        } else {
            return TURBOWASM_UNSUPPORTED;
        }

        if (context->call_status != TURBOWASM_OK) {
            *trap = context->call_trap;
            return context->call_status;
        }

        if (function->result_type == 0x7fu) {
            results[0].kind = TURBOWASM_VALUE_I32;
            results[0].as.i32 = (int32_t)(uint32_t)raw;
        } else {
            results[0].kind = TURBOWASM_VALUE_I64;
            results[0].as.i64 = raw;
        }
    } else if (function->result_type == 0x7du) {
        float raw;

        if (function->param_count == 0u) {
            float (*entry)(turbowasm_jit_invocation_context *);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context);
        } else if (function->param_count == 1u &&
                   function->param_types[0] == 0x7du) {
            float (*entry)(
                turbowasm_jit_invocation_context *,
                float);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context, arguments[0].as.f32);
        } else {
            return TURBOWASM_UNSUPPORTED;
        }

        if (context->call_status != TURBOWASM_OK) {
            *trap = context->call_trap;
            return context->call_status;
        }

        results[0].kind = TURBOWASM_VALUE_F32;
        results[0].as.f32 = raw;
    } else if (function->result_type == 0x7cu) {
        double raw;

        if (function->param_count == 0u) {
            double (*entry)(turbowasm_jit_invocation_context *);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context);
        } else if (function->param_count == 1u &&
                   function->param_types[0] == 0x7cu) {
            double (*entry)(
                turbowasm_jit_invocation_context *,
                double);
            _Static_assert(
                sizeof(entry) == sizeof(generated),
                "MIR generated entry pointer size mismatch");
            memcpy(&entry, &generated, sizeof(entry));
            raw = entry(context, arguments[0].as.f64);
        } else {
            return TURBOWASM_UNSUPPORTED;
        }

        if (context->call_status != TURBOWASM_OK) {
            *trap = context->call_trap;
            return context->call_status;
        }

        results[0].kind = TURBOWASM_VALUE_F64;
        results[0].as.f64 = raw;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    *result_count = 1u;
    return TURBOWASM_OK;
}

static void turbowasm_mir_destroy_function(
    void *context,
    turbowasm_compiled_function *compiled) {
    (void)context;
    if (compiled == NULL)
        return;
    free(compiled->impl);
    compiled->impl = NULL;
}


static bool turbowasm_mir_register_call_externals(
    turbowasm_mir_backend_context *backend) {
    void *address = NULL;

    if (backend == NULL || backend->mir == NULL)
        return false;

    {
        int64_t (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t) = turbowasm_mir_call_i64_0;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_i64_0", address);
    }
    {
        int64_t (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t,
            int64_t) = turbowasm_mir_call_i64_1;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_i64_1", address);
    }
    {
        int64_t (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t,
            int64_t,
            int64_t) = turbowasm_mir_call_i64_2;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_i64_2", address);
    }
    {
        float (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t) = turbowasm_mir_call_f32_0;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_f32_0", address);
    }
    {
        float (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t,
            float) = turbowasm_mir_call_f32_1;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_f32_1", address);
    }
    {
        double (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t) = turbowasm_mir_call_f64_0;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_f64_0", address);
    }
    {
        double (*fn)(
            turbowasm_jit_invocation_context *,
            int64_t,
            double) = turbowasm_mir_call_f64_1;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_f64_1", address);
    }
    {
        int64_t (*fn)(
            turbowasm_jit_invocation_context *) =
                turbowasm_mir_call_status;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_call_status", address);
    }
    {
        int64_t (*fn)(
            turbowasm_jit_invocation_context *) =
                turbowasm_mir_checkpoint;
        _Static_assert(sizeof(fn) == sizeof(address),
                       "MIR external pointer size mismatch");
        memcpy(&address, &fn, sizeof(address));
        MIR_load_external(
            backend->mir, "tw_jit_checkpoint", address);
    }

    return true;
}

static void turbowasm_mir_destroy_backend(void *context) {
    turbowasm_mir_backend_context *backend =
        (turbowasm_mir_backend_context *)context;

    if (backend == NULL)
        return;

    if (backend->mir != NULL) {
        MIR_gen_finish(backend->mir);
        MIR_finish(backend->mir);
    }

    free(backend);
}

turbowasm_status turbowasm_mir_backend_create(
    turbowasm_jit_backend *out_backend) {
    turbowasm_mir_backend_context *context;

    if (out_backend == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_backend, 0, sizeof(*out_backend));

    context = (turbowasm_mir_backend_context *)calloc(
        1u, sizeof(*context));
    if (context == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    context->mir = MIR_init();
    if (context->mir == NULL) {
        free(context);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    MIR_gen_init(context->mir);
    MIR_gen_set_optimize_level(context->mir, 0u);

    if (!turbowasm_mir_register_call_externals(context)) {
        MIR_gen_finish(context->mir);
        MIR_finish(context->mir);
        free(context);
        return TURBOWASM_UNSUPPORTED;
    }

    out_backend->context = context;
    out_backend->supports_execution_control = true;
    out_backend->is_function_eligible =
        turbowasm_mir_is_function_eligible;
    out_backend->compile_function =
        turbowasm_mir_compile_function;
    out_backend->invoke =
        turbowasm_mir_invoke_compiled;
    out_backend->destroy_function =
        turbowasm_mir_destroy_function;
    out_backend->destroy_backend =
        turbowasm_mir_destroy_backend;

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_mir_backend_smoke_constant(
    turbowasm_jit_backend *backend,
    int64_t expected) {
    turbowasm_mir_backend_context *context;
    MIR_module_t module;
    MIR_item_t function;
    void *generated;
    int64_t (*entry)(void);
    int64_t result;
    char program[256];

    if (backend == NULL || backend->context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    context = (turbowasm_mir_backend_context *)backend->context;

    if (snprintf(
            program, sizeof(program),
            "m_smoke: module\n"
            "export smoke\n"
            "smoke: func i64\n"
            "local i64:r\n"
            "mov r, %lld\n"
            "ret r\n"
            "endfunc\n"
            "endmodule\n",
            (long long)expected) <= 0)
        return TURBOWASM_INVALID_ARGUMENT;

    MIR_scan_string(context->mir, program);

    module = DLIST_TAIL(
        MIR_module_t, *MIR_get_module_list(context->mir));
    if (module == NULL)
        return TURBOWASM_UNSUPPORTED;

    function = DLIST_TAIL(MIR_item_t, module->items);
    if (function == NULL ||
        strcmp(MIR_item_name(context->mir, function), "smoke") != 0)
        return TURBOWASM_UNSUPPORTED;

    MIR_load_module(context->mir, module);
    MIR_link(context->mir, MIR_set_gen_interface, NULL);

    generated = MIR_gen(context->mir, function);
    if (generated == NULL)
        return TURBOWASM_UNSUPPORTED;

    _Static_assert(
        sizeof(entry) == sizeof(generated),
        "MIR generated entry pointer size mismatch");
    memcpy(&entry, &generated, sizeof(entry));
    result = entry();

    return result == expected
        ? TURBOWASM_OK
        : TURBOWASM_UNSUPPORTED;
}
