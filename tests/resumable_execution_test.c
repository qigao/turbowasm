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

typedef struct interrupt_flag {
    bool stop;
    uint32_t checks;
} interrupt_flag;

static bool interrupt_if_set(void *context) {
    interrupt_flag *flag = (interrupt_flag *)context;
    assert(flag != NULL);
    ++flag->checks;
    return flag->stop;
}

static void test_repeated_fuel_resumes_tail_control(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x14,
        0x01,
        0x12,
        0x00,
        /* if n == 0 return 0; else return_call self(n - 1) */
        0x20, 0x00,
        0x45,
        0x04, 0x7f,
          0x41, 0x00,
        0x05,
          0x20, 0x00,
          0x41, 0x01,
          0x6b,
          0x12, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value argument = {0};
    turbowasm_status status;
    const turbowasm_value *result;
    uint32_t yields = 0u;

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = 32;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_execution_create(
               &execution, &instance, 0u, &argument, 1u) == TURBOWASM_OK);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_READY);

    options.has_fuel_limit = true;
    options.fuel = 2u;

    do {
        status = turbowasm_execution_resume(&execution, &options);
        if (status == TURBOWASM_YIELDED) {
            ++yields;
            assert(yields < 1024u);
            assert(turbowasm_execution_state_get(&execution) ==
                   TURBOWASM_EXECUTION_YIELDED);
            assert(turbowasm_execution_yield_reason_get(&execution) ==
                   TURBOWASM_YIELD_FUEL);
        }
    } while (status == TURBOWASM_YIELDED);

    assert(status == TURBOWASM_OK);
    assert(yields > 1u);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_COMPLETED);
    assert(turbowasm_execution_terminal_status(&execution) ==
           TURBOWASM_OK);
    assert(turbowasm_execution_result_count(&execution) == 1u);
    result = turbowasm_execution_result_at(&execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 0);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_interrupt_resume_from_same_safe_point(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0x41, 0x07, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    interrupt_flag flag = {true, 0u};
    turbowasm_status status;
    const turbowasm_value *result;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_execution_create(
               &execution, &instance, 0u, NULL, 0u) == TURBOWASM_OK);

    options.should_interrupt = interrupt_if_set;
    options.interrupt_context = &flag;

    status = turbowasm_execution_resume(&execution, &options);
    assert(status == TURBOWASM_YIELDED);
    assert(flag.checks == 1u);
    assert(turbowasm_execution_yield_reason_get(&execution) ==
           TURBOWASM_YIELD_INTERRUPTION);

    flag.stop = false;
    status = turbowasm_execution_resume(&execution, &options);
    assert(status == TURBOWASM_OK);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_COMPLETED);
    result = turbowasm_execution_result_at(&execution, 0u);
    assert(result != NULL && result->as.i32 == 7);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_nested_eh_survives_repeated_yield(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* type0: (i32)->(); type1: ()->i32 */
        0x01, 0x09, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        /* f0 thrower, f1 caller/catcher */
        0x03, 0x03, 0x02, 0x01, 0x01,
        /* tag0 */
        0x0d, 0x03, 0x01, 0x00, 0x00,
        0x0a, 0x1a, 0x02,
        /* f0: i32.const 42; throw 0 */
        0x06, 0x00, 0x41, 0x2a, 0x08, 0x00, 0x0b,
        /*
         * f1:
         * block (result i32)
         *   try_table (catch tag0 0)
         *     call f0
         *     drop
         *   end
         *   i32.const 0
         * end
         */
        0x11, 0x00,
        0x02, 0x7f,
        0x1f, 0x40, 0x01,
        0x00, 0x00, 0x00,
        0x10, 0x00,
        0x1a,
        0x0b,
        0x41, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    turbowasm_status status;
    const turbowasm_value *result;
    uint32_t yields = 0u;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_execution_create(
               &execution, &instance, 1u, NULL, 0u) == TURBOWASM_OK);

    options.has_fuel_limit = true;
    options.fuel = 1u;

    do {
        status = turbowasm_execution_resume(&execution, &options);
        if (status == TURBOWASM_YIELDED) {
            ++yields;
            assert(turbowasm_execution_yield_reason_get(&execution) ==
                   TURBOWASM_YIELD_FUEL);
        }
    } while (status == TURBOWASM_YIELDED);

    assert(status == TURBOWASM_OK);
    assert(yields > 2u);
    result = turbowasm_execution_result_at(&execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 42);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_terminal_trap_and_exception_are_not_yields(void) {
    static const uint8_t trap_bytes[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x05,
        0x01, 0x03,
        0x00, 0x00, 0x0b
    };
    static const uint8_t exception_bytes[] = {
        WASM_HEADER,
        0x01, 0x08, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x01,
        0x0d, 0x03, 0x01, 0x00, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00, 0x41, 0x01, 0x08, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};

    assert(turbowasm_module_load_borrowed(
               &module, trap_bytes, sizeof(trap_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_execution_create(
               &execution, &instance, 0u, NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(&execution, NULL) ==
           TURBOWASM_TRAPPED);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_TRAPPED);
    assert(turbowasm_execution_trap(&execution) ==
           TURBOWASM_TRAP_UNREACHABLE);
    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    instance = (turbowasm_instance){0};
    execution = (turbowasm_execution){0};
    assert(turbowasm_module_load_borrowed(
               &module, exception_bytes, sizeof(exception_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_execution_create(
               &execution, &instance, 0u, NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(&execution, NULL) ==
           TURBOWASM_EXCEPTION);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_EXCEPTION);
    assert(turbowasm_execution_trap(&execution) ==
           TURBOWASM_TRAP_NONE);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_repeated_fuel_resumes_tail_control();
    test_interrupt_resume_from_same_safe_point();
    test_nested_eh_survives_repeated_yield();
    test_terminal_trap_and_exception_are_not_yields();
    return 0;
}
