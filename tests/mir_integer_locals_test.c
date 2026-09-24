#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"
#include "../src/module_internal.h"
#include "../src/validation_context.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void compare_with_args(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value_kind expected_kind) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function;
    turbowasm_compiled_function compiled = {0};
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;
    turbowasm_jit_invocation_context jit_context = {0};
    turbowasm_jit_execution_control execution = {0};

    jit_context.instance =
        (turbowasm_instance_impl *)instance->impl;
    jit_context.execution = NULL;
    jit_context.depth = 0u;

    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, function_index);
    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        function_index,
        function));

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &interpreted, 1u,
               &interpreted_count,
               &interpreted_trap) == TURBOWASM_OK);
    assert(interpreted_count == 1u);
    assert(interpreted.kind == expected_kind);
    assert(interpreted_trap == TURBOWASM_TRAP_NONE);

    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               function_index,
               function,
               &compiled) == TURBOWASM_OK);
    assert(compiled.impl != NULL);

    assert(backend->invoke(
               &compiled,
               &jit_context,
               arguments, argument_count,
               &generated, 1u,
               &generated_count,
               &generated_trap) == TURBOWASM_OK);
    assert(generated_count == 1u);
    assert(generated.kind == interpreted.kind);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    if (expected_kind == TURBOWASM_VALUE_I32)
        assert(generated.as.i32 == interpreted.as.i32);
    else
        assert(generated.as.i64 == interpreted.as.i64);

    generated_count = 7u;
    generated_trap = TURBOWASM_TRAP_UNREACHABLE;
    execution.fuel_limited = true;
    execution.fuel_remaining = 0u;
    jit_context.execution = &execution;
    assert(backend->invoke(
               &compiled,
               &jit_context,
               arguments, argument_count,
               &generated, 1u,
               &generated_count,
               &generated_trap) ==
           TURBOWASM_FUEL_EXHAUSTED);
    assert(generated_count == 0u);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    backend->destroy_function(backend->context, &compiled);
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32,i32)->i32, (i64)->i64, (i32,i32,i32)->i32 */
        0x01, 0x13,
        0x03,
        0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
        0x60, 0x01, 0x7e, 0x01, 0x7e,
        0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,

        0x03, 0x04,
        0x03, 0x00, 0x01, 0x02,

        0x0a, 0x27,
        0x03,

        /* f0: local2 = p0 + p1; local2 * 2 */
        0x10,
        0x01, 0x01, 0x7f,
        0x20, 0x00,
        0x20, 0x01,
        0x6a,
        0x21, 0x02,
        0x20, 0x02,
        0x41, 0x02,
        0x6c,
        0x0b,

        /* f1: local.tee stores p0 in local1, then local1 + 3 */
        0x0f,
        0x01, 0x01, 0x7e,
        0x20, 0x00,
        0x22, 0x01,
        0x21, 0x00,
        0x20, 0x01,
        0x42, 0x03,
        0x7c,
        0x0b,

        /* f2: valid, but arity 3 is outside #77 ABI bridge */
        0x04,
        0x00,
        0x20, 0x00,
        0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_value i32_args[2] = {0};
    turbowasm_value i64_arg = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *unsupported;
    turbowasm_compiled_function compiled = {0};

    i32_args[0].kind = TURBOWASM_VALUE_I32;
    i32_args[0].as.i32 = 10;
    i32_args[1].kind = TURBOWASM_VALUE_I32;
    i32_args[1].as.i32 = 5;

    i64_arg.kind = TURBOWASM_VALUE_I64;
    i64_arg.as.i64 = INT64_C(40);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);

    compare_with_args(
        &backend, &module, &instance,
        0u, i32_args, 2u,
        TURBOWASM_VALUE_I32);
    compare_with_args(
        &backend, &module, &instance,
        1u, &i64_arg, 1u,
        TURBOWASM_VALUE_I64);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    unsupported = turbowasm_validation_context_function(
        &impl->validation, 2u);
    assert(unsupported != NULL);
    assert(!backend.is_function_eligible(
        backend.context,
        &impl->validation,
        2u,
        unsupported));
    assert(backend.compile_function(
               backend.context,
               &impl->validation,
               2u,
               unsupported,
               &compiled) == TURBOWASM_UNSUPPORTED);
    assert(compiled.impl == NULL);

    /* Type mismatch in the native ABI bridge is explicit. */
    unsupported = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(unsupported != NULL);
    assert(backend.compile_function(
               backend.context,
               &impl->validation,
               0u,
               unsupported,
               &compiled) == TURBOWASM_OK);
    i32_args[0].kind = TURBOWASM_VALUE_I64;
    i32_args[0].as.i64 = 10;
    {
        turbowasm_value result = {0};
        size_t result_count = 0u;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        turbowasm_jit_invocation_context jit_context = {0};

        jit_context.instance =
            (turbowasm_instance_impl *)instance.impl;
        jit_context.execution = NULL;
        jit_context.depth = 0u;
        assert(backend.invoke(
                   &compiled,
                   &jit_context,
                   i32_args, 2u,
                   &result, 1u,
                   &result_count,
                   &trap) == TURBOWASM_TYPE_MISMATCH);
        assert(result_count == 0u);
        assert(trap == TURBOWASM_TRAP_NONE);
    }
    backend.destroy_function(backend.context, &compiled);

    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
