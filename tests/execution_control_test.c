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

static void test_fuel_and_interrupt(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type 0: () -> (), type 1: () -> i32 */
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x7f,

        /* function 0 uses type 0, function 1 uses type 1 */
        0x03, 0x03,
        0x02, 0x00, 0x01,

        0x0a, 0x0e,
        0x02,

        /* function 0: loop { br 0 } */
        0x07,
        0x00,
        0x03, 0x40,
        0x0c, 0x00,
        0x0b,
        0x0b,

        /* function 1: i32.const 7 */
        0x04,
        0x00,
        0x41, 0x07,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    interrupt_counter counter = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    options.has_fuel_limit = true;
    options.fuel = 3u;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap,
               &options) == TURBOWASM_FUEL_EXHAUSTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    options.has_fuel_limit = false;
    options.fuel = 0u;
    counter.checks = 0u;
    counter.stop_at = 4u;
    options.should_interrupt = stop_after_checks;
    options.interrupt_context = &counter;

    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap,
               &options) == TURBOWASM_INTERRUPTED);
    assert(counter.checks == 4u);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* The same instance remains usable after cooperative interruption. */
    assert(turbowasm_instance_invoke(
               &instance, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Two interpreter instructions: i32.const + end. */
    options.should_interrupt = NULL;
    options.interrupt_context = NULL;
    options.has_fuel_limit = true;
    options.fuel = 2u;
    result = (turbowasm_value){0};
    assert(turbowasm_instance_invoke_with_options(
               &instance, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap,
               &options) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);
    assert(trap == TURBOWASM_TRAP_NONE);

    options.fuel = 1u;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap,
               &options) == TURBOWASM_FUEL_EXHAUSTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    assert(turbowasm_status_string(TURBOWASM_FUEL_EXHAUSTED) != NULL);
    assert(turbowasm_status_string(TURBOWASM_INTERRUPTED) != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_fuel_and_interrupt();
    return 0;
}
