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

static void compile_and_compare(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_status expected_status,
    turbowasm_trap expected_trap) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function;
    turbowasm_compiled_function compiled = {0};
    turbowasm_jit_invocation_context context = {0};
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;
    turbowasm_status interpreted_status;
    turbowasm_status generated_status;

    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, function_index);
    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        function_index,
        function));

    interpreted_status = turbowasm_instance_invoke(
        instance,
        function_index,
        arguments,
        argument_count,
        &interpreted,
        1u,
        &interpreted_count,
        &interpreted_trap);
    assert(interpreted_status == expected_status);
    assert(interpreted_trap == expected_trap);

    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               function_index,
               function,
               &compiled) == TURBOWASM_OK);
    assert(compiled.impl != NULL);

    context.instance =
        (turbowasm_instance_impl *)instance->impl;
    context.execution = NULL;
    context.depth = 0u;
    context.call_status = TURBOWASM_OK;
    context.call_trap = TURBOWASM_TRAP_NONE;

    generated_status = backend->invoke(
        &compiled,
        &context,
        arguments,
        argument_count,
        &generated,
        1u,
        &generated_count,
        &generated_trap);
    assert(generated_status == interpreted_status);
    assert(generated_trap == interpreted_trap);

    if (expected_status == TURBOWASM_OK) {
        assert(interpreted_count == 1u);
        assert(generated_count == 1u);
        assert(generated.kind == interpreted.kind);

        switch (interpreted.kind) {
            case TURBOWASM_VALUE_I32:
                assert(generated.as.i32 == interpreted.as.i32);
                break;
            case TURBOWASM_VALUE_F32:
                assert(generated.as.f32 == interpreted.as.f32);
                break;
            default:
                assert(0 && "unexpected test result kind");
        }
    } else {
        assert(generated_count == 0u);
    }

    backend->destroy_function(backend->context, &compiled);
    assert(compiled.impl == NULL);
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32)->i32, (f32)->f32, ()->i32 */
        0x01, 0x0f,
        0x03,
        0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x60, 0x01, 0x7d, 0x01, 0x7d,
        0x60, 0x00, 0x01, 0x7f,

        /* callee/caller pairs */
        0x03, 0x07,
        0x06, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02,

        0x0a, 0x2f,
        0x06,

        /* f0: interpreted-only integer callee: p0 / 2 */
        0x07,
        0x00,
        0x20, 0x00,
        0x41, 0x02,
        0x6d,
        0x0b,

        /* f1: compiled caller: call f0, then +1 */
        0x09,
        0x00,
        0x20, 0x00,
        0x10, 0x00,
        0x41, 0x01,
        0x6a,
        0x0b,

        /* f2: interpreted-only f32 callee due nop */
        0x05,
        0x00,
        0x20, 0x00,
        0x01,
        0x0b,

        /* f3: compiled caller: call f2, then *1.5 */
        0x0c,
        0x00,
        0x20, 0x00,
        0x10, 0x02,
        0x43, 0x00, 0x00, 0xc0, 0x3f,
        0x94,
        0x0b,

        /* f4: interpreted-only trapping callee */
        0x03,
        0x00,
        0x00,
        0x0b,

        /* f5: compiled caller that propagates f4 trap */
        0x04,
        0x00,
        0x10, 0x04,
        0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *callee;
    turbowasm_value i32_arg = {0};
    turbowasm_value f32_arg = {0};

    i32_arg.kind = TURBOWASM_VALUE_I32;
    i32_arg.as.i32 = 20;
    f32_arg.kind = TURBOWASM_VALUE_F32;
    f32_arg.as.f32 = 2.0f;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);

    /* Prove the callees themselves remain interpreter-only. */
    callee = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(callee != NULL);
    assert(!backend.is_function_eligible(
        backend.context, &impl->validation, 0u, callee));

    callee = turbowasm_validation_context_function(
        &impl->validation, 2u);
    assert(callee != NULL);
    assert(!backend.is_function_eligible(
        backend.context, &impl->validation, 2u, callee));

    callee = turbowasm_validation_context_function(
        &impl->validation, 4u);
    assert(callee != NULL);
    assert(!backend.is_function_eligible(
        backend.context, &impl->validation, 4u, callee));

    compile_and_compare(
        &backend, &module, &instance,
        1u, &i32_arg, 1u,
        TURBOWASM_OK, TURBOWASM_TRAP_NONE);

    compile_and_compare(
        &backend, &module, &instance,
        3u, &f32_arg, 1u,
        TURBOWASM_OK, TURBOWASM_TRAP_NONE);

    compile_and_compare(
        &backend, &module, &instance,
        5u, NULL, 0u,
        TURBOWASM_TRAPPED,
        TURBOWASM_TRAP_UNREACHABLE);

    backend.destroy_backend(backend.context);
    backend.context = NULL;
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
