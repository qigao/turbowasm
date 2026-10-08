#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/eh_native.h"
#include "fixtures/eh_imports.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { RESULT_CAPACITY = 8, FUEL_BOUNDARIES = 40, LOOP_COUNT = 32 };
static turbowasm_module module, imports_module;
static turbowasm_store store;
static turbowasm_instance native, reference, consumer;
static turbowasm_linker linker, imports;
static turbowasm_root root;
static struct { size_t attempts, fail_at, live; } allocations;
static unsigned collections;
typedef struct outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value values[RESULT_CAPACITY];
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
static outcome invoke(turbowasm_instance *instance, const char *name, const turbowasm_value *args,
    size_t count, const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(instance, function_index(name), args, count,
        result.values, RESULT_CAPACITY, &result.count, &result.trap, options);
    return result;
}
static void attach(turbowasm_instance *instance) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(instance->impl, &backend, 1), TURBOWASM_OK);
#else
    (void)instance;
#endif
}
static void compiled(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    check(((turbowasm_instance_impl *)native.impl)->jit_functions[function_index(name)].state == TURBOWASM_JIT_COMPILED,
        "%s must compile", name);
#else
    (void)name;
#endif
}
static void same_value(turbowasm_value a, turbowasm_value b) {
    check_equal(a.kind, b.kind);
    switch (a.kind) {
    case TURBOWASM_VALUE_I32: case TURBOWASM_VALUE_F32:
        check_equal(memcmp(&a.as, &b.as, sizeof(uint32_t)), 0); break;
    case TURBOWASM_VALUE_I64: case TURBOWASM_VALUE_F64:
        check_equal(memcmp(&a.as, &b.as, sizeof(uint64_t)), 0); break;
    case TURBOWASM_VALUE_V128:
        check_equal(a.as.v128.shape, b.as.v128.shape);
        check_equal(memcmp(&a.as.v128.bits, &b.as.v128.bits, sizeof(a.as.v128.bits)), 0); break;
    case TURBOWASM_VALUE_GCREF:
        check_true(a.as.gcref.store == b.as.gcref.store); check_equal(a.as.gcref.handle, b.as.gcref.handle); break;
    case TURBOWASM_VALUE_EXNREF:
        check_equal(a.as.exnref.is_null, b.as.exnref.is_null);
        check_equal(a.as.exnref.exception == NULL, b.as.exnref.exception == NULL); break;
    default: check(false, "unexpected result kind");
    }
}
static outcome compare(const char *name, turbowasm_value arg, const turbowasm_execution_options *options) {
    outcome a = invoke(&reference, name, &arg, 1, options), b = invoke(&native, name, &arg, 1, options);
    size_t i;
    check_equal(b.status, a.status); check_equal(b.trap, a.trap); check_equal(b.count, a.count);
    for (i = 0; i < a.count; ++i) same_value(b.values[i], a.values[i]);
    compiled(name); return b;
}
static turbowasm_status host_collect(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)results; (void)capacity;
    check_equal(argc, 0u); ++collections; *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return turbowasm_store_collect(&store);
}
spec("native exception dispatch") {
    before_each() {
        turbowasm_store_config config;
        turbowasm_host_function_type type = {0};
        memset(&allocations, 0, sizeof(allocations)); collections = 0;
        turbowasm_store_config_init(&config);
        config.runtime.allocator.allocate = allocate; config.runtime.allocator.deallocate = deallocate;
        check_equal(turbowasm_store_create(&store, &config), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed_with_config(&module, eh_native_bytes,
            sizeof(eh_native_bytes), &config.runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"collect", 7},
            &type, host_collect, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&native, &module, &linker, &store), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&reference, &module, &linker, &store), TURBOWASM_OK);
        attach(&native);
    }
    after_each() {
        allocations.fail_at = 0;
        if (root.impl != NULL) check_equal(turbowasm_root_release(&root), TURBOWASM_OK);
        turbowasm_instance_destroy(&consumer); turbowasm_linker_destroy(&imports); turbowasm_module_destroy(&imports_module);
        turbowasm_instance_destroy(&native); turbowasm_instance_destroy(&reference);
        turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        check_equal(turbowasm_store_destroy(&store), TURBOWASM_OK); check_equal(allocations.live, 0u);
    }
    it("catches native direct indirect reference and tail callees in lexical order") {
        const char *names[] = {"inline", "direct", "indirect", "reference", "tail-caller", "ordered", "nested", "rethrow", "normal"};
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            outcome result = compare(names[i], integer(37), NULL);
            check_equal(result.status, TURBOWASM_OK); check_equal(result.count, 1u); check_equal(result.values[0].as.i32, 37);
        }
        compiled("throw"); compiled("tail");
        check_equal(compare("all", integer(37), NULL).values[0].as.i32, 7);
        check_equal(compare("loop", integer(LOOP_COUNT), NULL).values[0].as.i32, 19);
    }
    it("matches fuel before throws handler entry and handler loop backedges") {
        const char *names[] = {"inline", "direct", "indirect", "reference", "rethrow", "loop", "tail-caller", "normal"};
        turbowasm_execution_options options = {0}; unsigned fuel; size_t i;
        options.has_fuel_limit = true;
        for (i = 0; i < sizeof(names) / sizeof(*names); ++i)
            for (fuel = 0; fuel < FUEL_BOUNDARIES; ++fuel) {
                options.fuel = fuel; compare(names[i], integer(3), &options);
            }
    }
    it("does not catch traps and removes current handlers before a tail call") {
        outcome result = compare("null", integer(0), NULL);
        check_equal(result.status, TURBOWASM_TRAPPED); check_equal(result.trap, TURBOWASM_TRAP_NULL_REFERENCE);
        result = compare("trap", integer(0), NULL);
        check_equal(result.status, TURBOWASM_TRAPPED); check_equal(result.trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(compare("tail-escape", integer(31), NULL).status, TURBOWASM_EXCEPTION);
        check_equal(compare("throw", integer(31), NULL).status, TURBOWASM_EXCEPTION);
        check_equal(compare("inline", integer(31), NULL).values[0].as.i32, 31);
    }
    it("returns exception references and rethrows their original payload") {
        turbowasm_value arg = integer(41); outcome result = compare("catch-ref", arg, NULL);
        turbowasm_exception *exception;
        check_equal(result.status, TURBOWASM_OK); check_equal(result.count, 2u);
        check_false(result.values[1].as.exnref.is_null);
        exception = (turbowasm_exception *)result.values[1].as.exnref.exception;
        check_equal(exception->payload_count, 1u); check_equal(exception->payload[0].as.i32, 41);
        check_true(exception->tag.owner == native.impl);
        result = invoke(&native, "throw-ref", &result.values[1], 1, NULL);
        check_equal(result.status, TURBOWASM_EXCEPTION); compiled("throw-ref");
        check_true(((turbowasm_instance_impl *)native.impl)->pending_exception == exception);
    }
    it("preserves mixed payload bits vector shape and managed roots after collection") {
        const uint64_t bits[] = {UINT64_C(0x7ff8000000001234), UINT64_C(0x8000000000000000)};
        turbowasm_value arg = integer(73), args[2]; outcome actual, expected; size_t i;
        args[1] = invoke(&native, "make", &arg, 1, NULL).values[0];
        check_equal(turbowasm_root_retain(&store, &args[1], &root), TURBOWASM_OK);
        args[0] = (turbowasm_value){0}; args[0].kind = TURBOWASM_VALUE_V128; args[0].as.v128.shape = TURBOWASM_V128_F64X2;
        cmeta_simd_v128_load(&args[0].as.v128.bits, bits);
        expected = invoke(&reference, "mixed", args, 2, NULL); actual = invoke(&native, "mixed", args, 2, NULL);
        check_equal(actual.status, TURBOWASM_OK); check_equal(expected.status, TURBOWASM_OK); check_equal(actual.count, 6u);
        for (i = 0; i < actual.count; ++i) same_value(actual.values[i], expected.values[i]);
        same_value(actual.values[4], args[0]); same_value(actual.values[5], args[1]); compiled("mixed");
        check_equal(turbowasm_root_release(&root), TURBOWASM_OK);
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        check_equal(invoke(&native, "read", &args[1], 1, NULL).values[0].as.i32, 73);
        check_equal(collections, 2u);
    }
    it("preserves explicitly mixed interpreter and native exception boundaries") {
#ifdef TURBOWASM_TEST_MIR
        ((turbowasm_instance_impl *)native.impl)->jit_functions[function_index("throw")].state = TURBOWASM_JIT_INTERPRET_ONLY;
#endif
        check_equal(compare("direct", integer(53), NULL).values[0].as.i32, 53);
        check_equal(compare("indirect", integer(53), NULL).values[0].as.i32, 53);
        check_equal(compare("reference", integer(53), NULL).values[0].as.i32, 53);
    }
    it("resolves imported tag identity and transfers uncaught cross-instance tails") {
        turbowasm_value arg = integer(61), result; size_t count; turbowasm_trap trap;
        check_equal(turbowasm_module_load_borrowed(&imports_module, eh_imports_bytes, sizeof(eh_imports_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&imports), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&imports, (turbowasm_name){(const uint8_t *)"p", 1}, &native), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&consumer, &imports_module, &imports, &store), TURBOWASM_OK);
        attach(&consumer);
        check_equal(turbowasm_instance_invoke(&consumer, 1, &arg, 1, &result, 1, &count, &trap), TURBOWASM_OK);
        check_equal(result.as.i32, 61); compiled("throw");
        check_equal(turbowasm_instance_invoke(&consumer, 2, &arg, 1, NULL, 0, &count, &trap), TURBOWASM_EXCEPTION);
        check_true(((turbowasm_instance_impl *)consumer.impl)->pending_exception->tag.owner == native.impl);
#ifdef TURBOWASM_TEST_MIR
        check_equal(((turbowasm_instance_impl *)consumer.impl)->jit_functions[1].state, TURBOWASM_JIT_COMPILED);
        check_equal(((turbowasm_instance_impl *)consumer.impl)->jit_functions[2].state, TURBOWASM_JIT_COMPILED);
#endif
    }
    it("cleans up failed exception allocations and can throw again") {
        turbowasm_value arg = integer(37); size_t attempts, point, live;
        check_equal(invoke(&native, "inline", &arg, 1, NULL).status, TURBOWASM_OK); compiled("inline");
        allocations.attempts = 0;
        check_equal(invoke(&native, "inline", &arg, 1, NULL).status, TURBOWASM_OK);
        attempts = allocations.attempts; live = allocations.live;
        check_true(attempts != 0);
        for (point = 1; point <= attempts; ++point) {
            outcome result;
            allocations.attempts = 0; allocations.fail_at = point;
            result = invoke(&native, "inline", &arg, 1, NULL); allocations.fail_at = 0;
            check_equal(result.status, TURBOWASM_OUT_OF_MEMORY); check_equal(result.count, 0u);
            check_equal(allocations.live, live); check_null(((turbowasm_instance_impl *)native.impl)->pending_exception);
        }
        check_equal(invoke(&native, "inline", &arg, 1, NULL).status, TURBOWASM_OK);
    }
}
