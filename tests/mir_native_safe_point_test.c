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

static void test_exact_native_fuel_and_interrupt(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        /* i32.const 1; i32.const 2; i32.add; end = 4 instructions */
        0x0a, 0x09,
        0x01, 0x07,
        0x00,
        0x41, 0x01,
        0x41, 0x02,
        0x6a,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_jit_invocation_context context = {0};
    turbowasm_jit_execution_control execution = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    interrupt_counter counter = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    assert(backend.supports_execution_control);

    compile_function(&backend, &module, 0u, &compiled);

    context.instance =
        (turbowasm_instance_impl *)instance.impl;
    context.execution = &execution;
    context.depth = 0u;

    /* 3 fuel pays const/const/add; end is the fourth instruction. */
    execution.fuel_limited = true;
    execution.fuel_remaining = 3u;
    assert(backend.invoke(
               &compiled,
               &context,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Exact 4-instruction budget succeeds and leaves no fuel. */
    execution.fuel_remaining = 4u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(backend.invoke(
               &compiled,
               &context,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 3);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Interruption is checked before each native-lowered Wasm instruction. */
    execution.fuel_limited = false;
    execution.fuel_remaining = 0u;
    counter.checks = 0u;
    counter.stop_at = 3u;
    execution.should_interrupt = stop_after_checks;
    execution.interrupt_context = &counter;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(backend.invoke(
               &compiled,
               &context,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_INTERRUPTED);
    assert(counter.checks == 3u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_direct_call_shares_native_budget(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        /* f0 callee, f1 caller */
        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x0c,
        0x02,

        /* f0: const 7; nop; end = 3 instructions, MIR-ineligible */
        0x05,
        0x00,
        0x41, 0x07,
        0x01,
        0x0b,

        /* f1: call f0; end = 2 instructions, MIR-eligible */
        0x04,
        0x00,
        0x10, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_jit_invocation_context context = {0};
    turbowasm_jit_execution_control execution = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *callee;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    callee = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(callee != NULL);
    assert(!backend.is_function_eligible(
        backend.context, &impl->validation, 0u, callee));

    compile_function(&backend, &module, 1u, &compiled);

    context.instance =
        (turbowasm_instance_impl *)instance.impl;
    context.execution = &execution;
    context.depth = 0u;

    /*
     * Shared budget:
     * caller call checkpoint = 1
     * callee const/nop/end    = 2/3/4
     * caller end              = 5
     */
    execution.fuel_limited = true;
    execution.fuel_remaining = 4u;
    assert(backend.invoke(
               &compiled,
               &context,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    execution.fuel_remaining = 5u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(backend.invoke(
               &compiled,
               &context,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_exact_native_fuel_and_interrupt();
    test_direct_call_shares_native_budget();
    return 0;
}
