#include "mir_backend.h"

#include <mir-gen.h>
#include <mir.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_mir_backend_context {
    MIR_context_t mir;
} turbowasm_mir_backend_context;

static bool turbowasm_mir_never_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    (void)context;
    (void)validation;
    (void)function_index;
    (void)function;
    return false;
}

static turbowasm_status turbowasm_mir_compile_unsupported(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    (void)context;
    (void)validation;
    (void)function_index;
    (void)function;
    if (out != NULL)
        out->impl = NULL;
    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status turbowasm_mir_invoke_unsupported(
    const turbowasm_compiled_function *compiled,
    struct turbowasm_instance_impl *instance,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    turbowasm_jit_execution_control *execution) {
    (void)compiled;
    (void)instance;
    (void)arguments;
    (void)argument_count;
    (void)results;
    (void)result_capacity;
    (void)execution;
    if (result_count != NULL)
        *result_count = 0u;
    if (trap != NULL)
        *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_UNSUPPORTED;
}

static void turbowasm_mir_destroy_function(
    void *context,
    turbowasm_compiled_function *compiled) {
    (void)context;
    if (compiled != NULL)
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
        turbowasm_mir_never_eligible;
    out_backend->compile_function =
        turbowasm_mir_compile_unsupported;
    out_backend->invoke =
        turbowasm_mir_invoke_unsupported;
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
            "mov r0, %lld\n"
            "ret r0\n"
            "endfunc\n"
            "endmodule\n",
            (long long)expected) <= 0)
        return TURBOWASM_INVALID_ARGUMENT;

    MIR_scan_string(context->mir, program);

    module = DLIST_TAIL(
        MIR_module_t, *MIR_get_module_list(context->mir));
    if (module == NULL)
        return TURBOWASM_UNSUPPORTED;

    function = MIR_get_global_item(context->mir, "smoke");
    if (function == NULL)
        return TURBOWASM_UNSUPPORTED;

    MIR_load_module(context->mir, module);
    MIR_link(context->mir, MIR_set_gen_interface, NULL);

    generated = MIR_gen(context->mir, function);
    if (generated == NULL)
        return TURBOWASM_UNSUPPORTED;

    entry = (int64_t (*)(void))generated;
    result = entry();

    return result == expected
        ? TURBOWASM_OK
        : TURBOWASM_UNSUPPORTED;
}
