#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/scalar_results.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <string.h>

enum { ARGUMENT_COUNT = 4, RESULT_CAPACITY = 8, FUNCTION_COUNT = 14,
       FUEL_TEST_LIMIT = 45 };
static turbowasm_module module;
static turbowasm_instance reference_instance, instance;

static void compare_results(const turbowasm_value *a, const turbowasm_value *b,
    size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index) {
        size_t size = 0u;
        check_equal(a[index].kind, b[index].kind);
        switch (a[index].kind) {
            case TURBOWASM_VALUE_I32: size = sizeof(int32_t); break;
            case TURBOWASM_VALUE_I64: size = sizeof(int64_t); break;
            case TURBOWASM_VALUE_F32: size = sizeof(float); break;
            case TURBOWASM_VALUE_F64: size = sizeof(double); break;
            default: check(false, "unexpected scalar result kind"); break;
        }
        check_equal(memcmp(&a[index].as, &b[index].as, size), 0);
    }
}

static void compare_call(uint32_t function, const turbowasm_value *arguments,
    const turbowasm_execution_options *options) {
    turbowasm_value expected[RESULT_CAPACITY], actual[RESULT_CAPACITY];
    turbowasm_value untouched[RESULT_CAPACITY];
    turbowasm_trap expected_trap, actual_trap;
    turbowasm_status expected_status, actual_status;
    size_t expected_count = 0u, actual_count = 0u;
    memset(untouched, 0xa5, sizeof(untouched));
    memcpy(expected, untouched, sizeof(expected));
    memcpy(actual, untouched, sizeof(actual));
    expected_status = turbowasm_instance_invoke_with_options(&reference_instance,
        function, arguments, ARGUMENT_COUNT, expected, RESULT_CAPACITY,
        &expected_count, &expected_trap, options);
    actual_status = turbowasm_instance_invoke_with_options(&instance,
        function, arguments, ARGUMENT_COUNT, actual, RESULT_CAPACITY,
        &actual_count, &actual_trap, options);
    check_equal(actual_status, expected_status);
    check_equal(actual_trap, expected_trap);
    check_equal(actual_count, expected_count);
    if (actual_status == TURBOWASM_OK)
        compare_results(actual, expected, actual_count);
    else {
        check_equal(actual_count, (size_t)0);
        check_equal(memcmp(actual, untouched, sizeof(actual)), 0);
    }
#ifdef TURBOWASM_TEST_MIR
    {
        turbowasm_instance_impl *impl = instance.impl;
        check(impl->jit_functions[function].state == TURBOWASM_JIT_COMPILED,
            "function %u must compile; state %d", function,
            (int)impl->jit_functions[function].state);
        check_not_null(impl->jit_functions[function].compiled.impl);
    }
#endif
}

spec("scalar result tuples") {
    before_each() {
        check_equal(turbowasm_module_load_borrowed(&module,
            scalar_results_bytes, sizeof(scalar_results_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&reference_instance, &module), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            turbowasm_jit_backend backend = {0};
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(instance.impl,
                &backend, 1u), TURBOWASM_OK);
        }
#endif
    }
    after_each() {
        turbowasm_instance_destroy(&instance);
        turbowasm_instance_destroy(&reference_instance);
        turbowasm_module_destroy(&module);
    }
    it("preserves result order, branch paths, scalar bits and fuel boundaries") {
        turbowasm_execution_options options = {0};
        turbowasm_value args[ARGUMENT_COUNT] = {{0}}, saved[ARGUMENT_COUNT];
        uint32_t function, selector, fuel;
        uint32_t f32_bits = UINT32_C(0x7fc01234);
        uint64_t f64_bits = UINT64_C(0x8000000000000000);
        args[0].kind = TURBOWASM_VALUE_I32;
        args[1].kind = TURBOWASM_VALUE_I64;
        args[1].as.i64 = INT64_MIN;
        args[2].kind = TURBOWASM_VALUE_F32;
        args[3].kind = TURBOWASM_VALUE_F64;
        memcpy(&args[2].as.f32, &f32_bits, sizeof(f32_bits));
        memcpy(&args[3].as.f64, &f64_bits, sizeof(f64_bits));
        for (function = 0u; function < FUNCTION_COUNT; ++function) {
            for (selector = 0u; selector < 3u; ++selector) {
                args[0].as.i32 = (int32_t)selector;
                memcpy(saved, args, sizeof(saved));
                compare_call(function, args, NULL);
                options.has_fuel_limit = true;
                for (fuel = 0u; fuel < FUEL_TEST_LIMIT; ++fuel) {
                    options.fuel = fuel;
                    compare_call(function, args, &options);
                }
                check_equal(memcmp(args, saved, sizeof(args)), 0);
            }
        }
    }
    it("accepts null output for empty tuples and rejects insufficient capacity") {
        turbowasm_value args[ARGUMENT_COUNT] = {{0}}, results[RESULT_CAPACITY];
        turbowasm_value saved[RESULT_CAPACITY];
        turbowasm_trap trap;
        size_t count;
        uint32_t function;
        args[0].kind = TURBOWASM_VALUE_I32;
        args[1].kind = TURBOWASM_VALUE_I64;
        args[2].kind = TURBOWASM_VALUE_F32;
        args[3].kind = TURBOWASM_VALUE_F64;
        for (function = 0u; function < 3u; ++function) {
            check_equal(turbowasm_instance_invoke(&instance, function, args,
                ARGUMENT_COUNT, NULL, 0u, &count, &trap), TURBOWASM_OK);
            check_equal(count, (size_t)0);
            check_equal(trap, TURBOWASM_TRAP_NONE);
        }
        memset(results, 0xa5, sizeof(results));
        memcpy(saved, results, sizeof(saved));
        check_equal(turbowasm_instance_invoke(&instance, 3u, args,
            ARGUMENT_COUNT, results, ARGUMENT_COUNT - 1u, &count, &trap),
            TURBOWASM_INVALID_ARGUMENT);
        check_equal(count, (size_t)0);
        check_equal(memcmp(results, saved, sizeof(results)), 0);
    }
}
