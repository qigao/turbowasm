#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
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

static const uint8_t cross_frame_bytes[] = {
    WASM_HEADER,

    /* type0: (i32)->(); type1: ()->i32; type2: ()->() */
    0x01, 0x0c,
    0x03,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,
    0x60, 0x00, 0x00,

    /*
     * func0 thrower, func1 direct catcher, func2 table setter,
     * func3 indirect catcher, func4 tail wrapper, func5 tail catcher.
     */
    0x03, 0x07,
    0x06, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01,

    /* table0 funcref min=1 */
    0x04, 0x04,
    0x01, 0x70, 0x00, 0x01,

    /* tag0: (i32)->() */
    0x0d, 0x03,
    0x01, 0x00, 0x00,

    /* declarative element declares ref.func 0 */
    0x09, 0x05,
    0x01, 0x03, 0x00, 0x01, 0x00,

    0x0a, 0x4f,
    0x06,

    /* func0: i32.const 42; throw tag0 */
    0x06,
    0x00, 0x41, 0x2a, 0x08, 0x00, 0x0b,

    /*
     * func1:
     * block (result i32)
     *   try_table (catch tag0 0)
     *     call 0
     *     drop
     *   end
     *   i32.const 0
     * end
     */
    0x11,
    0x00,
    0x02, 0x7f,
    0x1f, 0x40, 0x01, 0x00, 0x00, 0x00,
    0x10, 0x00,
    0x1a,
    0x0b,
    0x41, 0x00,
    0x0b,
    0x0b,

    /* func2: table[0] = ref.func 0 */
    0x08,
    0x00,
    0x41, 0x00,
    0xd2, 0x00,
    0x26, 0x00,
    0x0b,

    /*
     * func3: same catch, but via call_indirect type1/table0.
     */
    0x14,
    0x00,
    0x02, 0x7f,
    0x1f, 0x40, 0x01, 0x00, 0x00, 0x00,
    0x41, 0x00,
    0x11, 0x01, 0x00,
    0x1a,
    0x0b,
    0x41, 0x00,
    0x0b,
    0x0b,

    /* func4: return_call func0 */
    0x04,
    0x00, 0x12, 0x00, 0x0b,

    /* func5: catches exception propagated through tail func4 */
    0x11,
    0x00,
    0x02, 0x7f,
    0x1f, 0x40, 0x01, 0x00, 0x00, 0x00,
    0x10, 0x04,
    0x1a,
    0x0b,
    0x41, 0x00,
    0x0b,
    0x0b
};

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index, NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_direct_indirect_and_tail_unwind(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, cross_frame_bytes, sizeof(cross_frame_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    /* Direct callee exception is caught by the caller frame. */
    assert(invoke_i32(&instance, 1u) == 42);

    /* Initialize table0, then qualify the same behavior through call_indirect. */
    assert(turbowasm_instance_invoke(
               &instance, 2u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(invoke_i32(&instance, 3u) == 42);

    /* The tail frame disappears, but its exception reaches the higher caller. */
    assert(invoke_i32(&instance, 5u) == 42);

    /* Invoking the tail wrapper directly leaves the Wasm exception uncaught. */
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
               &instance, 4u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_EXCEPTION);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* A later invocation must not observe stale pending-exception state. */
    assert(invoke_i32(&instance, 1u) == 42);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_execution_control_is_not_an_exception(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    interrupt_counter counter = {0};

    assert(turbowasm_module_load_borrowed(
               &module, cross_frame_bytes, sizeof(cross_frame_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    /*
     * func1 has a catch for tag0, but fuel/interruption are policy statuses,
     * not Wasm exceptions, and therefore must propagate unchanged.
     */
    options.has_fuel_limit = true;
    options.fuel = 1u;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 1u, NULL, 0u,
               &result, 1u, &result_count, &trap, &options) ==
           TURBOWASM_FUEL_EXHAUSTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    options.has_fuel_limit = false;
    counter.checks = 0u;
    counter.stop_at = 2u;
    options.should_interrupt = stop_after_checks;
    options.interrupt_context = &counter;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 1u, NULL, 0u,
               &result, 1u, &result_count, &trap, &options) ==
           TURBOWASM_INTERRUPTED);
    assert(counter.checks == 2u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static const uint8_t linked_provider_bytes[] = {
    WASM_HEADER,
    /* type0: (i32)->(); type1: ()->i32 */
    0x01, 0x09, 0x02,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,

    /* function0 type1 */
    0x03, 0x02, 0x01, 0x01,

    /* tag0=a, tag1=b: same signature, distinct identity */
    0x0d, 0x05, 0x02,
    0x00, 0x00,
    0x00, 0x00,

    /* export tag a, tag b, and thrower f */
    0x07, 0x0d, 0x03,
    0x01, 0x61, 0x04, 0x00,
    0x01, 0x62, 0x04, 0x01,
    0x01, 0x66, 0x00, 0x00,

    /* func0: i32.const 7; throw tag1 */
    0x0a, 0x08, 0x01, 0x06,
    0x00, 0x41, 0x07, 0x08, 0x01, 0x0b
};

static const uint8_t linked_consumer_bytes[] = {
    WASM_HEADER,
    /* type0: (i32)->(); type1: ()->i32 */
    0x01, 0x09, 0x02,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,

    /*
     * import p.a tag0, p.b tag1, p.f function0.
     * The imported tags are normalized to provider store identity.
     */
    0x02, 0x15, 0x03,
    0x01, 0x70, 0x01, 0x61, 0x04, 0x00, 0x00,
    0x01, 0x70, 0x01, 0x62, 0x04, 0x00, 0x00,
    0x01, 0x70, 0x01, 0x66, 0x00, 0x01,

    /* defined consumer function1 type1 */
    0x03, 0x02, 0x01, 0x01,

    /*
     * outer block result i32
     *   inner block result i32
     *     try_table
     *       catch tag0 -> inner
     *       catch tag1 -> outer
     *       call imported provider function0
     *       drop
     *     end
     *     i32.const 0
     *   end
     *   i32.const 100
     *   i32.add
     * end
     *
     * Provider throws tag1 payload 7. Correct canonical identity returns 7;
     * structural/signature-only matching against tag0 would produce 107.
     */
    0x0a, 0x1d, 0x01, 0x1b,
    0x00,
    0x02, 0x7f,
    0x02, 0x7f,
    0x1f, 0x40, 0x02,
    0x00, 0x00, 0x00,
    0x00, 0x01, 0x01,
    0x10, 0x00,
    0x1a,
    0x0b,
    0x41, 0x00,
    0x0b,
    0x41, 0xe4, 0x00,
    0x6a,
    0x0b,
    0x0b
};

static void test_linked_provider_identity_crosses_call_frame(void) {
    static const uint8_t p_name_bytes[] = {(uint8_t)'p'};
    const turbowasm_name p_name = {p_name_bytes, 1u};
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               linked_provider_bytes,
               sizeof(linked_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               linked_consumer_bytes,
               sizeof(linked_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, p_name, &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer, &consumer_module, &linker) == TURBOWASM_OK);

    assert(invoke_i32(&consumer, 1u) == 7);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_direct_indirect_and_tail_unwind();
    test_execution_control_is_not_an_exception();
    test_linked_provider_identity_crosses_call_frame();
    return 0;
}
