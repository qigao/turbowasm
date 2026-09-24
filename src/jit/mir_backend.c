#include "mir_backend.h"

#include "../reader.h"
#include "../validation_context.h"

#include <mir-gen.h>
#include <mir.h>

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

static bool turbowasm_mir_scan_integer_locals(
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
    bool ok = false;

    if (validation == NULL || function == NULL ||
        function->imported || function->code == NULL)
        return false;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL || !type->defined ||
        type->param_count > 2u ||
        function->local_count < type->param_count ||
        type->result_count != 1u ||
        (type->results[0] != 0x7fu &&
         type->results[0] != 0x7eu))
        return false;

    for (index = 0u; index < type->param_count; ++index) {
        if (type->params[index] != 0x7fu &&
            type->params[index] != 0x7eu)
            return false;
    }
    for (index = 0u; index < function->local_count; ++index) {
        if (function->local_types[index] != 0x7fu &&
            function->local_types[index] != 0x7eu)
            return false;
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
                if (!turbowasm_reader_sleb32(&reader, &value))
                    goto done;
                (void)value;
                types[stack_size++] = 0x7fu;
                ++register_count;
                break;
            }
            case 0x42u: { /* i64.const */
                int64_t value;
                if (!turbowasm_reader_sleb64(&reader, &value))
                    goto done;
                (void)value;
                types[stack_size++] = 0x7eu;
                ++register_count;
                break;
            }
            case 0x6au: /* i32.add */
            case 0x6bu: /* i32.sub */
            case 0x6cu: /* i32.mul */
                if (stack_size < 2u ||
                    types[stack_size - 1u] != 0x7fu ||
                    types[stack_size - 2u] != 0x7fu)
                    goto done;
                --stack_size;
                types[stack_size - 1u] = 0x7fu;
                ++register_count;
                break;
            case 0x7cu: /* i64.add */
            case 0x7du: /* i64.sub */
            case 0x7eu: /* i64.mul */
                if (stack_size < 2u ||
                    types[stack_size - 1u] != 0x7eu ||
                    types[stack_size - 2u] != 0x7eu)
                    goto done;
                --stack_size;
                types[stack_size - 1u] = 0x7eu;
                ++register_count;
                break;
            case 0x0bu: /* end */
                if (turbowasm_reader_remaining(&reader) != 0u ||
                    stack_size != 1u ||
                    types[0] != type->results[0])
                    goto done;
                if (out_register_count != NULL)
                    *out_register_count = register_count;
                if (out_result_type != NULL)
                    *out_result_type = type->results[0];
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

static bool turbowasm_mir_is_function_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    (void)context;
    return turbowasm_mir_scan_integer_locals(
        validation, function_index, function, NULL, NULL);
}

static const char *turbowasm_mir_binary_name(uint8_t opcode) {
    switch (opcode) {
        case 0x6au: return "adds";
        case 0x6bu: return "subs";
        case 0x6cu: return "muls";
        case 0x7cu: return "add";
        case 0x7du: return "sub";
        case 0x7eu: return "mul";
        default: return NULL;
    }
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

    if (backend == NULL || backend->mir == NULL ||
        validation == NULL || function == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    out->impl = NULL;
    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_mir_scan_integer_locals(
            validation, function_index, function,
            &register_count, &result_type))
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
            "export %s\n"
            "%s: func i64",
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

        if (!turbowasm_reader_u8(&reader, &opcode))
            goto done;

        if (opcode == 0x20u) {
            uint32_t local_index;
            if (!turbowasm_reader_uleb32(
                    &reader, &local_index) ||
                local_index >= function->local_count)
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov r%u, l%u\n",
                    next_reg, local_index))
                goto oom;
            stack[stack_size].reg = next_reg++;
            stack[stack_size].type =
                function->local_types[local_index];
            ++stack_size;
            continue;
        }

        if (opcode == 0x21u || opcode == 0x22u) {
            uint32_t local_index;
            turbowasm_mir_stack_value value;
            if (!turbowasm_reader_uleb32(
                    &reader, &local_index) ||
                local_index >= function->local_count ||
                stack_size == 0u)
                goto done;
            value = stack[stack_size - 1u];
            if (value.type != function->local_types[local_index])
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov l%u, r%u\n",
                    local_index, value.reg))
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

        if (opcode == 0x6au || opcode == 0x6bu ||
            opcode == 0x6cu || opcode == 0x7cu ||
            opcode == 0x7du || opcode == 0x7eu) {
            const char *name = turbowasm_mir_binary_name(opcode);
            turbowasm_mir_stack_value right;
            turbowasm_mir_stack_value left;
            uint8_t expected =
                opcode >= 0x7cu ? 0x7eu : 0x7fu;

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
            if (!turbowasm_mir_text_appendf(
                    &text,
                    "ret r%u\n"
                    "endfunc\n"
                    "endmodule\n",
                    stack[0].reg))
                goto oom;
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

static turbowasm_status turbowasm_mir_invoke_compiled(
    const turbowasm_compiled_function *compiled,
    struct turbowasm_instance_impl *instance,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    turbowasm_jit_execution_control *execution) {
    const turbowasm_mir_compiled *function;
    void *generated;
    int64_t raw_args[2] = {0, 0};
    int64_t raw;
    uint32_t index;

    (void)instance;

    if (compiled == NULL || compiled->impl == NULL ||
        results == NULL || result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    if (execution != NULL)
        return TURBOWASM_UNSUPPORTED;
    if (result_capacity < 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    function = (const turbowasm_mir_compiled *)compiled->impl;
    if (argument_count != function->param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < function->param_count; ++index) {
        if (!turbowasm_mir_argument_raw(
                &arguments[index],
                function->param_types[index],
                &raw_args[index]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    generated = function->generated;
    if (generated == NULL)
        return TURBOWASM_UNSUPPORTED;

    if (function->param_count == 0u) {
        int64_t (*entry)(void);
        _Static_assert(
            sizeof(entry) == sizeof(generated),
            "MIR generated entry pointer size mismatch");
        memcpy(&entry, &generated, sizeof(entry));
        raw = entry();
    } else if (function->param_count == 1u) {
        int64_t (*entry)(int64_t);
        _Static_assert(
            sizeof(entry) == sizeof(generated),
            "MIR generated entry pointer size mismatch");
        memcpy(&entry, &generated, sizeof(entry));
        raw = entry(raw_args[0]);
    } else if (function->param_count == 2u) {
        int64_t (*entry)(int64_t, int64_t);
        _Static_assert(
            sizeof(entry) == sizeof(generated),
            "MIR generated entry pointer size mismatch");
        memcpy(&entry, &generated, sizeof(entry));
        raw = entry(raw_args[0], raw_args[1]);
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    if (function->result_type == 0x7fu) {
        results[0].kind = TURBOWASM_VALUE_I32;
        results[0].as.i32 = (int32_t)(uint32_t)raw;
    } else if (function->result_type == 0x7eu) {
        results[0].kind = TURBOWASM_VALUE_I64;
        results[0].as.i64 = raw;
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

    out_backend->context = context;
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
