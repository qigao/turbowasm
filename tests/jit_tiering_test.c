#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct fake_backend_state {
    uint32_t eligibility_checks[3];
    uint32_t compile_attempts[3];
    uint32_t invoke_count;
    uint32_t destroy_function_count;
    uint32_t destroy_backend_count;
} fake_backend_state;

typedef struct fake_compiled {
    int32_t value;
} fake_compiled;

static bool fake_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    fake_backend_state *state = (fake_backend_state *)context;
    (void)validation;
    (void)function;
    assert(state != NULL);
    assert(function_index < 3u);
    ++state->eligibility_checks[function_index];
    return function_index != 2u;
}

static turbowasm_status fake_compile(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    fake_backend_state *state = (fake_backend_state *)context;
    fake_compiled *compiled;
    (void)validation;
    (void)function;

    assert(state != NULL);
    assert(out != NULL);
    assert(function_index < 3u);
    ++state->compile_attempts[function_index];
    out->impl = NULL;

    if (function_index == 1u)
        return TURBOWASM_UNSUPPORTED;

    compiled = (fake_compiled *)calloc(1u, sizeof(*compiled));
    if (compiled == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    compiled->value = 42;
    out->impl = compiled;
    return TURBOWASM_OK;
}

static turbowasm_status fake_invoke(
    const turbowasm_compiled_function *compiled,
    turbowasm_jit_invocation_context *context,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    fake_backend_state *state;
    const fake_compiled *function;

    assert(compiled != NULL);
    assert(compiled->impl != NULL);
    assert(context != NULL);
    assert(context->instance != NULL);
    assert(context->execution == NULL);
    assert(argument_count == 0u);
    assert(arguments == NULL);
    assert(results != NULL);
    assert(result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    state = (fake_backend_state *)
        context->instance->jit_backend.context;
    assert(state != NULL);
    ++state->invoke_count;

    function = (const fake_compiled *)compiled->impl;
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = function->value;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static void fake_destroy_function(
    void *context,
    turbowasm_compiled_function *compiled) {
    fake_backend_state *state = (fake_backend_state *)context;
    assert(state != NULL);
    if (compiled != NULL && compiled->impl != NULL) {
        ++state->destroy_function_count;
        free(compiled->impl);
        compiled->impl = NULL;
    }
}

static void fake_destroy_backend(void *context) {
    fake_backend_state *state = (fake_backend_state *)context;
    assert(state != NULL);
    ++state->destroy_backend_count;
}

static turbowasm_jit_backend make_backend(
    fake_backend_state *state) {
    turbowasm_jit_backend backend = {0};
    backend.context = state;
    backend.is_function_eligible = fake_eligible;
    backend.compile_function = fake_compile;
    backend.invoke = fake_invoke;
    backend.destroy_function = fake_destroy_function;
    backend.destroy_backend = fake_destroy_backend;
    return backend;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(trap == TURBOWASM_TRAP_NONE);
    return result.as.i32;
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x04,
        0x03, 0x00, 0x00, 0x00,
        0x0a, 0x10,
        0x03,
        0x04, 0x00, 0x41, 0x2a, 0x0b,
        0x04, 0x00, 0x41, 0x07, 0x0b,
        0x04, 0x00, 0x41, 0x09, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    fake_backend_state state = {0};
    turbowasm_jit_backend backend;
    turbowasm_execution_options options = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);

    backend = make_backend(&state);
    assert(turbowasm_jit_instance_attach_backend(
               impl, &backend, 2u) == TURBOWASM_OK);
    assert(backend.context == NULL);

    /* First call is cold/interpreted. */
    assert(invoke_i32(&instance, 0u) == 42);
    assert(state.compile_attempts[0] == 0u);
    assert(state.invoke_count == 0u);

    /* Threshold call compiles once and immediately uses compiled code. */
    assert(invoke_i32(&instance, 0u) == 42);
    assert(state.eligibility_checks[0] == 1u);
    assert(state.compile_attempts[0] == 1u);
    assert(state.invoke_count == 1u);

    /* Later calls reuse the compiled handle without recompiling. */
    assert(invoke_i32(&instance, 0u) == 42);
    assert(state.compile_attempts[0] == 1u);
    assert(state.invoke_count == 2u);

    /* Eligible compile failure becomes interpreter-only exactly once. */
    assert(invoke_i32(&instance, 1u) == 7);
    assert(invoke_i32(&instance, 1u) == 7);
    assert(invoke_i32(&instance, 1u) == 7);
    assert(state.eligibility_checks[1] == 1u);
    assert(state.compile_attempts[1] == 1u);

    /* Ineligible function is checked once and stays interpreted. */
    assert(invoke_i32(&instance, 2u) == 9);
    assert(invoke_i32(&instance, 2u) == 9);
    assert(invoke_i32(&instance, 2u) == 9);
    assert(state.eligibility_checks[2] == 1u);
    assert(state.compile_attempts[2] == 0u);

    /* Execution-control invocations never enter compiled code. */
    options.has_fuel_limit = true;
    options.fuel = 2u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap,
               &options) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);
    assert(state.invoke_count == 2u);

    /* Direct-call helper shares the same compiled dispatch cache. */
    {
        turbowasm_jit_invocation_context context = {
            impl, NULL, 0u,
            TURBOWASM_OK, TURBOWASM_TRAP_NONE
        };
        result_count = 0u;
        trap = TURBOWASM_TRAP_NONE;
        assert(turbowasm_jit_direct_call(
                   &context, 0u,
                   NULL, 0u,
                   &result, 1u,
                   &result_count,
                   &trap) == TURBOWASM_OK);
        assert(result.as.i32 == 42);
        assert(state.invoke_count == 3u);
    }

    turbowasm_instance_destroy(&instance);
    assert(state.destroy_function_count == 1u);
    assert(state.destroy_backend_count == 1u);
    turbowasm_module_destroy(&module);
    return 0;
}
