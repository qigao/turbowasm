#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "fixtures/resumable_native.h"
#include "fixtures/resumable_imports.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { RESULT_CAPACITY = 3, ITERATIONS = 37, MAX_RESUMES = 4096, COMPLETION = 19 };
static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_store store;
static turbowasm_linker linker;
static turbowasm_execution execution;
static turbowasm_module imports_module;
static turbowasm_instance consumer;
static turbowasm_linker imports;
static struct { size_t attempts, fail_at, live; } allocations;
static struct { unsigned submitted, resumed, cancelled; bool immediate, interrupt; } host;

static void *allocate(void *context, size_t size) {
    void *p;
    (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    p = malloc(size);
    if (p != NULL) ++allocations.live;
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
        if (e->name.size == length && memcmp(e->name.bytes, name, length) == 0)
            return e->item_index;
    }
    check(false, "missing function %s", name);
    return UINT32_MAX;
}
static void compiled(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    check(((turbowasm_instance_impl *)instance.impl)->jit_functions[function_index(name)].state == TURBOWASM_JIT_COMPILED,
        "%s must execute compiled", name);
#else
    (void)name;
#endif
}
static void attach(turbowasm_instance *target) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(target->impl, &backend, 1), TURBOWASM_OK);
#else
    (void)target;
#endif
}
static turbowasm_value integer(int32_t n) {
    turbowasm_value value = {0}; value.kind = TURBOWASM_VALUE_I32; value.as.i32 = n; return value;
}
static turbowasm_value vector(void) {
    const uint64_t bits[] = {UINT64_C(0x8000000000000000), UINT64_C(0x7ff8000000004321)};
    turbowasm_value value = {0}; value.kind = TURBOWASM_VALUE_V128;
    value.as.v128.shape = TURBOWASM_V128_F64X2;
    cmeta_simd_v128_load(&value.as.v128.bits, bits); return value;
}
static turbowasm_status host_wait(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    int completion = COMPLETION;
    turbowasm_status status;
    turbowasm_host_wait wait = {0};
    (void)context; (void)args;
    check_equal(argc, 0u); check_true(capacity >= 1);
    ++host.submitted;
    if (!host.immediate) {
        check_true(turbowasm_host_call_can_wait(call));
        status = turbowasm_host_call_wait(call, host.submitted, &wait, &completion);
        if (status != TURBOWASM_OK) {
            if (status == TURBOWASM_INTERRUPTED) ++host.cancelled;
            return status;
        }
    }
    ++host.resumed;
    results[0] = integer(completion); *count = 1; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static void create(const char *name, const turbowasm_value *args, size_t count) {
    check_equal(turbowasm_execution_create(&execution, &instance, function_index(name), args, count), TURBOWASM_OK);
}
static int32_t query(const char *name, const turbowasm_value *args, size_t count) {
    turbowasm_value result; size_t actual = 0; turbowasm_trap trap;
    check_equal(turbowasm_instance_invoke(&instance, function_index(name), args, count,
        &result, 1, &actual, &trap), TURBOWASM_OK);
    check_equal(actual, 1u); check_equal(result.kind, TURBOWASM_VALUE_I32);
    return result.as.i32;
}
static void complete_wait(void) {
    turbowasm_host_wait wait = {0}, wrong;
    check_equal(turbowasm_execution_yield_reason_get(&execution), TURBOWASM_YIELD_HOST_WAIT);
    check_true(turbowasm_execution_pending_host_wait(&execution, &wait));
    wrong = wait; ++wrong.operation_token;
    check_equal(turbowasm_execution_complete_host_wait(&execution, wrong, COMPLETION), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
    check_equal(turbowasm_execution_complete_host_wait(&execution, wait, COMPLETION), TURBOWASM_OK);
    check_equal(turbowasm_execution_complete_host_wait(&execution, wait, COMPLETION), TURBOWASM_INVALID_ARGUMENT);
}
static bool interrupt(void *context) { (void)context; return host.interrupt; }
static void collect_empty(void) {
    turbowasm_store_stats stats;
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    check_equal(turbowasm_store_get_stats(&store, &stats), TURBOWASM_OK);
    check_equal(stats.objects, 0u);
}
static void finish_values(turbowasm_value expected_vector) {
    const turbowasm_value *value;
    check_equal(turbowasm_execution_result_count(&execution), RESULT_CAPACITY);
    check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
    value = turbowasm_execution_result_at(&execution, 0);
    check_not_null(value); check_equal(query("read", value, 1), ITERATIONS);
    check_equal(turbowasm_execution_result_at(&execution, 1)->as.i32, ITERATIONS + COMPLETION);
    value = turbowasm_execution_result_at(&execution, 2);
    check_equal(value->kind, TURBOWASM_VALUE_V128);
    check_equal(value->as.v128.shape, expected_vector.as.v128.shape);
    check_equal(memcmp(&value->as.v128.bits, &expected_vector.as.v128.bits, sizeof(value->as.v128.bits)), 0);
}
spec("resumable native frames") {
    before_each() {
        turbowasm_store_config config;
        const turbowasm_value_kind result_kind = TURBOWASM_VALUE_I32;
        turbowasm_host_function_type type = {NULL, 0, &result_kind, 1};
        memset(&allocations, 0, sizeof(allocations)); memset(&host, 0, sizeof(host));
        turbowasm_store_config_init(&config);
        config.runtime.allocator.allocate = allocate; config.runtime.allocator.deallocate = deallocate;
        check_equal(turbowasm_store_create(&store, &config), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed_with_config(&module, resumable_native_bytes,
            sizeof(resumable_native_bytes), &config.runtime), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"wait", 4},
            &type, host_wait, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&instance, &module, &linker, &store), TURBOWASM_OK);
        attach(&instance);
    }
    after_each() {
        allocations.fail_at = 0;
        turbowasm_execution_destroy(&execution);
        turbowasm_instance_destroy(&consumer); turbowasm_linker_destroy(&imports); turbowasm_module_destroy(&imports_module);
        turbowasm_instance_destroy(&instance); turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        check_equal(turbowasm_store_destroy(&store), TURBOWASM_OK);
        check_equal(allocations.live, 0u);
    }
    it("resumes direct indirect reference tail and exception frames without replaying host effects") {
        const char *names[] = {"direct", "indirect", "reference", "tail", "eh"};
        turbowasm_value arg = integer(ITERATIONS); size_t i;
        for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            create(names[i], &arg, 1);
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
            compiled(names[i]); compiled("leaf");
            check_equal(host.submitted, i + 1); check_equal(host.resumed, i);
            check_equal(query("effects", NULL, 0), (int32_t)i + 1);
            complete_wait();
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
            check_equal(host.submitted, i + 1); check_equal(host.resumed, i + 1);
            check_equal(turbowasm_execution_state_get(&execution), TURBOWASM_EXECUTION_COMPLETED);
            check_equal(turbowasm_execution_result_at(&execution, 0)->as.i32,
                COMPLETION + (strcmp(names[i], "tail") == 0 ? 0 : ITERATIONS));
            turbowasm_execution_destroy(&execution);
        }
    }
    it("preserves stack-only GC values and vector bits across host wait and external collection") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        turbowasm_store_stats stats;
        create("values", args, 2);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
        compiled("values"); compiled("direct"); compiled("leaf");
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
        check_equal(turbowasm_store_get_stats(&store, &stats), TURBOWASM_OK); check_equal(stats.objects, 1u);
        complete_wait();
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        finish_values(args[1]);
        turbowasm_execution_destroy(&execution); collect_empty();
    }
    it("preserves GC and vectors while yielding at every instruction boundary") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        turbowasm_execution_options options = {0};
        turbowasm_status status; unsigned resumes = 0;
        host.immediate = true; options.has_fuel_limit = true; options.fuel = 1;
        create("values", args, 2);
        do {
            status = turbowasm_execution_resume(&execution, &options);
            check_true(++resumes < MAX_RESUMES);
            if (status == TURBOWASM_YIELDED) {
                check_equal(turbowasm_execution_yield_reason_get(&execution), TURBOWASM_YIELD_FUEL);
                check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
            }
        } while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK); check_true(resumes > 2);
        check_equal(host.submitted, 1u); compiled("values"); finish_values(args[1]);
    }
    it("retains cross-instance direct and tail calls while a provider is suspended") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        uint32_t index;
        check_equal(turbowasm_module_load_borrowed(&imports_module, resumable_imports_bytes,
            sizeof(resumable_imports_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&imports), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&imports,
            (turbowasm_name){(const uint8_t *)"p", 1}, &instance), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_in_store(&consumer, &imports_module, &imports, &store), TURBOWASM_OK);
        attach(&consumer);
        for (index = 1; index <= 2; ++index) {
            check_equal(turbowasm_execution_create(&execution, &consumer, index, args, 2), TURBOWASM_OK);
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
            check_equal(turbowasm_store_collect(&store), TURBOWASM_OK);
            compiled("values");
#ifdef TURBOWASM_TEST_MIR
            check_equal(((turbowasm_instance_impl *)consumer.impl)->jit_functions[index].state, TURBOWASM_JIT_COMPILED);
#endif
            complete_wait(); check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
            finish_values(args[1]); turbowasm_execution_destroy(&execution); collect_empty();
        }
    }
    it("changes interruption policy and resumes loop effects exactly once") {
        turbowasm_value arg = integer(ITERATIONS);
        turbowasm_execution_options options = {0};
        turbowasm_status status; unsigned resumes = 0;
        options.should_interrupt = interrupt; host.interrupt = true;
        create("loop", &arg, 1);
        check_equal(turbowasm_execution_resume(&execution, &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_execution_yield_reason_get(&execution), TURBOWASM_YIELD_INTERRUPTION);
        check_equal(query("effects", NULL, 0), 0); compiled("loop");
        host.interrupt = false; options.has_fuel_limit = true; options.fuel = 1;
        do {
            status = turbowasm_execution_resume(&execution, &options);
            check_true(++resumes < MAX_RESUMES);
        } while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK);
        check_equal(turbowasm_execution_result_at(&execution, 0)->as.i32, ITERATIONS);
        check_equal(query("effects", NULL, 0), ITERATIONS);
    }
    it("cancels suspended native host frames and releases every retained GC source") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        size_t baseline; unsigned i;
        host.immediate = true;
        create("values", args, 2); check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        turbowasm_execution_destroy(&execution); collect_empty(); baseline = allocations.live;
        host.immediate = false;
        for (i = 0; i < ITERATIONS; ++i) {
            create("values", args, 2);
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
            turbowasm_execution_destroy(&execution); collect_empty();
            check_equal(host.cancelled, i + 1); check_equal(allocations.live, baseline);
        }
        compiled("values");
    }
    it("cancels at fuel checkpoints with live native references") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        turbowasm_execution_options options = {0};
        unsigned point;
        host.immediate = true; options.has_fuel_limit = true;
        for (point = 0; point < ITERATIONS; ++point) {
            turbowasm_status status;
            options.fuel = point; create("values", args, 2);
            status = turbowasm_execution_resume(&execution, &options);
            check_true(status == TURBOWASM_YIELDED || status == TURBOWASM_OK);
            turbowasm_execution_destroy(&execution); collect_empty();
        }
        compiled("values");
    }
    it("returns trap and exception terminal states after a native host wait") {
        const char *names[] = {"trap", "throw"};
        const turbowasm_status statuses[] = {TURBOWASM_TRAPPED, TURBOWASM_EXCEPTION};
        const turbowasm_execution_state states[] = {TURBOWASM_EXECUTION_TRAPPED, TURBOWASM_EXECUTION_EXCEPTION};
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            create(names[i], NULL, 0);
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED); compiled(names[i]);
            complete_wait(); check_equal(turbowasm_execution_resume(&execution, NULL), statuses[i]);
            check_equal(turbowasm_execution_state_get(&execution), states[i]);
            check_equal(turbowasm_execution_terminal_status(&execution), statuses[i]);
            check_equal(turbowasm_execution_result_count(&execution), 0u);
            if (i == 0) check_equal(turbowasm_execution_trap(&execution), TURBOWASM_TRAP_UNREACHABLE);
            check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_INVALID_ARGUMENT);
            turbowasm_execution_destroy(&execution);
        }
    }
    it("unwinds allocation failure after resuming a native frame") {
        create("allocate-after-wait", NULL, 0);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
        compiled("allocate-after-wait"); complete_wait();
        allocations.attempts = 0; allocations.fail_at = 1;
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OUT_OF_MEMORY);
        allocations.fail_at = 0;
        check_equal(turbowasm_execution_state_get(&execution), TURBOWASM_EXECUTION_FAILED);
        check_equal(turbowasm_execution_result_count(&execution), 0u);
        turbowasm_execution_destroy(&execution); collect_empty();
        host.immediate = true; create("allocate-after-wait", NULL, 0);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        check_equal(query("read", turbowasm_execution_result_at(&execution, 0), 1), COMPLETION);
    }
#ifdef TURBOWASM_TEST_MIR
    it("retains native callers around explicitly interpreted host-wait callees") {
        turbowasm_value args[] = {integer(ITERATIONS), vector()};
        turbowasm_instance_impl *impl = instance.impl;
        impl->jit_functions[function_index("leaf")].state = TURBOWASM_JIT_INTERPRET_ONLY;
        create("values", args, 2);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
        compiled("values"); compiled("direct");
        check_equal(turbowasm_store_collect(&store), TURBOWASM_OK); complete_wait();
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK); finish_values(args[1]);
    }
    it("keeps interpreter behavior unless the attached backend opts into resumable frames") {
        turbowasm_value arg = integer(ITERATIONS);
        turbowasm_instance_impl *impl = instance.impl;
        impl->jit_backend.supports_resumable_execution = false;
        create("direct", &arg, 1);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
        check_equal(impl->jit_functions[function_index("direct")].state, TURBOWASM_JIT_INTERPRET);
        complete_wait(); check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        check_equal(turbowasm_execution_result_at(&execution, 0)->as.i32, ITERATIONS + COMPLETION);
    }
#endif
}
