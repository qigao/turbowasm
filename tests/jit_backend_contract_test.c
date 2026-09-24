#include "../src/jit_backend.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static bool never_eligible(
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

static turbowasm_status never_compile(
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
        memset(out, 0, sizeof(*out));
    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status never_invoke(
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

static void destroy_noop(
    void *context,
    turbowasm_compiled_function *compiled) {
    (void)context;
    if (compiled != NULL)
        compiled->impl = NULL;
}

static void destroy_backend_noop(void *context) {
    (void)context;
}

int main(void) {
    turbowasm_jit_backend backend = {
        NULL,
        never_eligible,
        never_compile,
        never_invoke,
        destroy_noop,
        destroy_backend_noop
    };
    turbowasm_compiled_function compiled = {0};
    size_t result_count = 1u;
    turbowasm_trap trap = TURBOWASM_TRAP_UNREACHABLE;

    assert(!backend.is_function_eligible(
        backend.context, NULL, 0u, NULL));
    assert(backend.compile_function(
               backend.context, NULL, 0u, NULL, &compiled) ==
           TURBOWASM_UNSUPPORTED);
    assert(compiled.impl == NULL);
    assert(backend.invoke(
               &compiled, NULL,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap,
               NULL) == TURBOWASM_UNSUPPORTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    return 0;
}
