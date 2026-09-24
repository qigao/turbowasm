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

#define SIGN_V128 \
    0x00, 0x00, 0x00, 0x80, \
    0x00, 0x00, 0x00, 0x80, \
    0x00, 0x00, 0x00, 0x80, \
    0x00, 0x00, 0x00, 0x80

#define ZERO_V128 \
    0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00

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

static const turbowasm_validation_function *function_at(
    const turbowasm_module_impl *module) {
    assert(module != NULL);
    return turbowasm_validation_context_function(
        &module->validation, 0u);
}

static void compile_function(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_compiled_function *compiled) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function =
        function_at(impl);

    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        0u,
        function));
    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               0u,
               function,
               compiled) == TURBOWASM_OK);
    assert(compiled->impl != NULL);
}

static int32_t invoke_interpreter(
    turbowasm_instance *instance,
    const turbowasm_value *argument) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 0u,
               argument, argument == NULL ? 0u : 1u,
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
    const turbowasm_value *argument,
    turbowasm_value *result,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_jit_invocation_context context = {0};
    salts_v128 outer_slots[2] = {{{0}}};
    uint8_t before[sizeof(outer_slots)];

    memset(outer_slots, 0xa5, sizeof(outer_slots));
    memcpy(before, outer_slots, sizeof(before));

    context.instance =
        (turbowasm_instance_impl *)instance->impl;
    context.execution = execution;
    context.depth = 0u;
    context.simd_slots = outer_slots;
    context.simd_slot_count = 2u;

    {
        turbowasm_status status = backend->invoke(
            compiled,
            &context,
            argument, argument == NULL ? 0u : 1u,
            result, 1u,
            result_count,
            trap);

        assert(context.simd_slots == outer_slots);
        assert(context.simd_slot_count == 2u);
        assert(memcmp(before, outer_slots, sizeof(before)) == 0);
        return status;
    }
}

static void compare_case(
    const uint8_t *bytes,
    size_t size,
    const turbowasm_value *argument,
    int32_t expected) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_interpreter(&instance, argument) == expected);

    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, &compiled);

    assert(invoke_compiled(
               &backend, &compiled, &instance,
               NULL, argument,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == expected);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static const uint8_t if_v128_module[] = {
    WASM_HEADER,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x31, 0x01, 0x2f,
    0x00,
    0x20, 0x00,
    0x04, 0x7b,
    0xfd, 0x0c, SIGN_V128,
    0x05,
    0xfd, 0x0c, ZERO_V128,
    0x0b,
    0xfd, 0xa4, 0x01,
    0x0b
};

static const uint8_t block_v128_module[] = {
    WASM_HEADER,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x30, 0x01, 0x2e,
    0x00,
    0x02, 0x7b,
    0xfd, 0x0c, SIGN_V128,
    0x0c, 0x00,
    0xfd, 0x0c, ZERO_V128,
    0x0b,
    0xfd, 0xa4, 0x01,
    0x0b
};

static const uint8_t loop_v128_module[] = {
    WASM_HEADER,
    0x01, 0x0b, 0x02,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x01, 0x7b, 0x01, 0x7b,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x25, 0x01, 0x23,
    0x00,
    0xfd, 0x0c, SIGN_V128,
    0x03, 0x01,
    0x20, 0x00,
    0x41, 0x01,
    0x6b,
    0x22, 0x00,
    0x0d, 0x00,
    0x0b,
    0xfd, 0xa4, 0x01,
    0x0b
};

static const uint8_t br_table_v128_module[] = {
    WASM_HEADER,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x25, 0x01, 0x23,
    0x00,
    0x02, 0x7b,
    0x02, 0x7b,
    0xfd, 0x0c, SIGN_V128,
    0x20, 0x00,
    0x0e, 0x01, 0x00, 0x01,
    0x0b,
    0x0b,
    0xfd, 0xa4, 0x01,
    0x0b
};

static const uint8_t select_v128_module[] = {
    WASM_HEADER,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x31, 0x01, 0x2f,
    0x00,
    0x02, 0x7b,
    0xfd, 0x0c, SIGN_V128,
    0xfd, 0x0c, ZERO_V128,
    0x20, 0x00,
    0x1b,
    0x0b,
    0xfd, 0xa4, 0x01,
    0x0b
};

static void test_if_else_v128_merge(void) {
    turbowasm_value one = i32_value(1);
    turbowasm_value zero = i32_value(0);

    compare_case(
        if_v128_module, sizeof(if_v128_module), &one, 15);
    compare_case(
        if_v128_module, sizeof(if_v128_module), &zero, 0);
}

static void test_block_branch_v128_merge(void) {
    compare_case(
        block_v128_module, sizeof(block_v128_module), NULL, 15);
}

static void test_loop_v128_parameter_and_policy(void) {
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
               &module,
               loop_v128_module,
               sizeof(loop_v128_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_interpreter(&instance, &argument) == 15);

    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    compile_function(&backend, &module, &compiled);

    execution.fuel_limited = true;
    execution.fuel_remaining = 19u;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution, &argument,
               &result, &result_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    execution.fuel_remaining = 20u;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution, &argument,
               &result, &result_count,
               &trap) == TURBOWASM_OK);
    assert(execution.fuel_remaining == 0u);
    assert(result_count == 1u);
    assert(result.as.i32 == 15);
    assert(trap == TURBOWASM_TRAP_NONE);

    execution.fuel_limited = false;
    counter.checks = 0u;
    counter.stop_at = 6u;
    execution.should_interrupt = stop_after_checks;
    execution.interrupt_context = &counter;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(invoke_compiled(
               &backend, &compiled, &instance,
               &execution, &argument,
               &result, &result_count,
               &trap) == TURBOWASM_INTERRUPTED);
    assert(counter.checks == 6u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_br_table_v128_forwarding(void) {
    turbowasm_value zero = i32_value(0);
    turbowasm_value one = i32_value(1);

    compare_case(
        br_table_v128_module, sizeof(br_table_v128_module),
        &zero, 15);
    compare_case(
        br_table_v128_module, sizeof(br_table_v128_module),
        &one, 15);
}

static void test_v128_select(void) {
    turbowasm_value one = i32_value(1);
    turbowasm_value zero = i32_value(0);

    compare_case(
        select_v128_module, sizeof(select_v128_module), &one, 15);
    compare_case(
        select_v128_module, sizeof(select_v128_module), &zero, 0);
}

int main(void) {
    test_if_else_v128_merge();
    test_block_branch_v128_merge();
    test_loop_v128_parameter_and_policy();
    test_br_table_v128_forwarding();
    test_v128_select();
    return 0;
}
