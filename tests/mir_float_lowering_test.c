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

static void compare_float_function(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *argument,
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
               argument, 1u,
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
               argument, 1u,
               &generated, 1u,
               &generated_count,
               &generated_trap) == TURBOWASM_OK);
    assert(generated_count == 1u);
    assert(generated.kind == interpreted.kind);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    if (expected_kind == TURBOWASM_VALUE_F32)
        assert(generated.as.f32 == interpreted.as.f32);
    else
        assert(generated.as.f64 == interpreted.as.f64);

    generated_count = 3u;
    generated_trap = TURBOWASM_TRAP_UNREACHABLE;
    jit_context.execution = &execution;
    assert(backend->invoke(
               &compiled,
               &jit_context,
               argument, 1u,
               &generated, 1u,
               &generated_count,
               &generated_trap) ==
           TURBOWASM_UNSUPPORTED);
    assert(generated_count == 0u);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    backend->destroy_function(backend->context, &compiled);
    assert(compiled.impl == NULL);
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (f32)->f32, (f64)->f64, (f32,f32)->f32 */
        0x01, 0x11,
        0x03,
        0x60, 0x01, 0x7d, 0x01, 0x7d,
        0x60, 0x01, 0x7c, 0x01, 0x7c,
        0x60, 0x02, 0x7d, 0x7d, 0x01, 0x7d,

        0x03, 0x04,
        0x03, 0x00, 0x01, 0x02,

        0x0a, 0x2e,
        0x03,

        /* f0: local1 = p0; local1 * 1.5f */
        0x12,
        0x01, 0x01, 0x7d,
        0x20, 0x00,
        0x22, 0x01,
        0x21, 0x00,
        0x20, 0x01,
        0x43, 0x00, 0x00, 0xc0, 0x3f,
        0x94,
        0x0b,

        /* f1: local1 = p0; local1 + 2.25 */
        0x14,
        0x01, 0x01, 0x7c,
        0x20, 0x00,
        0x21, 0x01,
        0x20, 0x01,
        0x44, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x02, 0x40,
        0xa0,
        0x0b,

        /* f2: valid Wasm but two float params exceed #78 ABI boundary */
        0x04,
        0x00,
        0x20, 0x00,
        0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_value f32_arg = {0};
    turbowasm_value f64_arg = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *unsupported;
    turbowasm_compiled_function compiled = {0};

    f32_arg.kind = TURBOWASM_VALUE_F32;
    f32_arg.as.f32 = 2.0f;
    f64_arg.kind = TURBOWASM_VALUE_F64;
    f64_arg.as.f64 = 10.0;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);

    compare_float_function(
        &backend, &module, &instance,
        0u, &f32_arg, TURBOWASM_VALUE_F32);
    compare_float_function(
        &backend, &module, &instance,
        1u, &f64_arg, TURBOWASM_VALUE_F64);

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

    /* Native floating bridge rejects a mismatched argument kind. */
    unsupported = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(unsupported != NULL);
    assert(backend.compile_function(
               backend.context,
               &impl->validation,
               0u,
               unsupported,
               &compiled) == TURBOWASM_OK);
    {
        turbowasm_value wrong = {0};
        turbowasm_value result = {0};
        size_t result_count = 0u;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        turbowasm_jit_invocation_context jit_context = {0};

        jit_context.instance =
            (turbowasm_instance_impl *)instance.impl;
        jit_context.execution = NULL;
        jit_context.depth = 0u;

        wrong.kind = TURBOWASM_VALUE_F64;
        wrong.as.f64 = 2.0;
        assert(backend.invoke(
                   &compiled,
                   &jit_context,
                   &wrong, 1u,
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
