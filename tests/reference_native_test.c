#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_reference_helper.h"
#include "jit_table_helper.h"
#include "fixtures/reference_native.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { RESULT_CAPACITY = 4, ROOT_LIMIT = 32, OBJECT_LIMIT = 64, CHAIN_LENGTH = 1024,
    FUEL_BOUNDARIES = 18, CALL_DEPTH_LIMIT = 256 };
static turbowasm_module module;
static turbowasm_store store, foreign_store;
static turbowasm_instance instance, reference, foreign_instance;
static turbowasm_linker linker;
static turbowasm_root roots[2], extra_roots[ROOT_LIMIT];
static turbowasm_value nodes[2];
static struct { size_t live, attempts, fail_at; } allocations;
static unsigned collections;
static bool reenter;

typedef struct call_result {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value values[RESULT_CAPACITY];
} call_result;

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (++allocations.attempts == allocations.fail_at)
        return NULL;
    pointer = malloc(size);
    if (pointer != NULL) ++allocations.live;
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
static uint32_t function_index(const char *name) {
    size_t i, length = strlen(name);
    for (i = 0u; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *desc = turbowasm_module_export_at(&module, i);
        if (desc->name.size == length && memcmp(desc->name.bytes, name, length) == 0)
            return desc->item_index;
    }
    check(false, "missing function %s", name);
    return UINT32_MAX;
}
static void compiled(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t index = function_index(name);
    turbowasm_instance_impl *impl = instance.impl;
    check(impl->jit_functions[index].state == TURBOWASM_JIT_COMPILED,
        "%s must compile; state %d", name, (int)impl->jit_functions[index].state);
    check_not_null(impl->jit_functions[index].compiled.impl);
#else
    (void)name;
#endif
}
static turbowasm_value integer(int64_t bits, bool wide) {
    turbowasm_value value = {0};
    value.kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
    if (wide) value.as.i64 = bits;
    else value.as.i32 = (int32_t)bits;
    return value;
}
static call_result invoke(turbowasm_instance *target, const char *name,
    const turbowasm_value *args, size_t argc, const turbowasm_execution_options *options) {
    call_result result;
    memset(&result, 0xa5, sizeof(result));
    result.status = turbowasm_instance_invoke_with_options(target, function_index(name),
        args, argc, result.values, RESULT_CAPACITY, &result.count, &result.trap, options);
    return result;
}
static call_result run(const char *name, const turbowasm_value *args, size_t argc) {
    call_result result = invoke(&instance, name, args, argc, NULL);
    check_equal(result.status, TURBOWASM_OK);
    check_equal(result.trap, TURBOWASM_TRAP_NONE);
    compiled(name);
    return result;
}
static void same_reference(turbowasm_value actual, turbowasm_value expected) {
    check_equal(actual.kind, expected.kind);
    switch (actual.kind) {
        case TURBOWASM_VALUE_GCREF:
        case TURBOWASM_VALUE_MANAGED_EXTERNREF:
            check_true(actual.as.gcref.store == expected.as.gcref.store);
            check_equal(actual.as.gcref.handle, expected.as.gcref.handle);
            break;
        case TURBOWASM_VALUE_EXTERNREF:
            check_equal(actual.as.externref.is_null, expected.as.externref.is_null);
            if (!actual.as.externref.is_null) check_equal(actual.as.externref.token, expected.as.externref.token);
            break;
        case TURBOWASM_VALUE_FUNCREF:
            check_equal(actual.as.funcref.is_null, expected.as.funcref.is_null);
            if (!actual.as.funcref.is_null) {
                check_equal(actual.as.funcref.function_index, expected.as.funcref.function_index);
                check_true(actual.as.funcref.owner == expected.as.funcref.owner);
            }
            break;
        case TURBOWASM_VALUE_EXNREF:
            check_equal(actual.as.exnref.is_null, expected.as.exnref.is_null);
            check_true(actual.as.exnref.exception == expected.as.exnref.exception);
            break;
        default: check(false, "unexpected reference kind");
    }
}
static turbowasm_status host_collect(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)results; (void)capacity;
    check_equal(argc, (size_t)0);
    ++collections;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    if (reenter) {
        call_result nested = invoke(&instance, "id", nodes, 1u, NULL);
        check_equal(nested.status, TURBOWASM_OK);
        same_reference(nested.values[0], nodes[0]);
    }
    *count = 0u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static turbowasm_value make_node(turbowasm_instance *target, int32_t payload) {
    turbowasm_value arg = integer(payload, false);
    call_result result = invoke(target, "make", &arg, 1u, NULL);
    check_equal(result.status, TURBOWASM_OK);
    check_equal(result.count, (size_t)1);
    return result.values[0];
}
static int32_t read_node(turbowasm_value node) {
    call_result result = invoke(&reference, "read", &node, 1u, NULL);
    check_equal(result.status, TURBOWASM_OK);
    return result.values[0].as.i32;
}
static bool interrupt_after_collection(void *context) {
    unsigned *checks = context;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    return ++*checks == 3u;
}

spec("native reference frame") {
    before_each() {
        turbowasm_runtime_config runtime;
        turbowasm_store_config config;
        turbowasm_host_function_type type = {0};
        memset(&allocations, 0, sizeof(allocations));
        collections = 0u; reenter = false;
        turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = allocate;
        runtime.allocator.deallocate = deallocate;
        turbowasm_store_config_init(&config);
        config.runtime = runtime;
        config.max_objects = OBJECT_LIMIT;
        config.max_roots = ROOT_LIMIT;
        check_equal(turbowasm_store_create(&store, &config), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed_with_config(&module,
            reference_native_bytes, sizeof(reference_native_bytes), &runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1u},
            (turbowasm_name){(const uint8_t *)"collect", 7u}, &type, host_collect, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&instance, &module, &linker, &store), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&reference, &module, &linker, &store), TURBOWASM_OK);
        nodes[0] = make_node(&instance, 42);
        check_equal(turbowasm_root_retain(&store, &nodes[0], &roots[0]), TURBOWASM_OK);
        nodes[1] = make_node(&instance, 13);
        check_equal(turbowasm_root_retain(&store, &nodes[1], &roots[1]), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            turbowasm_jit_backend backend = {0};
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1u), TURBOWASM_OK);
        }
#endif
    }
    after_each() {
        size_t i;
        allocations.fail_at = 0u;
        for (i = 0u; i < ROOT_LIMIT; ++i)
            if (extra_roots[i].impl != NULL) check_equal(turbowasm_root_release(&extra_roots[i]), TURBOWASM_OK);
        for (i = 0u; i < 2u; ++i)
            if (roots[i].impl != NULL) check_equal(turbowasm_root_release(&roots[i]), TURBOWASM_OK);
        turbowasm_instance_destroy(&foreign_instance);
        if (foreign_store.impl != NULL) check_equal(turbowasm_store_destroy(&foreign_store), TURBOWASM_OK);
        turbowasm_instance_destroy(&instance);
        turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker);
        turbowasm_module_destroy(&module);
        check_equal(turbowasm_store_destroy(&store), TURBOWASM_OK);
        check_equal(allocations.live, (size_t)0);
    }
    it("copies reference locals, branches, selects and direct/tail call results") {
        const char *identities[] = {"id", "local", "br", "call", "tail", "non-null"};
        turbowasm_value args[3];
        call_result result;
        size_t i;
        for (i = 0u; i < sizeof(identities) / sizeof(identities[0]); ++i) {
            result = run(identities[i], nodes, 1u);
            same_reference(result.values[0], nodes[0]);
        }
        args[0] = nodes[0]; args[1] = nodes[1]; args[2] = integer(1, false);
        result = run("select", args, 3u); same_reference(result.values[0], nodes[0]);
        args[2] = integer(0, false);
        result = run("select", args, 3u); same_reference(result.values[0], nodes[1]);
        args[0] = integer(1, false); args[1] = nodes[0]; args[2] = nodes[1];
        result = run("branch", args, 3u); same_reference(result.values[0], nodes[0]);
        args[0] = integer(0, false);
        result = run("branch", args, 3u); same_reference(result.values[0], nodes[1]);
    }
    it("preserves managed externrefs, function owners, nulls and identity tests") {
        turbowasm_value args[3], nil;
        call_result result = run("function", NULL, 0u);
        args[0] = nodes[0]; args[1] = nodes[1];
        args[1].kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
        args[2] = result.values[0];
        check_true(args[2].as.funcref.owner == instance.impl);
        check_equal(args[2].as.funcref.function_index, function_index("one"));
        result = run("tuple", args, 3u);
        check_equal(result.count, (size_t)3);
        same_reference(result.values[0], args[2]);
        same_reference(result.values[1], args[0]);
        same_reference(result.values[2], args[1]);
        result = run("null", NULL, 0u);
        check_equal(result.count, (size_t)RESULT_CAPACITY);
        nil = result.values[0];
        check_equal(nil.as.gcref.handle, UINT64_C(0));
        check_true(result.values[1].as.externref.is_null);
        check_true(result.values[2].as.funcref.is_null);
        check_true(result.values[3].as.exnref.is_null);
        result = run("is-null", &nil, 1u); check_equal(result.values[0].as.i32, 1);
        result = run("is-null", nodes, 1u); check_equal(result.values[0].as.i32, 0);
        result = run("equal", nodes, 2u); check_equal(result.values[0].as.i32, 0);
        args[0] = args[1] = nodes[0];
        result = run("equal", args, 2u); check_equal(result.values[0].as.i32, 1);
        result = invoke(&instance, "non-null", &nil, 1u, NULL);
        check_equal(result.status, TURBOWASM_TRAPPED);
        check_equal(result.trap, TURBOWASM_TRAP_NULL_REFERENCE);
        check_equal(result.count, (size_t)0);
    }
    it("roots native-only values during host collection and nested re-entry") {
        turbowasm_value arg = integer(7, false);
        call_result result;
        reenter = true;
        result = run("live-local", &arg, 1u);
        check_equal(read_node(result.values[0]), 7);
        check_equal(collections, 1u);
        arg = integer(12, false);
        result = run("loop", &arg, 1u);
        check_equal(read_node(result.values[0]), 12);
        check_equal(collections, 13u);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        {
            turbowasm_store_stats stats;
            check_equal(turbowasm_store_get_stats(&store, &stats), TURBOWASM_OK);
            check_equal(stats.objects, 2u);
        }
    }
    it("roots long tail handoffs and resets reference locals on self-tail entry") {
        turbowasm_value args[4];
        call_result result = run("function", NULL, 0u);
        args[0] = integer(CHAIN_LENGTH, false); args[1] = nodes[0];
        args[2] = nodes[1]; args[2].kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
        args[3] = result.values[0];
        result = run("tail-a", args, 4u);
        check_equal(result.count, (size_t)3);
        same_reference(result.values[0], args[1]);
        same_reference(result.values[1], args[2]);
        same_reference(result.values[2], args[3]);
        check_equal(collections, (unsigned)CHAIN_LENGTH);
        result = run("self", args, 2u);
        check_equal(result.values[0].as.gcref.handle, UINT64_C(0));
    }
    it("gets, sets, fills and grows reference tables with both address widths") {
        const char *names[2][4] = {{"get32", "set32", "fill32", "grow32"},
                                  {"get64", "set64", "fill64", "grow64"}};
        unsigned wide;
        for (wide = 0u; wide < 2u; ++wide) {
            turbowasm_value args[3];
            call_result result;
            args[0] = integer(1, wide != 0u); args[1] = nodes[0];
            run(names[wide][1], args, 2u);
            result = run(names[wide][0], args, 1u); same_reference(result.values[0], nodes[0]);
            args[0] = integer(0, wide != 0u); args[1] = nodes[1]; args[2] = integer(3, wide != 0u);
            run(names[wide][2], args, 3u);
            result = run(names[wide][0], args, 1u); same_reference(result.values[0], nodes[1]);
            args[0] = nodes[0]; args[1] = integer(2, wide != 0u);
            result = run(names[wide][3], args, 2u);
            if (wide) check_equal(result.values[0].as.i64, INT64_C(4));
            else check_equal(result.values[0].as.i32, 4);
            args[0] = integer(5, wide != 0u);
            result = run(names[wide][0], args, 1u); same_reference(result.values[0], nodes[0]);
            args[0] = nodes[0]; args[1] = integer(-1, wide != 0u);
            result = run(names[wide][3], args, 2u);
            if (wide) check_equal(result.values[0].as.i64, INT64_C(-1));
            else check_equal(result.values[0].as.i32, -1);
            args[0] = integer(wide ? INT64_C(1) << 32 : -1, wide != 0u);
            result = invoke(&instance, names[wide][0], args, 1u, NULL);
            check_equal(result.status, TURBOWASM_TRAPPED);
            check_equal(result.trap, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        }
        {
            turbowasm_value arg = integer(1, true);
            call_result result;
            run("set-function", &arg, 1u);
            result = run("get-function", &arg, 1u);
            check_true(result.values[0].as.funcref.owner == instance.impl);
            check_equal(result.values[0].as.funcref.function_index, function_index("one"));
        }
    }
    it("rejects null, stale, foreign-store and wrong nominal argument types") {
        turbowasm_value bad = {0};
        call_result result;
        bad.kind = TURBOWASM_VALUE_GCREF;
        result = invoke(&instance, "id", &bad, 1u, NULL);
        check_equal(result.status, TURBOWASM_TYPE_MISMATCH); compiled("id");
        result = invoke(&instance, "other", NULL, 0u, NULL); check_equal(result.status, TURBOWASM_OK);
        bad = result.values[0];
        result = invoke(&instance, "id", &bad, 1u, NULL); check_equal(result.status, TURBOWASM_TYPE_MISMATCH);
        bad = make_node(&instance, 9);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        result = invoke(&instance, "id", &bad, 1u, NULL); check_equal(result.status, TURBOWASM_TYPE_MISMATCH);
        check_equal(turbowasm_store_create(&foreign_store, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&foreign_instance, &module, &linker, &foreign_store), TURBOWASM_OK);
        bad = make_node(&foreign_instance, 4);
        result = invoke(&instance, "id", &bad, 1u, NULL); check_equal(result.status, TURBOWASM_TYPE_MISMATCH);
    }
    it("checks fuel before reference table mutations and preserves failed fill ranges") {
        const char *names[2][2] = {{"set32", "fill32"}, {"set64", "fill64"}};
        turbowasm_execution_options options = {0};
        unsigned wide, operation, fuel, index;
        options.has_fuel_limit = true;
        for (wide = 0u; wide < 2u; ++wide) {
            uint32_t table = wide ? 0u : 1u;
            for (operation = 0u; operation < 2u; ++operation) {
                for (fuel = 0u; fuel < FUEL_BOUNDARIES; ++fuel) {
                    turbowasm_value args[] = {integer(1, wide != 0u), nodes[1], integer(2, wide != 0u)};
                    call_result actual, expected;
                    check_equal(turbowasm_instance_table_fill(instance.impl, table, 0u, nodes[0], 4u), TURBOWASM_OK);
                    check_equal(turbowasm_instance_table_fill(reference.impl, table, 0u, nodes[0], 4u), TURBOWASM_OK);
                    options.fuel = fuel;
                    actual = invoke(&instance, names[wide][operation], args, operation ? 3u : 2u, &options);
                    expected = invoke(&reference, names[wide][operation], args, operation ? 3u : 2u, &options);
                    check_equal(actual.status, expected.status);
                    check_equal(actual.trap, expected.trap);
                    check_equal(actual.count, expected.count);
                    compiled(names[wide][operation]);
                    for (index = 0u; index < 4u; ++index) {
                        turbowasm_value a, b;
                        check_equal(turbowasm_instance_table_get_value(instance.impl, table, index, &a), TURBOWASM_OK);
                        check_equal(turbowasm_instance_table_get_value(reference.impl, table, index, &b), TURBOWASM_OK);
                        same_reference(a, b);
                    }
                }
            }
            {
                turbowasm_value args[] = {integer(3, wide != 0u), nodes[1], integer(2, wide != 0u)};
                call_result result = invoke(&instance, names[wide][1], args, 3u, NULL);
                turbowasm_value last;
                check_equal(result.status, TURBOWASM_TRAPPED);
                check_equal(result.trap, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
                check_equal(turbowasm_instance_table_get_value(instance.impl, table, 3u, &last), TURBOWASM_OK);
                same_reference(last, nodes[0]);
            }
        }
    }
    it("preserves fuel/trap parity and releases frame registrations after interruption") {
        const char *names[] = {"local", "call", "tail", "br", "non-null"};
        turbowasm_execution_options options = {0};
        turbowasm_store_stats before, after;
        size_t i;
        uint32_t fuel;
        check_equal(turbowasm_store_get_stats(&store, &before), TURBOWASM_OK);
        options.has_fuel_limit = true;
        for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
            for (fuel = 0u; fuel < FUEL_BOUNDARIES; ++fuel) {
                call_result actual, expected;
                options.fuel = fuel;
                expected = invoke(&reference, names[i], nodes, 1u, &options);
                actual = invoke(&instance, names[i], nodes, 1u, &options);
                check_equal(actual.status, expected.status);
                check_equal(actual.trap, expected.trap);
                check_equal(actual.count, expected.count);
                if (actual.count) same_reference(actual.values[0], expected.values[0]);
                compiled(names[i]);
            }
        }
        check_equal(turbowasm_store_get_stats(&store, &after), TURBOWASM_OK);
        check_equal(after.bytes, before.bytes);
        options.has_fuel_limit = false;
        options.should_interrupt = interrupt_after_collection;
        {
            unsigned actual_checks = 0u, expected_checks = 0u;
            call_result actual, expected;
            options.interrupt_context = &expected_checks;
            expected = invoke(&reference, "local", nodes, 1u, &options);
            options.interrupt_context = &actual_checks;
            actual = invoke(&instance, "local", nodes, 1u, &options);
            check_equal(actual.status, TURBOWASM_INTERRUPTED);
            check_equal(actual.status, expected.status);
            check_equal(actual.trap, expected.trap);
            check_equal(actual.count, (size_t)0);
            check_equal(actual_checks, expected_checks);
            check_equal(turbowasm_store_get_stats(&store, &after), TURBOWASM_OK);
            check_equal(after.bytes, before.bytes);
        }
    }
    it("enforces the logical call depth before entering compiled reference code") {
        turbowasm_jit_invocation_context context = {0};
        turbowasm_value results[RESULT_CAPACITY];
        size_t count = RESULT_CAPACITY;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        run("id", nodes, 1u);
        context.instance = instance.impl;
        context.depth = CALL_DEPTH_LIMIT - 1u;
        check_equal(turbowasm_jit_direct_call(&context, function_index("id"),
            nodes, 1u, results, RESULT_CAPACITY, &count, &trap), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_CALL_STACK_EXHAUSTED);
        check_equal(count, (size_t)0);
    }
    it("reports root-budget exhaustion and recovers after a root is released") {
        size_t i;
        call_result result;
        run("id", nodes, 1u);
        for (i = 0u; i < ROOT_LIMIT - 2u; ++i)
            check_equal(turbowasm_root_retain(&store, nodes, &extra_roots[i]), TURBOWASM_OK);
        result = invoke(&instance, "id", nodes, 1u, NULL);
        check_equal(result.status, TURBOWASM_OUT_OF_MEMORY);
        check_equal(result.count, (size_t)0);
        check_equal(turbowasm_root_release(&extra_roots[0]), TURBOWASM_OK);
        result = run("id", nodes, 1u); same_reference(result.values[0], nodes[0]);
    }
    it("cleans up every allocation failure in nested reference calls") {
        size_t live, attempts, point;
        call_result result;
        turbowasm_store_stats before, after;
        run("call", nodes, 1u);
        live = allocations.live;
        check_equal(turbowasm_store_get_stats(&store, &before), TURBOWASM_OK);
        allocations.attempts = 0u;
        run("call", nodes, 1u);
        attempts = allocations.attempts;
        check_true(attempts != 0u);
        for (point = 1u; point <= attempts; ++point) {
            allocations.attempts = 0u; allocations.fail_at = point;
            result = invoke(&instance, "call", nodes, 1u, NULL);
            allocations.fail_at = 0u;
            check_equal(result.status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(result.count, (size_t)0);
            check_equal(allocations.live, live);
            check_equal(turbowasm_store_get_stats(&store, &after), TURBOWASM_OK);
            check_equal(after.bytes, before.bytes);
        }
        result = run("call", nodes, 1u); same_reference(result.values[0], nodes[0]);
    }
}
