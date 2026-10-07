#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_gc_helper.h"
#include "fixtures/gc_native.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { GC_OPCODE_COUNT = 31, FUEL_BOUNDARIES = 32, OBJECT_LIMIT = 8, LOOP_COUNT = 1024 };
static turbowasm_module module;
static turbowasm_store store;
static turbowasm_instance native, reference;
static turbowasm_linker linker;
static struct { size_t attempts, fail_at, live; } allocations;
static unsigned collections;
typedef struct outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value value;
} outcome;

static void *allocate(void *context, size_t size) {
    void *p;
    (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++allocations.live;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(allocations.live != 0); --allocations.live; }
    free(p);
}
static uint32_t function_index(const char *name) {
    size_t i, length = strlen(name);
    for (i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->name.size == length && memcmp(e->name.bytes, name, length) == 0) return e->item_index;
    }
    check(false, "missing function %s", name); return UINT32_MAX;
}
static turbowasm_value integer(int32_t n) {
    turbowasm_value result = {0}; result.kind = TURBOWASM_VALUE_I32; result.as.i32 = n; return result;
}
static outcome invoke(turbowasm_instance *instance, const char *name, turbowasm_value arg,
    const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(instance, function_index(name), &arg, 1,
        &result.value, 1, &result.count, &result.trap, options);
    return result;
}
static void compiled(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_instance_impl *impl = native.impl;
    check(impl->jit_functions[function_index(name)].state == TURBOWASM_JIT_COMPILED,
        "%s must compile", name);
#else
    (void)name;
#endif
}
static void same_value(turbowasm_value a, turbowasm_value b) {
    check_equal(a.kind, b.kind);
    if (a.kind == TURBOWASM_VALUE_I32) check_equal(a.as.i32, b.as.i32);
    else if (a.kind == TURBOWASM_VALUE_V128) {
        check_equal(a.as.v128.shape, b.as.v128.shape);
        check_equal(memcmp(&a.as.v128.bits, &b.as.v128.bits, sizeof(a.as.v128.bits)), 0);
    } else if (a.kind == TURBOWASM_VALUE_EXTERNREF) {
        check_equal(a.as.externref.is_null, b.as.externref.is_null);
        if (!a.as.externref.is_null) check_equal(a.as.externref.token, b.as.externref.token);
    } else check(false, "unexpected result kind");
}
static outcome compare(const char *name, turbowasm_value arg, const turbowasm_execution_options *options) {
    outcome a, b;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    a = invoke(&reference, name, arg, options);
    b = invoke(&native, name, arg, options);
    check_equal(b.status, a.status); check_equal(b.trap, a.trap); check_equal(b.count, a.count);
    if (a.count != 0) same_value(b.value, a.value);
    compiled(name);
    return b;
}
static turbowasm_status host_collect(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)results; (void)capacity;
    check_equal(argc, 0u); ++collections;
    *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return turbowasm_store_collect(&store);
}
static int32_t expected(uint32_t op, int32_t n) {
    switch (op) {
    case 1: case 10: case 19: return 0;
    case 3: case 12: return (int32_t)(((uint32_t)n & 255u) ^ 128u) - 128;
    case 4: case 13: return (int32_t)((uint32_t)n & 255u);
    case 6: case 7: case 15: return 3;
    case 9: case 18: return 255;
    case 20: case 21: case 23: return 1;
    case 24: case 25: case 26: case 27: case 28: case 29:
        return (int32_t)(((uint32_t)n & UINT32_C(0x7fffffff)) ^ UINT32_C(0x40000000)) - INT32_C(0x40000000);
    case 30: return (int32_t)((uint32_t)n & UINT32_C(0x7fffffff));
    default: return n;
    }
}

spec("native GC instruction boundary") {
    before_each() {
        turbowasm_store_config config;
        turbowasm_host_function_type type = {0};
        memset(&allocations, 0, sizeof(allocations)); collections = 0;
        turbowasm_store_config_init(&config);
        config.max_objects = OBJECT_LIMIT;
        config.runtime.allocator.allocate = allocate;
        config.runtime.allocator.deallocate = deallocate;
        check_equal(turbowasm_store_create(&store, &config), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed_with_config(&module, gc_native_bytes,
            sizeof(gc_native_bytes), &config.runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"collect", 7},
            &type, host_collect, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&native, &module, &linker, &store), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&reference, &module, &linker, &store), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            turbowasm_jit_backend backend = {0};
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(native.impl, &backend, 1), TURBOWASM_OK);
        }
#endif
    }
    after_each() {
        allocations.fail_at = 0;
        turbowasm_instance_destroy(&native); turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        check_equal(turbowasm_store_destroy(&store), TURBOWASM_OK);
        check_equal(allocations.live, 0u);
    }
    it("executes every GC opcode over integer extrema and packed values") {
        const int32_t inputs[] = {INT32_MIN, -129, -1, 0, 127, 128, INT32_MAX};
        uint32_t op; size_t i;
        for (op = 0; op < GC_OPCODE_COUNT; ++op) {
            char name[16]; snprintf(name, sizeof(name), "op%u", op);
            for (i = 0; i < sizeof(inputs) / sizeof(*inputs); ++i) {
                outcome result = compare(name, integer(inputs[i]), NULL);
                check_equal(result.status, TURBOWASM_OK); check_equal(result.count, 1u);
                check_equal(result.value.as.i32, expected(op, inputs[i]));
            }
        }
    }
    it("matches instruction fuel boundaries for every GC opcode") {
        uint32_t op, fuel;
        turbowasm_execution_options options = {0}; options.has_fuel_limit = true;
        for (op = 0; op < GC_OPCODE_COUNT; ++op) {
            char name[16]; snprintf(name, sizeof(name), "op%u", op);
            for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
                options.fuel = fuel; compare(name, integer(-1), &options);
            }
        }
    }
    it("preserves heterogeneous constructor inputs and vector metadata across collection") {
        const uint64_t bits[] = {UINT64_C(0x123456789abcdef), UINT64_C(0x8000000000000000)};
        turbowasm_value vector = {0};
        outcome result = compare("mixed", integer(37), NULL);
        check_equal(result.status, TURBOWASM_OK); check_equal(collections, 2u);
        vector.kind = TURBOWASM_VALUE_V128; vector.as.v128.shape = TURBOWASM_V128_RAW;
        cmeta_simd_v128_load(&vector.as.v128.bits, bits); same_value(result.value, vector);
        vector.as.v128.shape = TURBOWASM_V128_F64X2;
        result = compare("vector-array", vector, NULL); check_equal(result.status, TURBOWASM_OK);
        same_value(result.value, vector);
        result = compare("wide", integer(73), NULL); check_equal(result.status, TURBOWASM_OK);
        check_equal(result.value.as.i32, 73);
    }
    it("collects native constructor temporaries within the object quota") {
        turbowasm_store_stats stats;
        outcome result = compare("collect-loop", integer(LOOP_COUNT), NULL);
        check_equal(result.status, TURBOWASM_OK); check_equal(result.value.as.i32, 1);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        check_equal(turbowasm_store_get_stats(&store, &stats), TURBOWASM_OK);
        check_equal(stats.objects, 0u);
    }
    it("takes both cast branch edges and forwards loop references") {
        const char *names[] = {"branch", "branch-fail", "loop"};
        unsigned name, choice, fuel;
        for (name = 0; name < sizeof(names) / sizeof(*names); ++name) {
            for (choice = 0; choice < 2; ++choice) {
                turbowasm_execution_options options = {0};
                outcome result = compare(names[name], integer((int32_t)choice), NULL);
                check_equal(result.status, TURBOWASM_OK);
                check_equal(result.value.as.i32, name == 2 ? 1 : choice ? 19 : name == 0 ? 7 : 1);
                options.has_fuel_limit = true;
                for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
                    options.fuel = fuel; compare(names[name], integer((int32_t)choice), &options);
                }
            }
        }
    }
    it("preserves external identity and null conversion across collection") {
        turbowasm_value arg = {0}; outcome result;
        arg.kind = TURBOWASM_VALUE_EXTERNREF; arg.as.externref.token = (uintptr_t)&module;
        result = compare("convert", arg, NULL); check_equal(result.status, TURBOWASM_OK); same_value(result.value, arg);
        arg.as.externref.is_null = true;
        result = compare("convert", arg, NULL); check_equal(result.status, TURBOWASM_OK); same_value(result.value, arg);
    }
    it("preserves null bounds cast segment and quota failures") {
        const char *names[] = {"null", "bounds", "cast", "dead-wide", "drop-data", "drop-elem"};
        const turbowasm_trap traps[] = {TURBOWASM_TRAP_NULL_REFERENCE, TURBOWASM_TRAP_ARRAY_OUT_OF_BOUNDS,
            TURBOWASM_TRAP_CAST_FAILURE, TURBOWASM_TRAP_UNREACHABLE,
            TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS};
        unsigned i;
        for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            outcome result = compare(names[i], integer(-1), NULL);
            check_equal(result.status, TURBOWASM_TRAPPED); check_equal(result.trap, traps[i]); check_equal(result.count, 0u);
        }
        check_equal(compare("drop-data", integer(0), NULL).status, TURBOWASM_OK);
        check_equal(compare("drop-elem", integer(0), NULL).status, TURBOWASM_OK);
        check_equal(compare("allocate", integer(INT32_MAX), NULL).status, TURBOWASM_OUT_OF_MEMORY);
    }
    it("cleans up failed native allocations and admits a later successful invocation") {
        size_t attempts, point, live;
        outcome result = invoke(&native, "mixed", integer(37), NULL);
        check_equal(result.status, TURBOWASM_OK); compiled("mixed");
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        live = allocations.live;
        allocations.attempts = 0;
        result = invoke(&native, "mixed", integer(37), NULL); check_equal(result.status, TURBOWASM_OK);
        attempts = allocations.attempts; check_true(attempts != 0);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        for (point = 1; point <= attempts; ++point) {
            allocations.attempts = 0; allocations.fail_at = point;
            result = invoke(&native, "mixed", integer(37), NULL);
            allocations.fail_at = 0;
            check_equal(result.status, TURBOWASM_OUT_OF_MEMORY); check_equal(result.count, 0u);
            check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
            check_equal(allocations.live, live);
        }
        check_equal(invoke(&native, "mixed", integer(37), NULL).status, TURBOWASM_OK);
    }
}
