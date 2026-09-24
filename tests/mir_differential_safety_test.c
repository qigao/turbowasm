#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct differential_pair {
    turbowasm_module module;
    turbowasm_instance interpreter;
    turbowasm_instance tiered;
} differential_pair;

typedef struct call_result {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value value;
} call_result;

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

static void pair_init(
    differential_pair *pair,
    const uint8_t *bytes,
    size_t size) {
    turbowasm_jit_backend backend = {0};
    turbowasm_instance_impl *tiered_impl;

    memset(pair, 0, sizeof(*pair));

    assert(turbowasm_module_load_borrowed(
               &pair->module, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &pair->interpreter, &pair->module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &pair->tiered, &pair->module) == TURBOWASM_OK);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    tiered_impl = (turbowasm_instance_impl *)pair->tiered.impl;
    assert(tiered_impl != NULL);

    /* Threshold=1 makes the first unbounded eligible call compile
     * immediately, which makes differential intent explicit. */
    assert(turbowasm_jit_instance_attach_backend(
               tiered_impl, &backend, 1u) == TURBOWASM_OK);
    assert(backend.context == NULL);
}

static void pair_destroy(differential_pair *pair) {
    turbowasm_instance_destroy(&pair->interpreter);
    turbowasm_instance_destroy(&pair->tiered);
    turbowasm_module_destroy(&pair->module);
}

static call_result invoke_capture(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    const turbowasm_execution_options *options) {
    call_result result;
    memset(&result, 0, sizeof(result));
    result.trap = TURBOWASM_TRAP_NONE;

    if (options == NULL) {
        result.status = turbowasm_instance_invoke(
            instance,
            function_index,
            arguments,
            argument_count,
            &result.value,
            1u,
            &result.count,
            &result.trap);
    } else {
        result.status = turbowasm_instance_invoke_with_options(
            instance,
            function_index,
            arguments,
            argument_count,
            &result.value,
            1u,
            &result.count,
            &result.trap,
            options);
    }

    return result;
}

static void assert_value_equal(
    const turbowasm_value *left,
    const turbowasm_value *right) {
    assert(left->kind == right->kind);

    switch (left->kind) {
        case TURBOWASM_VALUE_I32:
            assert(left->as.i32 == right->as.i32);
            break;
        case TURBOWASM_VALUE_I64:
            assert(left->as.i64 == right->as.i64);
            break;
        case TURBOWASM_VALUE_F32:
            assert(left->as.f32 == right->as.f32);
            break;
        case TURBOWASM_VALUE_F64:
            assert(left->as.f64 == right->as.f64);
            break;
        case TURBOWASM_VALUE_V128:
            assert(memcmp(
                left->as.v128.bits.bytes,
                right->as.v128.bits.bytes,
                sizeof(left->as.v128.bits.bytes)) == 0);
            break;
        case TURBOWASM_VALUE_FUNCREF:
            assert(left->as.funcref.is_null ==
                   right->as.funcref.is_null);
            assert(left->as.funcref.function_index ==
                   right->as.funcref.function_index);
            break;
        default:
            assert(0 && "unexpected result kind");
    }
}

static void assert_call_equal(
    call_result interpreted,
    call_result tiered) {
    assert(tiered.status == interpreted.status);
    assert(tiered.trap == interpreted.trap);
    assert(tiered.count == interpreted.count);

    if (interpreted.count != 0u)
        assert_value_equal(&interpreted.value, &tiered.value);
}

static void compare_unbounded(
    differential_pair *pair,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    call_result interpreted = invoke_capture(
        &pair->interpreter,
        function_index,
        arguments,
        argument_count,
        NULL);
    call_result tiered = invoke_capture(
        &pair->tiered,
        function_index,
        arguments,
        argument_count,
        NULL);

    assert_call_equal(interpreted, tiered);
}

static turbowasm_instance_impl *tiered_impl(
    differential_pair *pair) {
    turbowasm_instance_impl *impl =
        (turbowasm_instance_impl *)pair->tiered.impl;
    assert(impl != NULL);
    return impl;
}

static void test_integer_locals_and_fallback(void) {
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

        0x0f,
        0x01, 0x01, 0x7e,
        0x20, 0x00,
        0x22, 0x01,
        0x21, 0x00,
        0x20, 0x01,
        0x42, 0x03,
        0x7c,
        0x0b,

        /* valid Wasm; arity three remains MIR-ineligible */
        0x04,
        0x00,
        0x20, 0x00,
        0x0b
    };
    differential_pair pair;
    turbowasm_value i32_args[3] = {0};
    turbowasm_value i64_arg = {0};
    turbowasm_instance_impl *impl;

    i32_args[0].kind = TURBOWASM_VALUE_I32;
    i32_args[0].as.i32 = 10;
    i32_args[1].kind = TURBOWASM_VALUE_I32;
    i32_args[1].as.i32 = 5;
    i32_args[2].kind = TURBOWASM_VALUE_I32;
    i32_args[2].as.i32 = 99;

    i64_arg.kind = TURBOWASM_VALUE_I64;
    i64_arg.as.i64 = INT64_C(40);

    pair_init(&pair, bytes, sizeof(bytes));

    compare_unbounded(&pair, 0u, i32_args, 2u);
    compare_unbounded(&pair, 1u, &i64_arg, 1u);
    compare_unbounded(&pair, 2u, i32_args, 3u);

    impl = tiered_impl(&pair);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[1].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[2].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);

    pair_destroy(&pair);
}

static void test_float_lowering_and_fallback(void) {
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

        0x12,
        0x01, 0x01, 0x7d,
        0x20, 0x00,
        0x22, 0x01,
        0x21, 0x00,
        0x20, 0x01,
        0x43, 0x00, 0x00, 0xc0, 0x3f,
        0x94,
        0x0b,

        0x14,
        0x01, 0x01, 0x7c,
        0x20, 0x00,
        0x21, 0x01,
        0x20, 0x01,
        0x44, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x02, 0x40,
        0xa0,
        0x0b,

        /* two f32 params remain MIR-ineligible */
        0x04,
        0x00,
        0x20, 0x00,
        0x0b
    };
    differential_pair pair;
    turbowasm_value f32_args[2] = {0};
    turbowasm_value f64_arg = {0};
    turbowasm_instance_impl *impl;

    f32_args[0].kind = TURBOWASM_VALUE_F32;
    f32_args[0].as.f32 = 2.0f;
    f32_args[1].kind = TURBOWASM_VALUE_F32;
    f32_args[1].as.f32 = 9.0f;

    f64_arg.kind = TURBOWASM_VALUE_F64;
    f64_arg.as.f64 = 10.0;

    pair_init(&pair, bytes, sizeof(bytes));

    compare_unbounded(&pair, 0u, f32_args, 1u);
    compare_unbounded(&pair, 1u, &f64_arg, 1u);
    compare_unbounded(&pair, 2u, f32_args, 2u);

    impl = tiered_impl(&pair);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[1].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[2].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);

    pair_destroy(&pair);
}

static void test_direct_calls_and_trap_parity(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* (i32)->i32, (f32)->f32, ()->i32 */
        0x01, 0x0f,
        0x03,
        0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x60, 0x01, 0x7d, 0x01, 0x7d,
        0x60, 0x00, 0x01, 0x7f,

        0x03, 0x07,
        0x06, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02,

        0x0a, 0x2f,
        0x06,

        /* interpreted-only integer callee */
        0x07,
        0x00,
        0x20, 0x00,
        0x41, 0x02,
        0x6d,
        0x0b,

        /* compiled caller */
        0x09,
        0x00,
        0x20, 0x00,
        0x10, 0x00,
        0x41, 0x01,
        0x6a,
        0x0b,

        /* interpreted-only f32 callee due nop */
        0x05,
        0x00,
        0x20, 0x00,
        0x01,
        0x0b,

        /* compiled caller */
        0x0c,
        0x00,
        0x20, 0x00,
        0x10, 0x02,
        0x43, 0x00, 0x00, 0xc0, 0x3f,
        0x94,
        0x0b,

        /* interpreted-only trapping callee */
        0x03,
        0x00,
        0x00,
        0x0b,

        /* compiled caller propagates trap */
        0x04,
        0x00,
        0x10, 0x04,
        0x0b
    };
    differential_pair pair;
    turbowasm_value i32_arg = {0};
    turbowasm_value f32_arg = {0};
    turbowasm_instance_impl *impl;

    i32_arg.kind = TURBOWASM_VALUE_I32;
    i32_arg.as.i32 = 20;
    f32_arg.kind = TURBOWASM_VALUE_F32;
    f32_arg.as.f32 = 2.0f;

    pair_init(&pair, bytes, sizeof(bytes));

    compare_unbounded(&pair, 1u, &i32_arg, 1u);
    compare_unbounded(&pair, 3u, &f32_arg, 1u);
    compare_unbounded(&pair, 5u, NULL, 0u);

    impl = tiered_impl(&pair);
    assert(impl->jit_functions[1].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[3].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[5].state ==
           TURBOWASM_JIT_COMPILED);

    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);
    assert(impl->jit_functions[2].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);
    assert(impl->jit_functions[4].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);

    pair_destroy(&pair);
}

static void test_compiled_caller_preserves_state_mutation(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x04,
        0x03, 0x00, 0x00, 0x00,

        /* mutable i32 global = 0 */
        0x06, 0x06,
        0x01, 0x7f, 0x01,
        0x41, 0x00, 0x0b,

        0x0a, 0x17,
        0x03,

        /* f0: ++global; return global (MIR-ineligible) */
        0x0b,
        0x00,
        0x23, 0x00,
        0x41, 0x01,
        0x6a,
        0x24, 0x00,
        0x23, 0x00,
        0x0b,

        /* f1: compiled direct caller */
        0x04,
        0x00,
        0x10, 0x00,
        0x0b,

        /* f2: getter (MIR-ineligible) */
        0x04,
        0x00,
        0x23, 0x00,
        0x0b
    };
    differential_pair pair;
    turbowasm_instance_impl *impl;

    pair_init(&pair, bytes, sizeof(bytes));

    compare_unbounded(&pair, 1u, NULL, 0u);
    compare_unbounded(&pair, 2u, NULL, 0u);
    compare_unbounded(&pair, 1u, NULL, 0u);
    compare_unbounded(&pair, 2u, NULL, 0u);

    impl = tiered_impl(&pair);
    assert(impl->jit_functions[1].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);
    assert(impl->jit_functions[2].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);

    pair_destroy(&pair);
}

static void test_compiled_caller_preserves_memory_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x05, 0x03,
        0x01, 0x00, 0x01,

        0x0a, 0x0f,
        0x02,

        /* f0: i32.load at 65535 => OOB (MIR-ineligible) */
        0x08,
        0x00,
        0x41, 0xff, 0xff, 0x03,
        0x28, 0x02, 0x00,
        0x0b,

        /* f1: compiled direct caller */
        0x04,
        0x00,
        0x10, 0x00,
        0x0b
    };
    differential_pair pair;
    turbowasm_instance_impl *impl;
    call_result interpreted;
    call_result tiered;

    pair_init(&pair, bytes, sizeof(bytes));

    interpreted = invoke_capture(
        &pair.interpreter, 1u, NULL, 0u, NULL);
    tiered = invoke_capture(
        &pair.tiered, 1u, NULL, 0u, NULL);
    assert_call_equal(interpreted, tiered);
    assert(interpreted.status == TURBOWASM_TRAPPED);
    assert(interpreted.trap ==
           TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    impl = tiered_impl(&pair);
    assert(impl->jit_functions[1].state ==
           TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_INTERPRET_ONLY);

    pair_destroy(&pair);
}

static void test_execution_control_parity_and_cache(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        /* i32.const 7 */
        0x0a, 0x06,
        0x01,
        0x04, 0x00, 0x41, 0x07, 0x0b
    };
    differential_pair pair;
    turbowasm_instance_impl *impl;
    turbowasm_execution_options fuel = {0};
    turbowasm_execution_options interrupt_a = {0};
    turbowasm_execution_options interrupt_b = {0};
    interrupt_counter a = {0};
    interrupt_counter b = {0};
    call_result interpreted;
    call_result tiered;

    pair_init(&pair, bytes, sizeof(bytes));

    /* Compile the tiered copy first. */
    compare_unbounded(&pair, 0u, NULL, 0u);
    impl = tiered_impl(&pair);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);

    fuel.has_fuel_limit = true;
    fuel.fuel = 1u;
    interpreted = invoke_capture(
        &pair.interpreter, 0u, NULL, 0u, &fuel);
    tiered = invoke_capture(
        &pair.tiered, 0u, NULL, 0u, &fuel);
    assert_call_equal(interpreted, tiered);
    assert(interpreted.status == TURBOWASM_FUEL_EXHAUSTED);
    assert(interpreted.trap == TURBOWASM_TRAP_NONE);

    fuel.fuel = 2u;
    interpreted = invoke_capture(
        &pair.interpreter, 0u, NULL, 0u, &fuel);
    tiered = invoke_capture(
        &pair.tiered, 0u, NULL, 0u, &fuel);
    assert_call_equal(interpreted, tiered);
    assert(interpreted.status == TURBOWASM_OK);

    a.stop_at = 1u;
    b.stop_at = 1u;
    interrupt_a.should_interrupt = stop_after_checks;
    interrupt_a.interrupt_context = &a;
    interrupt_b.should_interrupt = stop_after_checks;
    interrupt_b.interrupt_context = &b;

    interpreted = invoke_capture(
        &pair.interpreter, 0u, NULL, 0u, &interrupt_a);
    tiered = invoke_capture(
        &pair.tiered, 0u, NULL, 0u, &interrupt_b);
    assert_call_equal(interpreted, tiered);
    assert(interpreted.status == TURBOWASM_INTERRUPTED);
    assert(interpreted.trap == TURBOWASM_TRAP_NONE);
    assert(a.checks == b.checks);

    /* Policy-controlled calls bypass native code but preserve the hot cache. */
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);
    compare_unbounded(&pair, 0u, NULL, 0u);
    assert(impl->jit_functions[0].state ==
           TURBOWASM_JIT_COMPILED);

    pair_destroy(&pair);
}

int main(void) {
    test_integer_locals_and_fallback();
    test_float_lowering_and_fallback();
    test_direct_calls_and_trap_parity();
    test_compiled_caller_preserves_state_mutation();
    test_compiled_caller_preserves_memory_trap();
    test_execution_control_parity_and_cache();
    return 0;
}
