#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_reference_helper.h"
#include "jit_table_helper.h"
#include "fixtures/reference_native.h"
#include "fixtures/global_imports.h"
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
static void same_value(turbowasm_value actual, turbowasm_value expected) {
    check_equal(actual.kind, expected.kind);
    if (actual.kind == TURBOWASM_VALUE_V128) {
        check_equal(actual.as.v128.shape, expected.as.v128.shape);
        check_equal(memcmp(&actual.as.v128.bits, &expected.as.v128.bits, sizeof(actual.as.v128.bits)), 0);
    } else if (actual.kind == TURBOWASM_VALUE_I32 || actual.kind == TURBOWASM_VALUE_F32)
        check_equal(memcmp(&actual.as, &expected.as, sizeof(uint32_t)), 0);
    else if (actual.kind == TURBOWASM_VALUE_I64 || actual.kind == TURBOWASM_VALUE_F64)
        check_equal(memcmp(&actual.as, &expected.as, sizeof(uint64_t)), 0);
    else same_reference(actual, expected);
}
static call_result compare_control(const char *name, const turbowasm_value *args, size_t count,
    const turbowasm_execution_options *options) {
    call_result expected = invoke(&reference, name, args, count, options);
    call_result actual = invoke(&instance, name, args, count, options);
    size_t i;
    check_equal(actual.status, expected.status); check_equal(actual.trap, expected.trap);
    check_equal(actual.count, expected.count);
    for (i = 0; i < actual.count; ++i) same_value(actual.values[i], expected.values[i]);
    compiled(name);
    return actual;
}
static turbowasm_value vector_value(void) {
    const uint64_t bits[] = {UINT64_C(0x7ff8000000001234), UINT64_C(0x8000000000000000)};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128; value.as.v128.shape = TURBOWASM_V128_F64X2;
    cmeta_simd_v128_load(&value.as.v128.bits, bits);
    return value;
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
    it("preserves nullable branch edges at block, function and loop targets") {
        const char *names[] = {"br-null", "br-non-null", "return-null", "return-non-null", "loop-null", "loop-non-null"};
        unsigned choice, name, fuel;
        for (choice = 0; choice < 2; ++choice) {
            turbowasm_value selected = choice ? nodes[0] : (turbowasm_value){.kind = TURBOWASM_VALUE_GCREF};
            for (name = 0; name < sizeof(names) / sizeof(*names); ++name) {
                turbowasm_value args[] = {selected, nodes[1]};
                size_t count = name >= 4 ? 2 : 1;
                turbowasm_execution_options options = {0};
                call_result result;
                if (name == 5) { args[0] = integer(0, false); args[1] = selected; }
                result = compare_control(names[name], args, count, NULL);
                check_equal(result.status, TURBOWASM_OK);
                if (name == 0 || name == 2) check_equal(result.values[0].as.i32, choice ? 22 : 11);
                else if (name == 4) check_equal(result.values[0].as.i32, choice ? 1 : 2);
                else same_reference(result.values[0], selected);
                options.has_fuel_limit = true;
                for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
                    options.fuel = fuel; compare_control(names[name], args, count, &options);
                }
            }
        }
    }
    it("branches on external, function and exception reference carriers") {
        turbowasm_value args[3] = {0};
        args[0].kind = TURBOWASM_VALUE_EXTERNREF;
        args[0].as.externref.token = (uintptr_t)&module;
        args[1].kind = TURBOWASM_VALUE_FUNCREF;
        args[1].as.funcref.function_index = function_index("one"); args[1].as.funcref.owner = instance.impl;
        args[2].kind = TURBOWASM_VALUE_EXNREF; args[2].as.exnref.is_null = true;
        check_equal(compare_control("br-extern", &args[0], 1, NULL).status, TURBOWASM_OK);
        check_equal(compare_control("br-function", &args[1], 1, NULL).status, TURBOWASM_OK);
        check_equal(compare_control("br-exception", &args[2], 1, NULL).status, TURBOWASM_OK);
        args[0].as.externref.is_null = true; args[1].as.funcref.is_null = true;
        check_equal(compare_control("br-extern", &args[0], 1, NULL).status, TURBOWASM_OK);
        check_equal(compare_control("br-function", &args[1], 1, NULL).status, TURBOWASM_OK);
    }
    it("reads and writes scalar, vector and reference globals without losing metadata") {
        turbowasm_value args[] = {integer(-19, false), integer(INT64_MIN, true), {0}, {0}};
        uint32_t nan = UINT32_C(0x7fc01234);
        turbowasm_value vector = vector_value();
        args[2].kind = TURBOWASM_VALUE_F32; memcpy(&args[2].as.f32, &nan, sizeof(nan));
        args[3].kind = TURBOWASM_VALUE_F64; args[3].as.f64 = -0.0;
        check_equal(compare_control("global-scalars", args, 4, NULL).status, TURBOWASM_OK);
        run("set-vector", &vector, 1); same_value(run("get-vector", NULL, 0).values[0], vector);
        args[0] = nodes[0]; args[0].kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
        args[1] = run("function", NULL, 0).values[0];
        args[2] = (turbowasm_value){0}; args[2].kind = TURBOWASM_VALUE_EXNREF; args[2].as.exnref.is_null = true;
        check_equal(compare_control("global-refs", args, 3, NULL).status, TURBOWASM_OK);
        check_equal(run("global-const", NULL, 0).values[0].as.i32, 91);
    }
    it("retains managed values in globals and leaves rejected writes unchanged") {
        turbowasm_value arg = integer(73, false), out = {0};
        turbowasm_jit_invocation_context context = {0};
        call_result result = run("global-live", &arg, 1);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        check_equal(read_node(run("get-any", NULL, 0).values[0]), 73);
        context.instance = instance.impl;
        turbowasm_jit_reference(&context, TURBOWASM_JIT_GLOBAL_SET, 0, NULL, &arg, NULL);
        check_equal(context.call_status, TURBOWASM_TYPE_MISMATCH);
        same_reference(run("get-any", NULL, 0).values[0], result.values[0]);
        turbowasm_jit_reference(&context, TURBOWASM_JIT_GLOBAL_SET, 9, NULL, &arg, NULL);
        check_equal(context.call_status, TURBOWASM_TYPE_MISMATCH);
        check_equal(run("global-const", NULL, 0).values[0].as.i32, 91);
        turbowasm_jit_reference(&context, TURBOWASM_JIT_GLOBAL_GET, UINT32_MAX, &out, NULL, NULL);
        check_equal(context.call_status, TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_store_create(&foreign_store, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&foreign_instance, &module, &linker, &foreign_store), TURBOWASM_OK);
        arg = make_node(&foreign_instance, 4);
        turbowasm_jit_reference(&context, TURBOWASM_JIT_GLOBAL_SET, 0, NULL, &arg, NULL);
        check_equal(context.call_status, TURBOWASM_TYPE_MISMATCH);
        same_reference(run("get-any", NULL, 0).values[0], result.values[0]);
    }
    it("preserves global mutation ordering at every fuel boundary") {
        const char *setters[] = {"set-any", "set-vector", "global-scalars", "global-refs"};
        turbowasm_value scalar[] = {integer(19, false), integer(INT64_MIN, true), {0}, {0}};
        turbowasm_value vector = vector_value(), refs[] = {nodes[0], {0}, {0}};
        const turbowasm_value *args[] = {&nodes[1], &vector, scalar, refs};
        const size_t counts[] = {1, 1, 4, 3};
        turbowasm_value initial[9];
        turbowasm_execution_options options = {0};
        unsigned operation, fuel, global;
        scalar[2].kind = TURBOWASM_VALUE_F32; scalar[2].as.f32 = -0.0f;
        scalar[3].kind = TURBOWASM_VALUE_F64; scalar[3].as.f64 = 0.25;
        refs[0].kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
        refs[1] = run("function", NULL, 0).values[0];
        refs[2].kind = TURBOWASM_VALUE_EXNREF; refs[2].as.exnref.is_null = true;
        for (global = 0; global < sizeof(initial) / sizeof(*initial); ++global)
            check_equal(turbowasm_instance_global_get(instance.impl, global, &initial[global]), TURBOWASM_OK);
        options.has_fuel_limit = true;
        for (operation = 0; operation < sizeof(setters) / sizeof(*setters); ++operation) {
            for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
                for (global = 0; global < sizeof(initial) / sizeof(*initial); ++global) {
                    check_equal(turbowasm_instance_global_set(instance.impl, global, initial[global]), TURBOWASM_OK);
                    check_equal(turbowasm_instance_global_set(reference.impl, global, initial[global]), TURBOWASM_OK);
                }
                options.fuel = fuel;
                compare_control(setters[operation], args[operation], counts[operation], &options);
                for (global = 0; global < sizeof(initial) / sizeof(*initial); ++global) {
                    turbowasm_value actual, expected;
                    check_equal(turbowasm_instance_global_get(instance.impl, global, &actual), TURBOWASM_OK);
                    check_equal(turbowasm_instance_global_get(reference.impl, global, &expected), TURBOWASM_OK);
                    same_value(actual, expected);
                }
            }
        }
    }
    it("updates imported globals in the provider and preserves function ownership") {
        turbowasm_module imported = {0};
        turbowasm_instance consumer = {0};
        turbowasm_linker imports = {0};
        const uint32_t provider_globals[] = {0, 1, 7};
        turbowasm_value values[] = {nodes[0], vector_value(), run("function", NULL, 0).values[0]};
        unsigned i;
        check_equal(turbowasm_module_load_borrowed(&imported, global_imports_bytes, sizeof(global_imports_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&imports), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&imports,
            (turbowasm_name){(const uint8_t *)"p", 1}, &instance), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&consumer, &imported, &imports, &store), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            turbowasm_jit_backend backend = {0};
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(consumer.impl, &backend, 1u), TURBOWASM_OK);
        }
#endif
        for (i = 0; i < 3; ++i) {
            turbowasm_value result = {0};
            size_t count;
            turbowasm_trap trap;
            check_equal(turbowasm_instance_invoke(&consumer, i * 2 + 1, &values[i], 1, NULL, 0, &count, &trap), TURBOWASM_OK);
            check_equal(turbowasm_instance_global_get(instance.impl, provider_globals[i], &result), TURBOWASM_OK);
            same_value(result, values[i]);
            if (i == 0) values[i] = nodes[1];
            if (i == 1) values[i].as.v128.shape = TURBOWASM_V128_U64X2;
            check_equal(turbowasm_instance_global_set(instance.impl, provider_globals[i], values[i]), TURBOWASM_OK);
            check_equal(turbowasm_instance_invoke(&consumer, i * 2, NULL, 0, &result, 1, &count, &trap), TURBOWASM_OK);
            check_equal(count, 1u); same_value(result, values[i]);
#ifdef TURBOWASM_TEST_MIR
            check_equal(((turbowasm_instance_impl *)consumer.impl)->jit_functions[i * 2].state, TURBOWASM_JIT_COMPILED);
            check_equal(((turbowasm_instance_impl *)consumer.impl)->jit_functions[i * 2 + 1].state, TURBOWASM_JIT_COMPILED);
#endif
        }
        turbowasm_instance_destroy(&consumer); turbowasm_linker_destroy(&imports); turbowasm_module_destroy(&imported);
    }
    it("traps only on reachable unreachable instructions and matches fuel priority") {
        turbowasm_value arg = integer(0, false);
        turbowasm_execution_options options = {0};
        unsigned fuel;
        check_equal(compare_control("trap-arm", &arg, 1, NULL).values[0].as.i32, 17);
        arg.as.i32 = 1;
        check_equal(compare_control("trap-arm", &arg, 1, NULL).trap, TURBOWASM_TRAP_UNREACHABLE);
        options.has_fuel_limit = true;
        for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
            options.fuel = fuel;
            compare_control("trap-arm", &arg, 1, &options);
            compare_control("trap", NULL, 0, &options);
        }
    }
}
