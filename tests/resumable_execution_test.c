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


typedef struct differential_outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t result_count;
    turbowasm_value result;
} differential_outcome;

static differential_outcome invoke_one_shot(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    differential_outcome outcome = {0};

    outcome.trap = TURBOWASM_TRAP_NONE;
    outcome.status = turbowasm_instance_invoke(
        instance, function_index,
        arguments, argument_count,
        &outcome.result, 1u,
        &outcome.result_count,
        &outcome.trap);
    return outcome;
}

static differential_outcome invoke_resumable_chunked(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    uint64_t fuel_chunk) {
    differential_outcome outcome = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    turbowasm_status status;
    uint32_t guard = 0u;

    assert(fuel_chunk != 0u);
    assert(turbowasm_execution_create(
               &execution, instance, function_index,
               arguments, argument_count) == TURBOWASM_OK);

    options.has_fuel_limit = true;
    options.fuel = fuel_chunk;

    do {
        status = turbowasm_execution_resume(&execution, &options);
        if (status == TURBOWASM_YIELDED) {
            assert(turbowasm_execution_yield_reason_get(&execution) ==
                   TURBOWASM_YIELD_FUEL);
            ++guard;
            assert(guard < 4096u);
        }
    } while (status == TURBOWASM_YIELDED);

    outcome.status = status;
    outcome.trap = turbowasm_execution_trap(&execution);
    outcome.result_count =
        turbowasm_execution_result_count(&execution);
    if (outcome.result_count != 0u) {
        const turbowasm_value *value =
            turbowasm_execution_result_at(&execution, 0u);
        assert(value != NULL);
        outcome.result = *value;
    }

    turbowasm_execution_destroy(&execution);
    return outcome;
}

static void assert_outcome_equal(
    differential_outcome left,
    differential_outcome right) {
    assert(left.status == right.status);
    assert(left.trap == right.trap);
    assert(left.result_count == right.result_count);

    if (left.result_count != 0u) {
        assert(left.result.kind == right.result.kind);
        switch (left.result.kind) {
            case TURBOWASM_VALUE_I32:
                assert(left.result.as.i32 == right.result.as.i32);
                break;
            case TURBOWASM_VALUE_I64:
                assert(left.result.as.i64 == right.result.as.i64);
                break;
            default:
                assert(0 && "unexpected differential result kind");
        }
    }
}

static void test_resumable_differential_equivalence(void) {
    static const uint8_t nested_bytes[] = {
        WASM_HEADER,
        /* type0: () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        /* f0 and f1 type0 */
        0x03, 0x03,
        0x02, 0x00, 0x00,
        0x0a, 0x11,
        0x02,
        /* f0 => 7 */
        0x04, 0x00, 0x41, 0x07, 0x0b,
        /* f1: block(result i32) { call f0; i32.const 1; add } */
        0x0a, 0x00,
        0x02, 0x7f,
        0x10, 0x00,
        0x41, 0x01,
        0x6a,
        0x0b,
        0x0b
    };
    static const uint8_t tail_bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x14,
        0x01, 0x12,
        0x00,
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
    static const uint8_t eh_bytes[] = {
        WASM_HEADER,
        0x01, 0x09, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x01, 0x01,
        0x0d, 0x03, 0x01, 0x00, 0x00,
        0x0a, 0x1a, 0x02,
        0x06, 0x00, 0x41, 0x2a, 0x08, 0x00, 0x0b,
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
    static const uint8_t trap_bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x05, 0x01, 0x03,
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
    const uint8_t *modules[] = {
        nested_bytes, tail_bytes, eh_bytes, trap_bytes, exception_bytes
    };
    const size_t module_sizes[] = {
        sizeof(nested_bytes), sizeof(tail_bytes), sizeof(eh_bytes),
        sizeof(trap_bytes), sizeof(exception_bytes)
    };
    const uint32_t functions[] = {1u, 0u, 1u, 0u, 0u};
    uint32_t module_index;

    for (module_index = 0u; module_index < 5u; ++module_index) {
        turbowasm_module module = {0};
        turbowasm_instance instance = {0};
        turbowasm_value argument = {0};
        const turbowasm_value *arguments = NULL;
        size_t argument_count = 0u;
        differential_outcome expected;
        uint64_t chunk;

        if (module_index == 1u) {
            argument.kind = TURBOWASM_VALUE_I32;
            argument.as.i32 = 24;
            arguments = &argument;
            argument_count = 1u;
        }

        assert(turbowasm_module_load_borrowed(
                   &module, modules[module_index],
                   module_sizes[module_index]) == TURBOWASM_OK);
        assert(turbowasm_instance_create(
                   &instance, &module) == TURBOWASM_OK);

        expected = invoke_one_shot(
            &instance, functions[module_index],
            arguments, argument_count);

        for (chunk = 1u; chunk <= 3u; ++chunk) {
            differential_outcome actual =
                invoke_resumable_chunked(
                    &instance, functions[module_index],
                    arguments, argument_count, chunk);
            assert_outcome_equal(expected, actual);
        }

        turbowasm_instance_destroy(&instance);
        turbowasm_module_destroy(&module);
    }
}

int main(void) {
    test_repeated_fuel_resumes_tail_control();
    test_interrupt_resume_from_same_safe_point();
    test_nested_eh_survives_repeated_yield();
    test_terminal_trap_and_exception_are_not_yields();
    test_resumable_differential_equivalence();
    return 0;
}
