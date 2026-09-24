#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"
#include "../src/module_internal.h"
#include "../src/validation_context.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct interrupt_counter {
    uint32_t checks;
    uint32_t stop_at;
} interrupt_counter;

static bool stop_after_checks(void *context) {
    interrupt_counter *counter = (interrupt_counter *)context;
    assert(counter != NULL);
    ++counter->checks;
    return counter->checks >= counter->stop_at;
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static void compile_function(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    uint32_t function_index,
    turbowasm_compiled_function *compiled) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function;

    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, function_index);
    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        function_index,
        function));
    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               function_index,
               function,
               compiled) == TURBOWASM_OK);
    assert(compiled->impl != NULL);
}

static int32_t invoke_interpreter_i32(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(trap == TURBOWASM_TRAP_NONE);
    return result.as.i32;
}

static turbowasm_status invoke_compiled(
    turbowasm_jit_backend *backend,
    turbowasm_compiled_function *compiled,
    turbowasm_instance *instance,
    turbowasm_jit_execution_control *execution,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *result,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_jit_invocation_context context = {0};

    context.instance =
        (turbowasm_instance_impl *)instance->impl;
    context.execution = execution;
    context.depth = 0u;

    return backend->invoke(
        compiled,
        &context,
        arguments, argument_count,
        result, 1u,
        result_count,
        trap);
}

static void test_native_loop_exact_safe_points(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x12,
        0x01, 0x10,
        0x00,
        0x03, 0x40,
        0x20, 0x00,
        0x41, 0x01,
        0x6b,
        0x22, 0x00,
        0x0d, 0x00,
        0x0b,
        0x20, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_jit_execution_control execution = {0};
    turbowasm_value argument = i32_value(3);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    interrupt_counter counter = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_interpreter_i32(
               &instance, 0u, &argument, 1u) == 0);

    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    assert(backend.supports_execution_control);
    compile_function(&backend, &module, 0u, &compiled);

    /* 19 executed Wasm instructions for input=3:
     * loop + 3*(get,const,sub,tee,br_if) + loop-end + get + func-end. */
    execution.fuel_limited = true;
    execution.fuel_remaining = 18u;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution,
               &argument, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    execution.fuel_remaining = 19u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution,
               &argument, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 0);
    assert(trap == TURBOWASM_TRAP_NONE);

    execution.fuel_limited = false;
    counter.checks = 0u;
    counter.stop_at = 7u;
    execution.should_interrupt = stop_after_checks;
    execution.interrupt_context = &counter;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution,
               &argument, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_INTERRUPTED);
    assert(counter.checks == 7u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_native_if_else(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x16,
        0x01, 0x14,
        0x01, 0x01, 0x7f,
        0x20, 0x00,
        0x04, 0x40,
        0x41, 0x07,
        0x21, 0x01,
        0x05,
        0x41, 0x09,
        0x21, 0x01,
        0x0b,
        0x20, 0x01,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_value one = i32_value(1);
    turbowasm_value zero = i32_value(0);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, 0u, &compiled);

    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, &one, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 7);
    assert(invoke_interpreter_i32(
               &instance, 0u, &one, 1u) == 7);

    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, &zero, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 9);
    assert(invoke_interpreter_i32(
               &instance, 0u, &zero, 1u) == 9);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_native_block_branch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x0e,
        0x01, 0x0c,
        0x00,
        0x41, 0x05,
        0x02, 0x40,
        0x0c, 0x00,
        0x41, 0x09,
        0x1a,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, 0u, &compiled);

    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, NULL, 0u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 5);
    assert(invoke_interpreter_i32(
               &instance, 0u, NULL, 0u) == 5);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_native_return_inside_if(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x0e,
        0x01, 0x0c,
        0x00,
        0x20, 0x00,
        0x04, 0x40,
        0x41, 0x07,
        0x0f,
        0x0b,
        0x41, 0x09,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_value one = i32_value(1);
    turbowasm_value zero = i32_value(0);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, 0u, &compiled);

    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, &one, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 7);

    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, &zero, 1u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 9);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_structured_direct_call_shares_budget(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        /* f0 callee, f1 structured caller */
        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x14,
        0x02,

        /* f0: const 7; end */
        0x04,
        0x00, 0x41, 0x07, 0x0b,

        /* f1: local i32; block { call f0; local.set 0; };
         * local.get 0; end */
        0x0d,
        0x01, 0x01, 0x7f,
        0x02, 0x40,
        0x10, 0x00,
        0x21, 0x00,
        0x0b,
        0x20, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_jit_execution_control execution = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, 1u, &compiled);

    /* block + call + callee const/end + set + block-end + get +
     * function-end = 8 checkpoints. */
    execution.fuel_limited = true;
    execution.fuel_remaining = 7u;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution, NULL, 0u,
               &result, &result_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);

    execution.fuel_remaining = 8u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution, NULL, 0u,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.as.i32 == 7);
    assert(execution.fuel_remaining == 0u);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_structured_hot_tiering(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x12,
        0x01, 0x10,
        0x00,
        0x03, 0x40,
        0x20, 0x00,
        0x41, 0x01,
        0x6b,
        0x22, 0x00,
        0x0d, 0x00,
        0x0b,
        0x20, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_jit_backend backend = {0};
    turbowasm_value argument = i32_value(3);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);

    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    assert(turbowasm_jit_instance_attach_backend(
               impl, &backend, 1u) == TURBOWASM_OK);
    assert(backend.context == NULL);

    assert(invoke_interpreter_i32(
               &instance, 0u, &argument, 1u) == 0);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].compiled.impl != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_native_loop_exact_safe_points();
    test_native_if_else();
    test_native_block_branch();
    test_native_return_inside_if();
    test_structured_direct_call_shares_budget();
    test_structured_hot_tiering();
    return 0;
}
