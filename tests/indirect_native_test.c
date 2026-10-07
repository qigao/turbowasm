#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/indirect_native.h"
#include "fixtures/indirect_imports.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { TUPLE_COUNT = 7, ARGUMENT_COUNT = 8, CHAIN_LENGTH = 1024, FUEL_LIMIT = 40 };
static turbowasm_module module, imported_module;
static turbowasm_instance instance, reference, consumer, foreign;
static turbowasm_store store, other_store;
static turbowasm_linker linker, imports;
static turbowasm_root root;
static turbowasm_value node;
static struct { size_t live, attempts, fail_at; } allocations;
static unsigned collections;

typedef struct outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value values[TUPLE_COUNT];
} outcome;

static void *allocate(void *context, size_t size) {
    void *value;
    (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    value = malloc(size);
    if (value != NULL) ++allocations.live;
    return value;
}
static void deallocate(void *context, void *value) {
    (void)context;
    if (value != NULL) { check_true(allocations.live != 0u); --allocations.live; }
    free(value);
}
static uint32_t function_index(const turbowasm_instance *target, const char *name) {
    const turbowasm_module *source = turbowasm_instance_module(target);
    size_t i, size = strlen(name);
    for (i = 0; i < turbowasm_module_export_count(source); ++i) {
        const turbowasm_export_desc *desc = turbowasm_module_export_at(source, i);
        if (desc->name.size == size && memcmp(desc->name.bytes, name, size) == 0)
            return desc->item_index;
    }
    check(false, "missing export %s", name);
    return UINT32_MAX;
}
static void attach(turbowasm_instance *target) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(target->impl, &backend, 1u), TURBOWASM_OK);
#else
    (void)target;
#endif
}
static void compiled(turbowasm_instance *target, const char *name) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_instance_impl *impl = target->impl;
    check(impl->jit_functions[function_index(target, name)].state == TURBOWASM_JIT_COMPILED,
        "%s must compile", name);
#else
    (void)target; (void)name;
#endif
}
static turbowasm_value integer(int64_t value, bool wide) {
    turbowasm_value result = {0};
    result.kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
    if (wide) result.as.i64 = value; else result.as.i32 = (int32_t)value;
    return result;
}
static turbowasm_value function(turbowasm_instance *target, const char *name) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_FUNCREF;
    value.as.funcref.owner = target->impl;
    value.as.funcref.function_index = function_index(target, name);
    return value;
}
static outcome invoke(turbowasm_instance *target, const char *name,
    const turbowasm_value *args, size_t count, const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(target, function_index(target, name),
        args, count, result.values, TUPLE_COUNT, &result.count, &result.trap, options);
    return result;
}
static void same_values(const turbowasm_value *a, const turbowasm_value *b, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        size_t bytes = 0u;
        check_equal(a[i].kind, b[i].kind);
        switch (a[i].kind) {
            case TURBOWASM_VALUE_I32: case TURBOWASM_VALUE_F32: bytes = sizeof(uint32_t); break;
            case TURBOWASM_VALUE_I64: case TURBOWASM_VALUE_F64: bytes = sizeof(uint64_t); break;
            case TURBOWASM_VALUE_GCREF: case TURBOWASM_VALUE_MANAGED_EXTERNREF:
                check_true(a[i].as.gcref.store == b[i].as.gcref.store);
                check_equal(a[i].as.gcref.handle, b[i].as.gcref.handle); break;
            case TURBOWASM_VALUE_FUNCREF:
                check_equal(a[i].as.funcref.is_null, b[i].as.funcref.is_null);
                check_equal(a[i].as.funcref.function_index, b[i].as.funcref.function_index);
                check_true(a[i].as.funcref.owner == b[i].as.funcref.owner); break;
            default: check(false, "unexpected result kind");
        }
        if (bytes != 0u) check_equal(memcmp(&a[i].as, &b[i].as, bytes), 0);
    }
}
static void tuple_arguments(turbowasm_value args[ARGUMENT_COUNT]) {
    uint32_t f32 = UINT32_C(0x7fc01234);
    uint64_t f64 = UINT64_C(0x8000000000000000);
    memset(args, 0, ARGUMENT_COUNT * sizeof(*args));
    args[0] = integer(-13, false); args[1] = integer(INT64_C(0x1234567887654321), true);
    args[2].kind = TURBOWASM_VALUE_F32; memcpy(&args[2].as.f32, &f32, sizeof(f32));
    args[3].kind = TURBOWASM_VALUE_F64; memcpy(&args[3].as.f64, &f64, sizeof(f64));
    args[4] = node; args[5] = node; args[5].kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
    args[6] = function(&instance, "zero");
}
static outcome compare(const char *name, const turbowasm_value *args, size_t count,
    const turbowasm_execution_options *options) {
    outcome expected = invoke(&reference, name, args, count, options);
    outcome actual = invoke(&instance, name, args, count, options);
    check_equal(actual.status, expected.status);
    check_equal(actual.trap, expected.trap);
    check_equal(actual.count, expected.count);
    same_values(actual.values, expected.values, actual.count);
    compiled(&instance, name);
    return actual;
}
static turbowasm_status host_collect(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t count, turbowasm_value *results,
    size_t capacity, size_t *out_count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)count; (void)results; (void)capacity;
    ++collections;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    *out_count = 0u; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static turbowasm_status host_zero(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t count, turbowasm_value *results,
    size_t capacity, size_t *out_count, turbowasm_trap *trap) {
    turbowasm_instance *target = turbowasm_host_call_instance(call);
    (void)context;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    return turbowasm_instance_invoke(target, function_index(target, "zero"),
        args, count, results, capacity, out_count, trap);
}

spec("native indirect calls") {
    before_each() {
        const turbowasm_value_kind result_kind = TURBOWASM_VALUE_I32;
        turbowasm_host_function_type type = {NULL, 0u, &result_kind, 1u}, empty = {0};
        turbowasm_runtime_config runtime;
        turbowasm_store_config config;
        turbowasm_value argument = integer(7, false);
        outcome made;
        memset(&allocations, 0, sizeof(allocations)); collections = 0;
        turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = allocate; runtime.allocator.deallocate = deallocate;
        turbowasm_store_config_init(&config); config.runtime = runtime;
        check_equal(turbowasm_store_create(&store, &config), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed_with_config(&module,
            indirect_native_bytes, sizeof(indirect_native_bytes), &runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"zero", 4},
            &type, host_zero, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"collect", 7},
            &empty, host_collect, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&instance, &module, &linker, &store), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&reference, &module, &linker, &store), TURBOWASM_OK);
        made = invoke(&instance, "make", &argument, 1u, NULL);
        check_equal(made.status, TURBOWASM_OK); node = made.values[0];
        check_equal(turbowasm_root_retain(&store, &node, &root), TURBOWASM_OK);
        attach(&instance);
    }
    after_each() {
        allocations.fail_at = 0u;
        turbowasm_instance_destroy(&consumer); turbowasm_linker_destroy(&imports);
        turbowasm_module_destroy(&imported_module);
        turbowasm_instance_destroy(&foreign);
        if (other_store.impl != NULL) check_equal(turbowasm_store_destroy(&other_store), TURBOWASM_OK);
        if (root.impl != NULL) check_equal(turbowasm_root_release(&root), TURBOWASM_OK);
        turbowasm_instance_destroy(&instance); turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        if (store.impl != NULL) check_equal(turbowasm_store_destroy(&store), TURBOWASM_OK);
        check_equal(allocations.live, (size_t)0);
    }
    it("preserves mixed tuples through table32/table64 and typed reference calls and tails") {
        const char *names[] = {"call-32", "tail-32", "call-64", "tail-64", "call-ref", "tail-ref"};
        unsigned form, target;
        turbowasm_value args[ARGUMENT_COUNT];
        tuple_arguments(args);
        for (form = 0u; form < 6u; ++form) {
            for (target = 0u; target < 3u; ++target) {
                static const unsigned slots[] = {0, 3, 5};
                static const char *functions[] = {"tuple", NULL, "interpret"};
                outcome result;
                if (form >= 4u) {
                    if (target == 1u) {
                        check_equal(turbowasm_instance_table_get64(&instance, 0u, 3u, &args[7]), TURBOWASM_OK);
                    } else args[7] = function(&instance, functions[target]);
                } else args[7] = integer(slots[target], form >= 2u);
                result = compare(names[form], args, ARGUMENT_COUNT, NULL);
                check_equal(result.status, TURBOWASM_OK);
                check_equal(result.count, (size_t)TUPLE_COUNT);
            }
        }
    }
    it("invokes zero-argument host targets and permits callback re-entry") {
        const char *names[] = {"call-zero", "tail-zero"};
        turbowasm_value target = function(&instance, "host-zero");
        unsigned i;
        for (i = 0; i < 2; ++i) {
            outcome result = compare(names[i], &target, 1u, NULL);
            check_equal(result.status, TURBOWASM_OK);
            check_equal(result.values[0].as.i32, 42);
        }
    }
    it("preserves bounds, null and incompatible signature traps") {
        const char *names[] = {"call-32", "tail-32", "call-64", "tail-64", "call-ref", "tail-ref"};
        unsigned form, error;
        turbowasm_value args[ARGUMENT_COUNT];
        tuple_arguments(args);
        for (form = 0; form < 6; ++form) {
            for (error = 0; error < (form < 4u ? 4u : 1u); ++error) {
                outcome result;
                if (form >= 4u) {
                    args[7] = function(&instance, "tuple"); args[7].as.funcref.is_null = true;
                } else args[7] = integer(error == 0u ? 7 : error == 1u ? 1 :
                    error == 2u ? -1 : INT64_C(1) << 32, form >= 2u);
                result = compare(names[form], args, ARGUMENT_COUNT, NULL);
                if (form < 2u && error == 3u) check_equal(result.status, TURBOWASM_OK);
                else {
                    check_equal(result.status, TURBOWASM_TRAPPED);
                    check_equal(result.trap, form >= 4u ? TURBOWASM_TRAP_NULL_REFERENCE :
                        error == 0u ? TURBOWASM_TRAP_INDIRECT_CALL_NULL :
                        error == 1u ? TURBOWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH : TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
                    check_equal(result.count, (size_t)0);
                }
            }
        }
    }
    it("roots native-only values through long indirect tail chains and host collection") {
        const char *names[] = {"chain32", "chain64", "chainref"};
        turbowasm_value args[] = {integer(CHAIN_LENGTH, false), node};
        unsigned i;
        for (i = 0; i < 3; ++i) {
            outcome result = compare(names[i], args, 2u, NULL);
            check_equal(result.status, TURBOWASM_OK); same_values(result.values, &node, 1u);
        }
        {
            outcome result = invoke(&instance, "live", args, 1u, NULL);
            outcome read;
            check_equal(result.status, TURBOWASM_OK); compiled(&instance, "live");
            read = invoke(&reference, "read", result.values, 1u, NULL);
            check_equal(read.status, TURBOWASM_OK); check_equal(read.values[0].as.i32, 7);
        }
        check_equal(collections, (unsigned)(7u * CHAIN_LENGTH));
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        {
            turbowasm_store_stats stats;
            check_equal(turbowasm_store_get_stats(&store, &stats), TURBOWASM_OK);
            check_equal(stats.objects, 1u);
        }
    }
    it("dispatches imported tables to their original provider and propagates exceptions") {
        const char *names[] = {"call32", "tail32", "call64", "tail64"};
        turbowasm_value args[ARGUMENT_COUNT];
        unsigned i;
        tuple_arguments(args);
        check_equal(turbowasm_module_load_borrowed(&imported_module,
            indirect_imports_bytes, sizeof(indirect_imports_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&imports), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&imports,
            (turbowasm_name){(const uint8_t *)"p", 1}, &instance), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&consumer, &imported_module, &imports, &store), TURBOWASM_OK);
        attach(&consumer);
        for (i = 0; i < 4; ++i) {
            outcome actual, expected;
            args[7] = integer(0, i >= 2u);
            actual = invoke(&consumer, names[i], args, ARGUMENT_COUNT, NULL);
            expected = invoke(&instance, "tuple", args, TUPLE_COUNT, NULL);
            check_equal(actual.status, TURBOWASM_OK); same_values(actual.values, expected.values, TUPLE_COUNT);
            compiled(&consumer, names[i]); compiled(&instance, "tuple");
            args[7] = integer(2, i >= 2u);
            actual = invoke(&consumer, names[i], args, ARGUMENT_COUNT, NULL);
            check_equal(actual.status, TURBOWASM_EXCEPTION); check_equal(actual.count, (size_t)0);
            check_not_null(((turbowasm_instance_impl *)consumer.impl)->pending_exception);
            check_null(((turbowasm_instance_impl *)instance.impl)->pending_exception);
        }
    }
    it("switches target stores without retaining an obsolete root registration") {
        const char *names[] = {"call-zero", "tail-zero"};
        turbowasm_value target;
        unsigned i;
        turbowasm_store_stats before, after;
        check_equal(turbowasm_store_create(&other_store, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&foreign, &module, &linker, &other_store), TURBOWASM_OK);
        attach(&foreign); target = function(&foreign, "zero");
        check_equal(turbowasm_store_get_stats(&other_store, &before), TURBOWASM_OK);
        for (i = 0; i < 2; ++i) {
            outcome result = invoke(&instance, names[i], &target, 1u, NULL);
            check_equal(result.status, TURBOWASM_OK); check_equal(result.values[0].as.i32, 42);
            compiled(&instance, names[i]); compiled(&foreign, "zero");
            check_equal(turbowasm_store_get_stats(&other_store, &after), TURBOWASM_OK);
            check_equal(after.bytes, before.bytes);
        }
    }
    it("normalizes instance-relative function arguments before transferring them") {
        turbowasm_value args[ARGUMENT_COUNT];
        outcome result;
        tuple_arguments(args); args[6].as.funcref.owner = NULL;
        args[7] = function(&reference, "tuple");
        result = invoke(&instance, "tail-ref", args, ARGUMENT_COUNT, NULL);
        check_equal(result.status, TURBOWASM_OK); compiled(&instance, "tail-ref");
        check_true(result.values[3].as.funcref.owner == instance.impl);
        args[6].as.funcref.function_index = UINT32_MAX;
        result = invoke(&instance, "tail-ref", args, ARGUMENT_COUNT, NULL);
        check_equal(result.status, TURBOWASM_INVALID_ARGUMENT);
    }
    it("accepts declared function subtypes and zero-result tuples") {
        const char *names[] = {"call-sub-ref", "tail-sub-ref", "call-sub-table", "tail-sub-table"};
        turbowasm_value args[] = {node, function(&instance, "derived")};
        unsigned i;
        check_equal(turbowasm_instance_table_set64(&instance, 0u, 0u, args[1]), TURBOWASM_OK);
        args[1] = function(&reference, "derived");
        check_equal(turbowasm_instance_table_set64(&reference, 0u, 0u, args[1]), TURBOWASM_OK);
        for (i = 0; i < 4; ++i) {
            outcome result;
            args[1] = i < 2u ? function(&instance, "derived") : integer(0, false);
            result = compare(names[i], args, 2u, NULL);
            check_equal(result.status, TURBOWASM_OK); same_values(result.values, &node, 1u);
        }
        {
            outcome result = compare("call-void", NULL, 0u, NULL);
            check_equal(result.status, TURBOWASM_OK); check_equal(result.count, (size_t)0);
            result = compare("tail-void", NULL, 0u, NULL);
            check_equal(result.status, TURBOWASM_OK); check_equal(result.count, (size_t)0);
        }
    }
    it("preserves fuel boundaries for all four instructions and cleans allocation failures") {
        const char *names[] = {"call-32", "tail-32", "call-ref", "tail-ref"};
        turbowasm_execution_options options = {0};
        turbowasm_value args[ARGUMENT_COUNT];
        unsigned i, fuel;
        tuple_arguments(args); options.has_fuel_limit = true;
        for (i = 0u; i < 4u; ++i) {
            size_t attempts, point, live;
            args[7] = i < 2u ? integer(0, false) : function(&instance, "tuple");
            for (fuel = 0; fuel < FUEL_LIMIT; ++fuel) {
                options.fuel = fuel; compare(names[i], args, ARGUMENT_COUNT, &options);
            }
            live = allocations.live; allocations.attempts = 0u;
            check_equal(invoke(&instance, names[i], args, ARGUMENT_COUNT, NULL).status, TURBOWASM_OK);
            attempts = allocations.attempts; check_true(attempts != 0u);
            for (point = 1; point <= attempts; ++point) {
                outcome result;
                allocations.attempts = 0u; allocations.fail_at = point;
                result = invoke(&instance, names[i], args, ARGUMENT_COUNT, NULL);
                allocations.fail_at = 0u;
                check_equal(result.status, TURBOWASM_OUT_OF_MEMORY); check_equal(result.count, (size_t)0);
                check_equal(allocations.live, live);
            }
            check_equal(invoke(&instance, names[i], args, ARGUMENT_COUNT, NULL).status, TURBOWASM_OK);
        }
    }
}
