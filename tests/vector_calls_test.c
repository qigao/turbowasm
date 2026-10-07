#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/vector_calls.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { TUPLE_SIZE = 6, TAIL_DEPTH = 1024, FUEL_LIMIT = 40 };
static turbowasm_module module;
static turbowasm_instance instance, reference;
static turbowasm_linker linker;
static struct { size_t attempts, fail_at, live; } allocations;

typedef struct outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value values[TUPLE_SIZE];
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
static uint32_t index_of(const char *name) {
    size_t i, size = strlen(name);
    for (i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *desc = turbowasm_module_export_at(&module, i);
        if (desc->name.size == size && memcmp(desc->name.bytes, name, size) == 0)
            return desc->item_index;
    }
    check(false, "missing export %s", name);
    return UINT32_MAX;
}
static void compiled(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_instance_impl *impl = instance.impl;
    check(impl->jit_functions[index_of(name)].state == TURBOWASM_JIT_COMPILED,
        "%s must compile", name);
#else
    (void)name;
#endif
}
static turbowasm_value vector(void) {
    const uint32_t bits[] = {UINT32_C(0x7fc01234), UINT32_C(0x80000000), UINT32_MAX, 1};
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_V128;
    value.as.v128.shape = TURBOWASM_V128_F32X4;
    cmeta_simd_v128_load(&value.as.v128.bits, bits);
    return value;
}
static turbowasm_value integer(int32_t number) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_I32; value.as.i32 = number;
    return value;
}
static void same(const turbowasm_value *actual, const turbowasm_value *expected) {
    size_t size = 0;
    check_equal(actual->kind, expected->kind);
    switch (expected->kind) {
        case TURBOWASM_VALUE_V128:
            check_equal(actual->as.v128.shape, expected->as.v128.shape);
            check_equal(memcmp(&actual->as.v128.bits, &expected->as.v128.bits,
                sizeof(expected->as.v128.bits)), 0); break;
        case TURBOWASM_VALUE_I32: size = sizeof(int32_t); break;
        case TURBOWASM_VALUE_I64: case TURBOWASM_VALUE_F64: size = sizeof(int64_t); break;
        case TURBOWASM_VALUE_EXTERNREF:
            check_equal(actual->as.externref.is_null, expected->as.externref.is_null);
            check_equal(actual->as.externref.token, expected->as.externref.token); break;
        default: check(false, "unexpected result kind");
    }
    if (size != 0) check_equal(memcmp(&actual->as, &expected->as, size), 0);
}
static outcome invoke(turbowasm_instance *target, const char *name,
    const turbowasm_value *args, size_t count, const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(target, index_of(name),
        args, count, result.values, TUPLE_SIZE, &result.count, &result.trap, options);
    return result;
}
static outcome compare(const char *name, const turbowasm_value *args, size_t count,
    const turbowasm_execution_options *options) {
    outcome expected = invoke(&reference, name, args, count, options);
    outcome actual = invoke(&instance, name, args, count, options);
    size_t i;
    check_equal(actual.status, expected.status); check_equal(actual.trap, expected.trap);
    check_equal(actual.count, expected.count);
    for (i = 0; i < actual.count; ++i) same(&actual.values[i], &expected.values[i]);
    compiled(name);
    return actual;
}
static turbowasm_status host(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t count, turbowasm_value *results,
    size_t capacity, size_t *out_count, turbowasm_trap *trap) {
    (void)context;
    return turbowasm_instance_invoke(turbowasm_host_call_instance(call), index_of("identity"),
        args, count, results, capacity, out_count, trap);
}

spec("vector call values") {
    before_each() {
        const turbowasm_value_kind kind = TURBOWASM_VALUE_V128;
        turbowasm_host_function_type type = {&kind, 1, &kind, 1};
        turbowasm_runtime_config runtime;
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = allocate; runtime.allocator.deallocate = deallocate;
        check_equal(turbowasm_module_load_borrowed_with_config(&module,
            vector_calls_bytes, sizeof(vector_calls_bytes), &runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1},
            (turbowasm_name){(const uint8_t *)"vector", 6}, &type, host, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&reference, &module, &linker), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            turbowasm_jit_backend backend = {0};
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1u), TURBOWASM_OK);
        }
#endif
    }
    after_each() {
        allocations.fail_at = 0;
        turbowasm_instance_destroy(&instance); turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        check_equal(allocations.live, 0u);
    }
    it("preserves vector bits and shape through direct, host and indirect calls") {
        const char *direct[] = {"identity", "direct", "tail", "host"};
        const char *indirect[] = {"table", "tail-table"};
        turbowasm_value args[] = {vector(), integer(0)};
        size_t i, target;
        for (i = 0; i < sizeof(direct) / sizeof(*direct); ++i) {
            outcome result = compare(direct[i], args, 1, NULL);
            check_equal(result.status, TURBOWASM_OK); same(&result.values[0], args);
        }
        for (i = 0; i < sizeof(indirect) / sizeof(*indirect); ++i)
            for (target = 0; target < 3; ++target) {
                args[1] = integer((int32_t)target);
                check_equal(compare(indirect[i], args, 2, NULL).status, TURBOWASM_OK);
            }
        args[1] = (turbowasm_value){0}; args[1].kind = TURBOWASM_VALUE_FUNCREF;
        args[1].as.funcref.function_index = index_of("identity");
        check_equal(compare("ref", args, 2, NULL).status, TURBOWASM_OK);
        check_equal(compare("tail-ref", args, 2, NULL).status, TURBOWASM_OK);
    }
    it("uses independent cells for mixed tuples and local snapshots") {
        turbowasm_value args[TUPLE_SIZE] = {0};
        outcome result;
        args[0] = integer(-17); args[1] = vector();
        args[2].kind = TURBOWASM_VALUE_F64; args[2].as.f64 = -0.0;
        args[3].kind = TURBOWASM_VALUE_EXTERNREF; args[3].as.externref.token = (uintptr_t)&module;
        args[4] = vector(); args[4].as.v128.shape = TURBOWASM_V128_U64X2;
        args[5].kind = TURBOWASM_VALUE_I64; args[5].as.i64 = INT64_C(0x1234567887654321);
        check_equal(compare("tuple", args, TUPLE_SIZE, NULL).status, TURBOWASM_OK);
        result = compare("locals", &args[1], 1, NULL);
        check_equal(result.status, TURBOWASM_OK);
        same(&result.values[0], &args[1]); same(&result.values[1], &args[1]);
    }
    it("merges vector control results and resets locals on self tail calls") {
        turbowasm_value args[] = {vector(), integer(0)};
        check_equal(compare("merge", args, 2, NULL).status, TURBOWASM_OK);
        args[1] = integer(1);
        check_equal(compare("merge", args, 2, NULL).status, TURBOWASM_OK);
        check_equal(compare("branch", args, 1, NULL).status, TURBOWASM_OK);
        args[1] = integer(TAIL_DEPTH);
        check_equal(compare("self", args, 2, NULL).status, TURBOWASM_OK);
        check_equal(compare("operation", args, 1, NULL).status, TURBOWASM_OK);
    }
    it("propagates traps and bounded execution without publishing results") {
        turbowasm_execution_options options = {0};
        turbowasm_value args[] = {vector(), integer(3)};
        check_true(compare("table", args, 2, NULL).status != TURBOWASM_OK);
        check_true(compare("trap", args, 1, NULL).status != TURBOWASM_OK);
        options.has_fuel_limit = true; options.fuel = FUEL_LIMIT;
        args[1] = integer(TAIL_DEPTH);
        check_true(compare("self", args, 2, &options).status != TURBOWASM_OK);
    }
    it("releases all call cells on allocation failure") {
        turbowasm_value args = vector();
        size_t start, attempts, live, i;
        check_equal(compare("host", &args, 1, NULL).status, TURBOWASM_OK);
        start = allocations.attempts;
        check_equal(invoke(&instance, "host", &args, 1, NULL).status, TURBOWASM_OK);
        attempts = allocations.attempts - start; live = allocations.live;
        for (i = 1; i <= attempts; ++i) {
            outcome result;
            allocations.fail_at = allocations.attempts + i;
            result = invoke(&instance, "host", &args, 1, NULL);
            allocations.fail_at = 0;
            check_equal(result.status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(allocations.live, live);
        }
        check_equal(compare("host", &args, 1, NULL).status, TURBOWASM_OK);
    }
}
