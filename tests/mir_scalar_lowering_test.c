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
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void compare_function(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance,
    uint32_t function_index,
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
               NULL, 0u,
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
               NULL, 0u,
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

    generated_count = 99u;
    generated_trap = TURBOWASM_TRAP_UNREACHABLE;
    jit_context.execution = &execution;
    assert(backend->invoke(
               &compiled,
               &jit_context,
               NULL, 0u,
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

        /* type 0: () -> i32, type 1: () -> i64 */
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7e,

        /* three defined functions: i32, i64, unsupported i32 */
        0x03, 0x04,
        0x03, 0x00, 0x01, 0x00,

        0x0a, 0x1e,
        0x03,

        /* f0: INT32_MAX + 1 -> INT32_MIN */
        0x0b,
        0x00,
        0x41, 0xff, 0xff, 0xff, 0xff, 0x07,
        0x41, 0x01,
        0x6a,
        0x0b,

        /* f1: (7 * 6) + 2 -> 44 */
        0x0a,
        0x00,
        0x42, 0x07,
        0x42, 0x06,
        0x7e,
        0x42, 0x02,
        0x7c,
        0x0b,

        /* f2: validated, but nop is outside #73 eligibility */
        0x05,
        0x00,
        0x41, 0x01,
        0x01,
        0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *unsupported;
    turbowasm_compiled_function compiled = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);

    compare_function(
        &backend, &module, &instance,
        0u, TURBOWASM_VALUE_I32);
    compare_function(
        &backend, &module, &instance,
        1u, TURBOWASM_VALUE_I64);

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

    backend.destroy_backend(backend.context);
    backend.context = NULL;
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
