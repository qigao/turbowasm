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

enum {
    TURBOWASM_MIR_WASM_I32 = 0x7f,
    TURBOWASM_MIR_WASM_I64 = 0x7e
};

typedef struct turbowasm_mir_backend_context {
    MIR_context_t mir;
    uint64_t next_function_id;
} turbowasm_mir_backend_context;

typedef struct turbowasm_mir_analysis {
    uint8_t result_type;
    uint32_t constant_count;
} turbowasm_mir_analysis;

typedef struct turbowasm_mir_compiled_impl {
    void *entry;
    uint8_t result_type;
} turbowasm_mir_compiled_impl;

typedef struct turbowasm_mir_stack_entry {
    uint32_t reg;
    uint8_t type;
} turbowasm_mir_stack_entry;

typedef struct turbowasm_mir_text {
    char *data;
    size_t size;
    size_t capacity;
} turbowasm_mir_text;

static bool turbowasm_mir_text_appendf(
    turbowasm_mir_text *text,
    const char *format,
    ...) {
    va_list ap;
    va_list copy;
    int needed;
    size_t required;
    char *grown;

    if (text == NULL || format == NULL)
        return false;

    va_start(ap, format);
    va_copy(copy, ap);
    needed = vsnprintf(NULL, 0u, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return false;
    }

    required = text->size + (size_t)needed + 1u;
    if (required > text->capacity) {
        size_t next = text->capacity == 0u ? 512u : text->capacity;
        while (next < required) {
            if (next > SIZE_MAX / 2u) {
                va_end(ap);
                return false;
            }
            next *= 2u;
        }
        grown = (char *)realloc(text->data, next);
        if (grown == NULL) {
            va_end(ap);
            return false;
        }
        text->data = grown;
        text->capacity = next;
    }

    (void)vsnprintf(
        text->data + text->size,
        text->capacity - text->size,
        format,
        ap);
    va_end(ap);
    text->size += (size_t)needed;
    return true;
}

static bool turbowasm_mir_integer_binary_type(
    uint8_t opcode,
    uint8_t *out_type) {
    if (out_type == NULL)
        return false;

    switch (opcode) {
        case 0x6au: /* i32.add */
        case 0x6bu: /* i32.sub */
        case 0x6cu: /* i32.mul */
            *out_type = TURBOWASM_MIR_WASM_I32;
            return true;
        case 0x7cu: /* i64.add */
        case 0x7du: /* i64.sub */
        case 0x7eu: /* i64.mul */
            *out_type = TURBOWASM_MIR_WASM_I64;
            return true;
        default:
            return false;
    }
}

static const char *turbowasm_mir_integer_binary_name(uint8_t opcode) {
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

static bool turbowasm_mir_analyze_pure_integer(
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    turbowasm_mir_analysis *out) {
    const turbowasm_validation_func_type *type;
    turbowasm_reader reader;
    uint8_t *stack = NULL;
    uint32_t depth = 0u;
    uint32_t constants = 0u;
    bool ended = false;
    bool ok = false;

    if (validation == NULL || function == NULL || out == NULL ||
        function->imported)
        return false;

    type = turbowasm_validation_context_function_type(
        validation, function_index);
    if (type == NULL || !type->defined ||
        type->param_count != 0u ||
        type->result_count != 1u ||
        function->local_count != 0u ||
        function->code == NULL ||
        function->code_size == 0u)
        return false;

    if (type->results[0] != TURBOWASM_MIR_WASM_I32 &&
        type->results[0] != TURBOWASM_MIR_WASM_I64)
        return false;

    stack = (uint8_t *)calloc(
        (size_t)function->code_size + 1u, sizeof(*stack));
    if (stack == NULL)
        return false;

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;
        uint8_t binary_type;

        if (!turbowasm_reader_u8(&reader, &opcode))
            goto done;

        if (opcode == 0x41u) {
            int32_t ignored;
            if (!turbowasm_reader_sleb32(&reader, &ignored))
                goto done;
            stack[depth++] = TURBOWASM_MIR_WASM_I32;
            ++constants;
            continue;
        }

        if (opcode == 0x42u) {
            int64_t ignored;
            if (!turbowasm_reader_sleb64(&reader, &ignored))
                goto done;
            stack[depth++] = TURBOWASM_MIR_WASM_I64;
            ++constants;
            continue;
        }

        if (turbowasm_mir_integer_binary_type(
                opcode, &binary_type)) {
            if (depth < 2u ||
                stack[depth - 1u] != binary_type ||
                stack[depth - 2u] != binary_type)
                goto done;
            --depth;
            stack[depth - 1u] = binary_type;
            continue;
        }

        if (opcode == 0x0bu) {
            if (turbowasm_reader_remaining(&reader) != 0u ||
                depth != 1u ||
                stack[0] != type->results[0])
                goto done;
            ended = true;
            break;
        }

        goto done;
    }

    if (!ended)
        goto done;

    out->result_type = type->results[0];
    out->constant_count = constants;
    ok = true;

done:
    free(stack);
    return ok;
}

static bool turbowasm_mir_is_function_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    turbowasm_mir_analysis analysis;

    (void)context;
    memset(&analysis, 0, sizeof(analysis));
    return turbowasm_mir_analyze_pure_integer(
        validation, function_index, function, &analysis);
}

static turbowasm_status turbowasm_mir_lower_pure_integer(
    turbowasm_mir_backend_context *context,
    const turbowasm_validation_context *validation,
    uint32_t function_index,
    const turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    turbowasm_mir_analysis analysis = {0};
    turbowasm_mir_text text = {0};
    turbowasm_mir_stack_entry *stack = NULL;
    turbowasm_reader reader;
    uint32_t depth = 0u;
    uint32_t next_reg = 0u;
    uint32_t index;
    uint64_t id;
    MIR_module_t module;
    MIR_item_t item;
    void *generated;
    turbowasm_mir_compiled_impl *compiled = NULL;
    turbowasm_status status = TURBOWASM_UNSUPPORTED;

    if (context == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    out->impl = NULL;

    if (!turbowasm_mir_analyze_pure_integer(
            validation, function_index, function, &analysis))
        return TURBOWASM_UNSUPPORTED;

    stack = (turbowasm_mir_stack_entry *)calloc(
        (size_t)function->code_size + 1u, sizeof(*stack));
    if (stack == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    id = ++context->next_function_id;

    if (!turbowasm_mir_text_appendf(
            &text,
            "tw_m_%llu: module\n"
            "export tw_f_%llu\n"
            "tw_f_%llu: func %s\n",
            (unsigned long long)id,
            (unsigned long long)id,
            (unsigned long long)id,
            analysis.result_type == TURBOWASM_MIR_WASM_I32
                ? "i32" : "i64")) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    for (index = 0u; index < analysis.constant_count; ++index) {
        if (!turbowasm_mir_text_appendf(
                &text, "local i64:r%u\n", index)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto done;
        }
    }

    turbowasm_reader_init(
        &reader, function->code, function->code_size);

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&reader, &opcode))
            goto done;

        if (opcode == 0x41u) {
            int32_t value;
            if (!turbowasm_reader_sleb32(&reader, &value))
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov r%u, %lld\n",
                    next_reg, (long long)value)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto done;
            }
            stack[depth].reg = next_reg++;
            stack[depth].type = TURBOWASM_MIR_WASM_I32;
            ++depth;
            continue;
        }

        if (opcode == 0x42u) {
            int64_t value;
            if (!turbowasm_reader_sleb64(&reader, &value))
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text, "mov r%u, %lld\n",
                    next_reg, (long long)value)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto done;
            }
            stack[depth].reg = next_reg++;
            stack[depth].type = TURBOWASM_MIR_WASM_I64;
            ++depth;
            continue;
        }

        if (opcode == 0x0bu) {
            if (depth != 1u)
                goto done;
            if (!turbowasm_mir_text_appendf(
                    &text,
                    "ret r%u\n"
                    "endfunc\n"
                    "endmodule\n",
                    stack[0].reg)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto done;
            }
            break;
        }

        {
            uint8_t binary_type;
            const char *name =
                turbowasm_mir_integer_binary_name(opcode);
            turbowasm_mir_stack_entry left;
            turbowasm_mir_stack_entry right;

            if (name == NULL ||
                !turbowasm_mir_integer_binary_type(
                    opcode, &binary_type) ||
                depth < 2u)
                goto done;

            right = stack[--depth];
            left = stack[--depth];
            if (left.type != binary_type ||
                right.type != binary_type)
                goto done;

            if (!turbowasm_mir_text_appendf(
                    &text,
                    "%s r%u, r%u, r%u\n",
                    name,
                    left.reg,
                    left.reg,
                    right.reg)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto done;
            }

            stack[depth] = left;
            ++depth;
        }
    }

    if (text.data == NULL || depth != 1u)
        goto done;

    MIR_scan_string(context->mir, text.data);

    module = DLIST_TAIL(
        MIR_module_t, *MIR_get_module_list(context->mir));
    if (module == NULL)
        goto done;

    item = DLIST_TAIL(MIR_item_t, module->items);
    if (item == NULL)
        goto done;

    MIR_load_module(context->mir, module);
    MIR_link(context->mir, MIR_set_gen_interface, NULL);

    generated = MIR_gen(context->mir, item);
    if (generated == NULL)
        goto done;

    compiled = (turbowasm_mir_compiled_impl *)calloc(
        1u, sizeof(*compiled));
    if (compiled == NULL) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    compiled->entry = generated;
    compiled->result_type = analysis.result_type;
    out->impl = compiled;
    compiled = NULL;
    status = TURBOWASM_OK;

done:
    free(compiled);
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
    return turbowasm_mir_lower_pure_integer(
        (turbowasm_mir_backend_context *)context,
        validation,
        function_index,
        function,
        out);
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
    const turbowasm_mir_compiled_impl *impl;

    (void)instance;
    (void)arguments;

    if (compiled == NULL || compiled->impl == NULL ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    if (execution != NULL)
        return TURBOWASM_UNSUPPORTED;

    if (argument_count != 0u ||
        results == NULL ||
        result_capacity < 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (const turbowasm_mir_compiled_impl *)compiled->impl;

    if (impl->result_type == TURBOWASM_MIR_WASM_I32) {
        int32_t (*entry)(void) = NULL;
        int32_t value;

        _Static_assert(
            sizeof(entry) == sizeof(impl->entry),
            "MIR i32 entry pointer size mismatch");
        memcpy(&entry, &impl->entry, sizeof(entry));
        value = entry();

        results[0].kind = TURBOWASM_VALUE_I32;
        results[0].as.i32 = value;
    } else if (impl->result_type == TURBOWASM_MIR_WASM_I64) {
        int64_t (*entry)(void) = NULL;
        int64_t value;

        _Static_assert(
            sizeof(entry) == sizeof(impl->entry),
            "MIR i64 entry pointer size mismatch");
        memcpy(&entry, &impl->entry, sizeof(entry));
        value = entry();

        results[0].kind = TURBOWASM_VALUE_I64;
        results[0].as.i64 = value;
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
    MIR_gen_set_optimize_level(context->mir, 1u);

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
