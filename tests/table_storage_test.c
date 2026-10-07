#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_table_helper.h"
#include "fixtures/table_storage.h"
#include "fixtures/table_storage_import.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { SIZE_WIDE, SIZE_NARROW, INIT_WIDE, INIT_NARROW, DROP_ELEMENT,
    COPY_WIDE, COPY_NARROW, COPY_WIDE_NARROW, COPY_NARROW_WIDE,
    COPY_SECOND_WIDE, BRANCH_SIZE, ONE, TWO };
enum { TABLE_COUNT = 3, INITIAL_SIZE = 8, ARGUMENTS = 3, FUEL_BOUNDARIES = 8 };
static turbowasm_module module, import_module;
static turbowasm_instance instance, reference, imported, imported_reference;
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
static void attach_backend(turbowasm_instance *target) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(target->impl, &backend, 1u), TURBOWASM_OK);
#else
    (void)target;
#endif
}
static void require_compiled(turbowasm_instance *target, uint32_t function) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_instance_impl *impl = target->impl;
    check(impl->jit_functions[function].state == TURBOWASM_JIT_COMPILED,
        "function %u must compile; state %d", function,
        (int)impl->jit_functions[function].state);
    check_not_null(impl->jit_functions[function].compiled.impl);
#else
    (void)target; (void)function;
#endif
}
static void seed(turbowasm_instance *target) {
    uint32_t table, index;
    for (table = 0u; table < TABLE_COUNT; ++table) {
        for (index = 0u; index < INITIAL_SIZE; ++index) {
            turbowasm_value value = {0};
            value.kind = TURBOWASM_VALUE_FUNCREF;
            value.as.funcref.is_null = index % ARGUMENTS == 2u;
            value.as.funcref.function_index = index % ARGUMENTS == 0u ? ONE : TWO;
            check_equal(turbowasm_instance_table_set64(target, table, index, value), TURBOWASM_OK);
        }
    }
}
static void compare_tables(void) {
    uint32_t table;
    for (table = 0u; table < TABLE_COUNT; ++table) {
        uint64_t size, expected_size, index;
        check_equal(turbowasm_instance_table_size64(&instance, table, &size), TURBOWASM_OK);
        check_equal(turbowasm_instance_table_size64(&reference, table, &expected_size), TURBOWASM_OK);
        check_equal(size, expected_size);
        for (index = 0u; index < size; ++index) {
            turbowasm_value actual, expected;
            check_equal(turbowasm_instance_table_get64(&instance, table, index, &actual), TURBOWASM_OK);
            check_equal(turbowasm_instance_table_get64(&reference, table, index, &expected), TURBOWASM_OK);
            check_equal(actual.kind, expected.kind);
            check_equal(actual.as.funcref.is_null, expected.as.funcref.is_null);
            if (!actual.as.funcref.is_null) {
                check_equal(actual.as.funcref.function_index, expected.as.funcref.function_index);
                check_true(actual.as.funcref.owner == instance.impl);
                check_true(expected.as.funcref.owner == reference.impl);
            }
        }
    }
    check_equal(((turbowasm_instance_impl *)instance.impl)->element_segment_dropped[0],
        ((turbowasm_instance_impl *)reference.impl)->element_segment_dropped[0]);
}
static size_t make_arguments(uint32_t function, uint64_t a, uint64_t b,
    uint64_t c, turbowasm_value *arguments) {
    uint64_t bits[ARGUMENTS] = {a, b, c};
    bool wide[ARGUMENTS] = {false, false, false};
    size_t i;
    memset(arguments, 0, ARGUMENTS * sizeof(*arguments));
    if (function == SIZE_WIDE || function == SIZE_NARROW || function == DROP_ELEMENT)
        return 0u;
    wide[0] = function == INIT_WIDE || function == COPY_WIDE ||
        function == COPY_WIDE_NARROW || function == COPY_SECOND_WIDE;
    wide[1] = function == COPY_WIDE || function == COPY_NARROW_WIDE ||
        function == COPY_SECOND_WIDE;
    wide[2] = function == COPY_WIDE || function == COPY_SECOND_WIDE;
    for (i = 0u; i < ARGUMENTS; ++i) {
        arguments[i].kind = wide[i] ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
        if (wide[i])
            memcpy(&arguments[i].as.i64, &bits[i], sizeof(bits[i]));
        else
            arguments[i].as.i32 = (int32_t)(uint32_t)bits[i];
    }
    return function == BRANCH_SIZE ? 1u : ARGUMENTS;
}
static void compare_call(uint32_t function, uint64_t a, uint64_t b, uint64_t c,
    const turbowasm_execution_options *options) {
    turbowasm_value args[ARGUMENTS], result, expected, untouched;
    size_t argc = make_arguments(function, a, b, c, args), count, expected_count;
    turbowasm_trap trap, expected_trap;
    turbowasm_status status, expected_status;
    memset(&untouched, 0xa5, sizeof(untouched));
    result = expected = untouched;
    expected_status = turbowasm_instance_invoke_with_options(&reference, function,
        argc != 0u ? args : NULL, argc, &expected, 1u, &expected_count, &expected_trap, options);
    status = turbowasm_instance_invoke_with_options(&instance, function,
        argc != 0u ? args : NULL, argc, &result, 1u, &count, &trap, options);
    check_equal(status, expected_status);
    check_equal(trap, expected_trap);
    check_equal(count, expected_count);
    if (count != 0u) {
        check_equal(result.kind, expected.kind);
        if (result.kind == TURBOWASM_VALUE_I64)
            check_equal(result.as.i64, expected.as.i64);
        else
            check_equal(result.as.i32, expected.as.i32);
    } else {
        check_equal(memcmp(&result, &untouched, sizeof(result)), 0);
    }
    compare_tables();
    require_compiled(&instance, function);
}
static void create_import(turbowasm_instance *out, turbowasm_instance *provider) {
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_instance(&linker,
        (turbowasm_name){(const uint8_t *)"p", 1u}, provider), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(out, &import_module, &linker), TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
}

spec("native table storage") {
    before_each() {
        turbowasm_runtime_config config;
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        check_equal(turbowasm_module_load_borrowed_with_config(&module,
            table_storage_bytes, sizeof(table_storage_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&reference, &module), TURBOWASM_OK);
        seed(&instance);
        seed(&reference);
        attach_backend(&instance);
    }
    after_each() {
        allocations.fail_at = 0u;
        turbowasm_instance_destroy(&imported);
        turbowasm_instance_destroy(&imported_reference);
        turbowasm_linker_destroy(&linker);
        turbowasm_module_destroy(&import_module);
        turbowasm_instance_destroy(&instance);
        turbowasm_instance_destroy(&reference);
        turbowasm_module_destroy(&module);
        check_equal(allocations.live, (size_t)0);
    }
    it("reads current table sizes and emits table32/table64 branch results") {
        turbowasm_value nil = {0};
        uint64_t previous;
        nil.kind = TURBOWASM_VALUE_FUNCREF;
        nil.as.funcref.is_null = true;
        compare_call(SIZE_WIDE, 0, 0, 0, NULL);
        compare_call(SIZE_NARROW, 0, 0, 0, NULL);
        check_equal(turbowasm_instance_table_grow64(&instance, 0u, nil, 2u, &previous), TURBOWASM_OK);
        check_equal(previous, (uint64_t)INITIAL_SIZE);
        check_equal(turbowasm_instance_table_grow64(&reference, 0u, nil, 2u, &previous), TURBOWASM_OK);
        compare_call(SIZE_WIDE, 0, 0, 0, NULL);
        compare_call(BRANCH_SIZE, 1, 0, 0, NULL);
        compare_call(BRANCH_SIZE, 0, 0, 0, NULL);
    }
    it("preserves copy overlap, mixed address widths and full-width bounds") {
        const uint64_t cases[][ARGUMENTS] = {
            {1, 0, 6}, {0, 1, 6}, {2, 2, 3}, {8, 8, 0}, {9, 8, 0},
            {0, 8, 1}, {7, 0, 2}, {UINT64_C(1) << 32, 0, 1},
            {0, UINT64_C(1) << 32, 1}, {0, 0, UINT64_C(1) << 32},
            {UINT64_C(1) << 63, 0, 1}, {0, 0, UINT64_MAX}, {UINT32_MAX, 0, 1}};
        uint32_t function;
        size_t i;
        for (function = COPY_WIDE; function <= COPY_SECOND_WIDE; ++function) {
            for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                seed(&instance); seed(&reference);
                compare_call(function, cases[i][0], cases[i][1], cases[i][2], NULL);
            }
        }
    }
    it("keeps table.init source/length narrow and consumes element drop state") {
        compare_call(INIT_WIDE, 2, 0, 3, NULL);
        compare_call(INIT_NARROW, 1, 1, 2, NULL);
        compare_call(INIT_WIDE, UINT64_C(1) << 32, 0, 1, NULL);
        compare_call(INIT_WIDE, 0, UINT32_MAX, 1, NULL);
        compare_call(INIT_WIDE, 0, 0, UINT32_MAX, NULL);
        compare_call(DROP_ELEMENT, 0, 0, 0, NULL);
        compare_call(DROP_ELEMENT, 0, 0, 0, NULL);
        compare_call(INIT_WIDE, INITIAL_SIZE, 0, 0, NULL);
        compare_call(INIT_WIDE, 0, 0, 1, NULL);
    }
    it("pays fuel before table writes and segment drops") {
        const uint32_t functions[] = {INIT_WIDE, INIT_NARROW, COPY_WIDE,
            COPY_NARROW, COPY_WIDE_NARROW, COPY_NARROW_WIDE, COPY_SECOND_WIDE,
            BRANCH_SIZE, DROP_ELEMENT};
        turbowasm_execution_options options = {0};
        size_t i;
        uint32_t fuel;
        options.has_fuel_limit = true;
        for (i = 0u; i < sizeof(functions) / sizeof(functions[0]); ++i) {
            for (fuel = 0u; fuel < FUEL_BOUNDARIES; ++fuel) {
                seed(&instance); seed(&reference);
                options.fuel = fuel;
                compare_call(functions[i], 1, 0, 2, &options);
            }
        }
    }
    it("uses imported backing and observes growth through the original owner") {
        turbowasm_value args[ARGUMENTS], result, nil = {0};
        turbowasm_trap trap;
        size_t count;
        uint64_t previous;
        check_equal(turbowasm_module_load_borrowed(&import_module,
            table_storage_import_bytes, sizeof(table_storage_import_bytes)), TURBOWASM_OK);
        create_import(&imported, &instance);
        create_import(&imported_reference, &reference);
        attach_backend(&imported);
        make_arguments(COPY_WIDE, 1, 0, 6, args);
        check_equal(turbowasm_instance_invoke(&imported, 0u, args, ARGUMENTS,
            NULL, 0u, &count, &trap), TURBOWASM_OK);
        check_equal(turbowasm_instance_invoke(&imported_reference, 0u, args, ARGUMENTS,
            NULL, 0u, &count, &trap), TURBOWASM_OK);
        compare_tables();
        require_compiled(&imported, 0u);
        nil.kind = TURBOWASM_VALUE_FUNCREF; nil.as.funcref.is_null = true;
        check_equal(turbowasm_instance_table_grow64(&instance, 0u, nil, 1u, &previous), TURBOWASM_OK);
        check_equal(turbowasm_instance_table_grow64(&reference, 0u, nil, 1u, &previous), TURBOWASM_OK);
        check_equal(turbowasm_instance_invoke(&imported, 1u, NULL, 0u,
            &result, 1u, &count, &trap), TURBOWASM_OK);
        check_equal(count, (size_t)1);
        check_equal(result.kind, TURBOWASM_VALUE_I64);
        check_equal(result.as.i64, (int64_t)(INITIAL_SIZE + 1));
        require_compiled(&imported, 1u);
        compare_tables();
    }
    it("rejects invalid helper identities and preserves unsigned table64 addresses") {
        turbowasm_jit_invocation_context context = {0};
        context.instance = instance.impl;
        check_equal(turbowasm_jit_table(&context, TURBOWASM_JIT_TABLE_SIZE, 0, 0, 0, 0, 0, NULL),
            (int64_t)INITIAL_SIZE);
        check_equal(context.call_status, TURBOWASM_OK);
        turbowasm_jit_table(&context, TURBOWASM_JIT_TABLE_COPY, 0, 0,
            INT64_MIN, 0, 1, NULL);
        check_equal(context.call_status, TURBOWASM_TRAPPED);
        check_equal(context.call_trap, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        turbowasm_jit_table(&context, TURBOWASM_JIT_TABLE_COPY, 0, TABLE_COUNT, 0, 0, 0, NULL);
        check_equal(context.call_status, TURBOWASM_INVALID_ARGUMENT);
        check_equal(context.call_trap, TURBOWASM_TRAP_NONE);
        turbowasm_jit_table(&context, TURBOWASM_JIT_TABLE_SIZE, -1, 0, 0, 0, 0, NULL);
        check_equal(context.call_status, TURBOWASM_INVALID_ARGUMENT);
        turbowasm_jit_table(&context, TURBOWASM_JIT_TABLE_SIZE, INT64_C(1) << 32, 0, 0, 0, 0, NULL);
        check_equal(context.call_status, TURBOWASM_INVALID_ARGUMENT);
        compare_tables();
    }
    it("runs initialization, copy and drop through the portable helper boundary") {
        const struct {
            turbowasm_jit_table_opcode opcode;
            uint32_t function;
            int64_t a, b, c;
        } steps[] = {
            {TURBOWASM_JIT_TABLE_INIT, INIT_WIDE, 1, 0, 3},
            {TURBOWASM_JIT_TABLE_COPY, COPY_WIDE, 2, 1, 3},
            {TURBOWASM_JIT_ELEMENT_DROP, DROP_ELEMENT, 0, 0, 0},
            {TURBOWASM_JIT_TABLE_INIT, INIT_WIDE, 0, 0, 1}};
        turbowasm_jit_invocation_context context = {0};
        size_t i;
        context.instance = instance.impl;
        for (i = 0u; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            turbowasm_value args[ARGUMENTS];
            turbowasm_trap trap;
            turbowasm_status status;
            size_t argc = make_arguments(steps[i].function,
                (uint64_t)steps[i].a, (uint64_t)steps[i].b, (uint64_t)steps[i].c, args);
            size_t count;
            status = turbowasm_instance_invoke(&reference, steps[i].function,
                argc == 0u ? NULL : args, argc, NULL, 0u, &count, &trap);
            turbowasm_jit_table(&context, steps[i].opcode, 0, 0,
                steps[i].a, steps[i].b, steps[i].c, NULL);
            check_equal(context.call_status, status);
            check_equal(context.call_trap, trap);
            check_equal(count, (size_t)0);
            compare_tables();
        }
    }
    it("leaves table contents unchanged on every initialization allocation failure") {
        turbowasm_value args[ARGUMENTS];
        turbowasm_trap trap;
        size_t count, attempts, live, point;
        make_arguments(INIT_WIDE, 1, 0, 3, args);
        compare_call(INIT_WIDE, 1, 0, 3, NULL);
        seed(&instance); seed(&reference);
        live = allocations.live;
        allocations.attempts = 0u;
        check_equal(turbowasm_instance_invoke(&instance, INIT_WIDE, args, ARGUMENTS,
            NULL, 0u, &count, &trap), TURBOWASM_OK);
        attempts = allocations.attempts;
        check_true(attempts != 0u);
        seed(&instance);
        for (point = 1u; point <= attempts; ++point) {
            allocations.attempts = 0u;
            allocations.fail_at = point;
            check_equal(turbowasm_instance_invoke(&instance, INIT_WIDE, args, ARGUMENTS,
                NULL, 0u, &count, &trap), TURBOWASM_OUT_OF_MEMORY);
            allocations.fail_at = 0u;
            check_equal(count, (size_t)0);
            check_equal(trap, TURBOWASM_TRAP_NONE);
            check_equal(allocations.live, live);
            compare_tables();
        }
        compare_call(INIT_WIDE, 1, 0, 3, NULL);
    }
}
