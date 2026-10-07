#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/scalar_tail_calls.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { HOST = 0, LEFT = 1, RIGHT = 2, ENTRY = 3, VOID_ENTRY = 4,
    VOID_TARGET = 5, INTERPRETED_ENTRY = 6, INTERPRETED_TARGET = 7,
    HOST_ENTRY = 8, TRAP_ENTRY = 9, TRAP_TARGET = 10,
    ZERO_ARGS_ENTRY = 11, ZERO_ARGS_TARGET = 12, FUNCTION_COUNT = 13,
    TUPLE_SIZE = 4, WIDE_ARGUMENTS = 21, CHAIN_LENGTH = 4096,
    FUEL_BOUNDARIES = 100 };
static const turbowasm_value_kind kinds[TUPLE_SIZE] = {
    TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I64,
    TURBOWASM_VALUE_F32, TURBOWASM_VALUE_F64};
static turbowasm_module module;
static turbowasm_instance instance, reference;
static turbowasm_linker linker;
static struct { size_t live, attempts, fail_at; } allocations;

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (++allocations.attempts == allocations.fail_at)
        return NULL;
    pointer = malloc(size);
    if (pointer != NULL)
        ++allocations.live;
    return pointer;
}

static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) {
        check_true(allocations.live != 0u);
        --allocations.live;
    }
    free(pointer);
}

static void make_tuple(turbowasm_value *values, int32_t count) {
    uint32_t f32_bits = UINT32_C(0x7fc01234);
    uint64_t f64_bits = UINT64_C(0x8000000000000000);
    size_t i;
    memset(values, 0, TUPLE_SIZE * sizeof(*values));
    for (i = 0u; i < TUPLE_SIZE; ++i)
        values[i].kind = kinds[i];
    values[0].as.i32 = count;
    values[1].as.i64 = INT64_MIN;
    memcpy(&values[2].as.f32, &f32_bits, sizeof(f32_bits));
    memcpy(&values[3].as.f64, &f64_bits, sizeof(f64_bits));
}

static turbowasm_status host_tuple(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t capacity, size_t *count,
    turbowasm_trap *trap) {
    (void)context;
    /* Re-entry has an independent dispatcher while the host borrows its input. */
    return turbowasm_instance_invoke(turbowasm_host_call_instance(call),
        INTERPRETED_TARGET, arguments, argument_count, results, capacity, count, trap);
}

#ifndef TURBOWASM_TEST_MIR
/* This backend drives the private Runtime tail contract on platforms without MIR.
 * The WAT fixture supplies the reference semantics; native CI uses the real backend. */
static uint32_t function_ids[FUNCTION_COUNT];
static bool eligible(void *context, const turbowasm_validation_context *validation,
    uint32_t index, const turbowasm_validation_function *function) {
    (void)context; (void)validation; (void)function;
    return index == LEFT || index == RIGHT || index == ENTRY || index == VOID_ENTRY ||
        index == INTERPRETED_ENTRY || index == HOST_ENTRY || index == TRAP_ENTRY ||
        index == ZERO_ARGS_ENTRY;
}
static turbowasm_status compile_function(void *context,
    const turbowasm_validation_context *validation, uint32_t index,
    const turbowasm_validation_function *function, turbowasm_compiled_function *out) {
    (void)context; (void)validation; (void)function;
    function_ids[index] = index;
    out->impl = &function_ids[index];
    return TURBOWASM_OK;
}
static turbowasm_status invoke(const turbowasm_compiled_function *compiled,
    turbowasm_jit_invocation_context *context, const turbowasm_value *args,
    size_t argument_count, turbowasm_value *results, size_t capacity,
    size_t *count, turbowasm_trap *trap) {
    uint32_t index = *(uint32_t *)compiled->impl;
    turbowasm_value pending[WIDE_ARGUMENTS] = {{0}};
    turbowasm_status status;
    size_t i;
    (void)argument_count;
    check_equal(context->depth, 0u);
    *count = 0u;
    *trap = TURBOWASM_TRAP_NONE;
    status = turbowasm_jit_execution_checkpoint(context);
    if (status != TURBOWASM_OK)
        return status;
    switch (index) {
        case LEFT:
            if (args[0].as.i32 == 0) {
                check_true(capacity >= TUPLE_SIZE);
                memcpy(results, args, TUPLE_SIZE * sizeof(*args));
                *count = TUPLE_SIZE;
                return TURBOWASM_OK;
            }
            memcpy(pending, args, TUPLE_SIZE * sizeof(*args));
            --pending[0].as.i32;
            for (i = TUPLE_SIZE; i < WIDE_ARGUMENTS; ++i)
                pending[i].kind = TURBOWASM_VALUE_I32;
            return turbowasm_jit_request_tail_call(context, RIGHT, pending, WIDE_ARGUMENTS);
        case RIGHT:
            return turbowasm_jit_request_tail_call(context, LEFT, args, TUPLE_SIZE);
        case ENTRY:
            make_tuple(pending, args[0].as.i32);
            pending[1].as.i64 = -123;
            pending[2].as.f32 = -0.0f;
            pending[3].as.f64 = 7.5;
            return turbowasm_jit_request_tail_call(context, LEFT, pending, TUPLE_SIZE);
        case VOID_ENTRY:
            make_tuple(pending, 1);
            pending[1].as.i64 = 2;
            pending[2].as.f32 = 3;
            pending[3].as.f64 = 4;
            return turbowasm_jit_request_tail_call(context, VOID_TARGET, pending, TUPLE_SIZE);
        case INTERPRETED_ENTRY:
            return turbowasm_jit_request_tail_call(context, INTERPRETED_TARGET, args, TUPLE_SIZE);
        case HOST_ENTRY:
            return turbowasm_jit_request_tail_call(context, HOST, args, TUPLE_SIZE);
        case TRAP_ENTRY:
            return turbowasm_jit_request_tail_call(context, TRAP_TARGET, args, TUPLE_SIZE);
        case ZERO_ARGS_ENTRY:
            return turbowasm_jit_request_tail_call(context, ZERO_ARGS_TARGET, NULL, 0u);
        default: return TURBOWASM_UNSUPPORTED;
    }
}
static void destroy_function(void *context, turbowasm_compiled_function *compiled) {
    (void)context;
    compiled->impl = NULL;
}
static void destroy_backend(void *context) {
    /* Static test state has no heap ownership. */
    (void)context;
    memset(function_ids, 0, sizeof(function_ids));
}
#endif

static void attach_backend(void) {
    turbowasm_jit_backend backend = {0};
#ifdef TURBOWASM_TEST_MIR
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
#else
    backend.context = function_ids;
    backend.is_function_eligible = eligible;
    backend.compile_function = compile_function;
    backend.invoke = invoke;
    backend.destroy_function = destroy_function;
    backend.destroy_backend = destroy_backend;
    backend.supports_execution_control = true;
#endif
    check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1u), TURBOWASM_OK);
    ((turbowasm_instance_impl *)instance.impl)->jit_functions[INTERPRETED_TARGET].state =
        TURBOWASM_JIT_INTERPRET_ONLY;
}

static void compare(uint32_t function, const turbowasm_value *args, size_t argc,
    const turbowasm_execution_options *options) {
    turbowasm_value expected[TUPLE_SIZE], actual[TUPLE_SIZE], untouched[TUPLE_SIZE];
    turbowasm_status expected_status, actual_status;
    turbowasm_trap expected_trap, actual_trap;
    turbowasm_instance_impl *impl = instance.impl;
    size_t expected_count, actual_count, i;
    memset(untouched, 0xa5, sizeof(untouched));
    memcpy(expected, untouched, sizeof(expected));
    memcpy(actual, untouched, sizeof(actual));
    expected_status = turbowasm_instance_invoke_with_options(&reference, function,
        args, argc, expected, TUPLE_SIZE, &expected_count, &expected_trap, options);
    actual_status = turbowasm_instance_invoke_with_options(&instance, function,
        args, argc, actual, TUPLE_SIZE, &actual_count, &actual_trap, options);
    check_equal(actual_status, expected_status);
    check_equal(actual_trap, expected_trap);
    check_equal(actual_count, expected_count);
    for (i = 0u; i < actual_count; ++i) {
        size_t bytes = i == 0u || i == 2u ? sizeof(uint32_t) : sizeof(uint64_t);
        check_equal(actual[i].kind, expected[i].kind);
        check_equal(memcmp(&actual[i].as, &expected[i].as, bytes), 0);
    }
    if (actual_status != TURBOWASM_OK)
        check_equal(memcmp(actual, untouched, sizeof(actual)), 0);
    check_equal(impl->jit_functions[function].state, TURBOWASM_JIT_COMPILED);
    check_not_null(impl->jit_functions[function].compiled.impl);
}

spec("scalar tail transfers") {
    before_each() {
        turbowasm_runtime_config config;
        const turbowasm_host_function_type type = {kinds, TUPLE_SIZE, kinds, TUPLE_SIZE};
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        check_equal(turbowasm_module_load_borrowed_with_config(&module,
            scalar_tail_calls_bytes, sizeof(scalar_tail_calls_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"host", 4u},
            (turbowasm_name){(const uint8_t *)"tuple", 5u}, &type, host_tuple, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&reference, &module, &linker), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
        attach_backend();
    }
    after_each() {
        allocations.fail_at = 0u;
        turbowasm_instance_destroy(&instance);
        turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker);
        turbowasm_module_destroy(&module);
        check_equal(allocations.live, (size_t)0);
    }
    it("replaces mixed-arity frames across long chains and preserves scalar bits") {
        turbowasm_value args[TUPLE_SIZE], saved[TUPLE_SIZE];
        turbowasm_instance_impl *impl = instance.impl;
        make_tuple(args, CHAIN_LENGTH);
        memcpy(saved, args, sizeof(saved));
        compare(LEFT, args, TUPLE_SIZE, NULL);
        check_equal(memcmp(args, saved, sizeof(args)), 0);
        compare(ENTRY, args, 1u, NULL);
        check_equal(impl->jit_functions[RIGHT].state, TURBOWASM_JIT_COMPILED);
    }
    it("handles host re-entry, interpreter targets, zero arguments and traps") {
        turbowasm_value args[TUPLE_SIZE];
        make_tuple(args, 2);
        compare(INTERPRETED_ENTRY, args, TUPLE_SIZE, NULL);
        compare(HOST_ENTRY, args, TUPLE_SIZE, NULL);
        compare(ZERO_ARGS_ENTRY, args, TUPLE_SIZE, NULL);
        compare(TRAP_ENTRY, args, TUPLE_SIZE, NULL);
    }
    it("accepts a null result array for an empty result tuple") {
        turbowasm_trap trap;
        size_t count;
        compare(VOID_ENTRY, NULL, 0u, NULL);
        check_equal(turbowasm_instance_invoke(&instance, VOID_ENTRY, NULL, 0u,
            NULL, 0u, &count, &trap), TURBOWASM_OK);
        check_equal(count, (size_t)0);
        check_equal(trap, TURBOWASM_TRAP_NONE);
    }
    it("cleans up every allocation failure during a growing tail chain") {
        turbowasm_value args[TUPLE_SIZE], result[TUPLE_SIZE], saved[TUPLE_SIZE];
        turbowasm_trap trap;
        size_t count, baseline, attempts, point;
        make_tuple(args, 1);
        compare(ENTRY, args, 1u, NULL);
        baseline = allocations.live;
        allocations.attempts = 0u;
        check_equal(turbowasm_instance_invoke(&instance, ENTRY, args, 1u,
            result, TUPLE_SIZE, &count, &trap), TURBOWASM_OK);
        attempts = allocations.attempts;
        check_true(attempts != 0u);
        for (point = 1u; point <= attempts; ++point) {
            memset(result, 0xa5, sizeof(result));
            memcpy(saved, result, sizeof(saved));
            allocations.attempts = 0u;
            allocations.fail_at = point;
            check_equal(turbowasm_instance_invoke(&instance, ENTRY, args, 1u,
                result, TUPLE_SIZE, &count, &trap), TURBOWASM_OUT_OF_MEMORY);
            allocations.fail_at = 0u;
            check_equal(count, (size_t)0);
            check_equal(trap, TURBOWASM_TRAP_NONE);
            check_equal(memcmp(result, saved, sizeof(result)), 0);
            check_equal(allocations.live, baseline);
        }
        compare(ENTRY, args, 1u, NULL);
        check_equal(allocations.live, baseline);
    }
#ifdef TURBOWASM_TEST_MIR
    it("matches interpreter fuel boundaries through native and host tail targets") {
        const uint32_t functions[] = {LEFT, ENTRY, INTERPRETED_ENTRY, HOST_ENTRY,
            TRAP_ENTRY, ZERO_ARGS_ENTRY, VOID_ENTRY};
        turbowasm_value args[TUPLE_SIZE];
        turbowasm_execution_options options = {0};
        size_t i;
        uint32_t fuel;
        make_tuple(args, 1);
        options.has_fuel_limit = true;
        for (i = 0u; i < sizeof(functions) / sizeof(functions[0]); ++i) {
            uint32_t function = functions[i];
            size_t argc = function == VOID_ENTRY ? 0u : function == ENTRY ? 1u : TUPLE_SIZE;
            for (fuel = 0u; fuel < FUEL_BOUNDARIES; ++fuel) {
                options.fuel = fuel;
                compare(function, argc == 0u ? NULL : args, argc, &options);
            }
        }
    }
#endif
}
