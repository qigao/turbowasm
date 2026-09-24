#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct interrupt_counter {
    uint32_t checks;
    uint32_t stop_at;
} interrupt_counter;

static bool interrupt_after(void *context) {
    interrupt_counter *counter = (interrupt_counter *)context;
    assert(counter != NULL);
    ++counter->checks;
    return counter->checks >= counter->stop_at;
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type 0: (i32)->i32, type 1: ()->i32 */
        0x01, 0x0a,
        0x02,
        0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7f,

        0x03, 0x03,
        0x02, 0x00, 0x01,

        0x0a, 0x0f,
        0x02,

        /* f0: p0 + 1 */
        0x08,
        0x00,
        0x20, 0x00,
        0x41, 0x01,
        0x6a,
        0x0b,

        /* f1: 7 */
        0x04,
        0x00,
        0x41, 0x07,
        0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value public_result = {0};
    turbowasm_value helper_result = {0};
    size_t public_count = 0u;
    size_t helper_count = 0u;
    turbowasm_trap public_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap helper_trap = TURBOWASM_TRAP_NONE;
    turbowasm_jit_invocation_context context = {0};
    turbowasm_jit_execution_control execution = {0};
    interrupt_counter counter = {0};

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = 41;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    context.instance = (turbowasm_instance_impl *)instance.impl;
    context.execution = NULL;
    context.depth = 0u;

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &public_result, 1u,
               &public_count,
               &public_trap) == TURBOWASM_OK);

    assert(turbowasm_jit_direct_call(
               &context, 0u,
               &argument, 1u,
               &helper_result, 1u,
               &helper_count,
               &helper_trap) == TURBOWASM_OK);

    assert(public_count == 1u);
    assert(helper_count == 1u);
    assert(public_trap == TURBOWASM_TRAP_NONE);
    assert(helper_trap == TURBOWASM_TRAP_NONE);
    assert(public_result.kind == TURBOWASM_VALUE_I32);
    assert(helper_result.kind == TURBOWASM_VALUE_I32);
    assert(public_result.as.i32 == 42);
    assert(helper_result.as.i32 == public_result.as.i32);

    /* f1 executes two opcodes: i32.const + end. */
    execution.fuel_limited = true;
    execution.fuel_remaining = 2u;
    execution.should_interrupt = NULL;
    execution.interrupt_context = NULL;
    context.execution = &execution;

    helper_count = 0u;
    helper_trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_jit_direct_call(
               &context, 1u,
               NULL, 0u,
               &helper_result, 1u,
               &helper_count,
               &helper_trap) == TURBOWASM_OK);
    assert(execution.fuel_remaining == 0u);
    assert(helper_result.kind == TURBOWASM_VALUE_I32);
    assert(helper_result.as.i32 == 7);
    assert(helper_trap == TURBOWASM_TRAP_NONE);

    /* The same execution-control object is not reset for the next call. */
    helper_count = 0u;
    helper_trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_jit_direct_call(
               &context, 1u,
               NULL, 0u,
               &helper_result, 1u,
               &helper_count,
               &helper_trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(execution.fuel_remaining == 0u);
    assert(helper_count == 0u);
    assert(helper_trap == TURBOWASM_TRAP_NONE);

    /* Interruption propagates as execution policy, not as a Wasm trap. */
    execution.fuel_limited = false;
    counter.checks = 0u;
    counter.stop_at = 1u;
    execution.should_interrupt = interrupt_after;
    execution.interrupt_context = &counter;

    helper_count = 0u;
    helper_trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_jit_direct_call(
               &context, 1u,
               NULL, 0u,
               &helper_result, 1u,
               &helper_count,
               &helper_trap) == TURBOWASM_INTERRUPTED);
    assert(counter.checks == 1u);
    assert(helper_count == 0u);
    assert(helper_trap == TURBOWASM_TRAP_NONE);

    /* Helper preserves the interpreter call-depth contract. */
    execution.should_interrupt = NULL;
    execution.interrupt_context = NULL;
    context.execution = NULL;
    context.depth = 255u;
    helper_count = 0u;
    helper_trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_jit_direct_call(
               &context, 1u,
               NULL, 0u,
               &helper_result, 1u,
               &helper_count,
               &helper_trap) == TURBOWASM_TRAPPED);
    assert(helper_trap == TURBOWASM_TRAP_CALL_STACK_EXHAUSTED);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
