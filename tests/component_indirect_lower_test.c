#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_indirect_lower.h"
#include "fixtures/component_indirect_lower64.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { PARAM_COUNT = 17, RESULT_COUNT = 2, EXPECTED_SUM = 127, MAX_RESUMES = 1024, MAX_FAILURES = 128 };
static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_exec_call call;
static turbowasm_component_value result;
static struct { size_t live, attempts, fail_at; } allocations;
static unsigned provider_calls;
static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++allocations.live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(allocations.live != 0u); --allocations.live; }
    free(p);
}
static bool can_bind(void *context, turbowasm_component_name instance,
    turbowasm_component_name function, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type) {
    (void)context; (void)instance; (void)function; (void)graph; (void)type; return true;
}
static turbowasm_status host_wide(void *context, turbowasm_host_call *host,
    turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    const turbowasm_component_value *args, size_t count,
    turbowasm_component_value *out, turbowasm_trap *trap) {
    turbowasm_component_value *items; size_t i; uint32_t sum;
    (void)context; (void)host; (void)instance; (void)function; (void)graph; (void)type;
    check_equal(count, PARAM_COUNT); ++provider_calls;
    check_equal(args[0].kind, TURBOWASM_COMPONENT_TYPE_U8); check_equal(args[0].as.u8, 7u);
    check_equal(args[1].kind, TURBOWASM_COMPONENT_TYPE_U64);
    check_equal(args[1].as.u64, UINT64_C(4294967296)); sum = args[0].as.u8;
    for (i = 2u; i < count; ++i) {
        check_equal(args[i].kind, TURBOWASM_COMPONENT_TYPE_U32);
        check_equal(args[i].as.u32, i - 1u); sum += args[i].as.u32;
    }
    items = turbowasm_rt_calloc(RESULT_COUNT, sizeof(*items));
    if (items == NULL) return TURBOWASM_OUT_OF_MEMORY;
    items[0].kind = TURBOWASM_COMPONENT_TYPE_U32; items[0].as.u32 = sum;
    items[1] = args[1];
    out->kind = TURBOWASM_COMPONENT_TYPE_TUPLE;
    out->as.tuple.items = items; out->as.tuple.count = RESULT_COUNT;
    *trap = TURBOWASM_TRAP_NONE; return TURBOWASM_OK;
}
static void setup(bool wide) {
    turbowasm_runtime_config config; turbowasm_component_exec_imports imports = {0};
    provider_calls = 0u;
    turbowasm_runtime_config_init(&config);
    config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
    check_equal(turbowasm_component_binary_load_with_config(&binary,
        wide ? component_indirect_lower64_bytes : component_indirect_lower_bytes,
        wide ? sizeof(component_indirect_lower64_bytes) : sizeof(component_indirect_lower_bytes), &config), TURBOWASM_OK);
    imports.can_bind = can_bind; imports.invoke = host_wide;
    check_equal(turbowasm_component_exec_init_with_imports(&exec, &binary, &imports), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    {
        uint32_t i;
        for (i = 0u; i < exec.core_instance_count; ++i) {
            turbowasm_jit_backend backend = {0};
            if (exec.core_instances[i].impl == NULL) continue;
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl, &backend, 1u), TURBOWASM_OK);
        }
    }
#endif
}
static void close_fixture(void) {
    allocations.fail_at = 0u;
    check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
    turbowasm_component_exec_call_destroy(&call);
    turbowasm_component_exec_destroy(&exec); turbowasm_component_binary_destroy(&binary);
    check_equal(allocations.live, 0u);
}
static turbowasm_status invoke(const char *name, turbowasm_component_value *out) {
    turbowasm_trap trap;
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&binary.config);
    turbowasm_status status = turbowasm_component_exec_invoke_export(&exec,
        (const uint8_t *)name, (uint32_t)strlen(name), NULL, 0u, out, &trap);
    turbowasm_runtime_scope_leave(scope); return status;
}
static uint32_t calls(void) {
    turbowasm_component_value out = {0};
    check_equal(invoke("calls", &out), TURBOWASM_OK); return out.as.u32;
}
static void expect_pair(void) {
    check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_TUPLE);
    check_equal(result.as.tuple.count, RESULT_COUNT);
    check_equal(result.as.tuple.items[0].as.u32, EXPECTED_SUM);
    check_equal(result.as.tuple.items[1].as.u64, UINT64_C(4294967296));
}
static void create_call(void) {
    check_equal(turbowasm_component_exec_call_create(&call, &exec,
        (const uint8_t *)"local", (uint32_t)strlen("local"), NULL, 0u), TURBOWASM_OK);
}
static turbowasm_status resume_one(void) {
    turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 1u;
    return turbowasm_component_exec_call_resume(&call, &options);
}
spec("canonical indirect parameter lowering") {
    after_each() { close_fixture(); }
    it("uses aligned mixed parameter tuples and separate indirect result pointers on both memories") {
        unsigned wide; const char *names[] = {"local", "external", "aggregate", "edge"}; size_t i;
        for (wide = 0u; wide != 2u; ++wide) {
            setup(wide != 0u);
            for (i = 0u; i != sizeof(names) / sizeof(*names); ++i) {
                check_equal(invoke(names[i], &result), TURBOWASM_OK); expect_pair();
                check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            }
            check_equal(provider_calls, 2u); check_equal(calls(), 2u); close_fixture();
        }
    }
    it("rejects misaligned out-of-bounds and overflowing tuples before provider admission") {
        unsigned wide; const char *names[] = {"misaligned", "bounds", "overflow"}; size_t i;
        for (wide = 0u; wide != 2u; ++wide) {
            setup(wide != 0u);
            for (i = 0u; i != sizeof(names) / sizeof(*names); ++i) {
                check_equal(invoke(names[i], &result), TURBOWASM_TRAPPED);
                check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
                check_equal(provider_calls, 0u); check_equal(calls(), 0u);
            }
            close_fixture();
        }
    }
    it("retains the full argument array across local fuel suspension and cancellation") {
        unsigned wide;
        for (wide = 0u; wide != 2u; ++wide) {
            turbowasm_status status; unsigned resumes = 0u;
            setup(wide != 0u); create_call();
            do {
                check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES);
            } while (calls() == 0u);
#ifdef TURBOWASM_TEST_MIR
            {
                turbowasm_component_core_call_adapter *adapter = &exec.functions[0];
                turbowasm_instance_impl *core = adapter->instance->impl;
                check_equal(core->jit_functions[adapter->function_index].state, TURBOWASM_JIT_COMPILED);
            }
#endif
            do { status = resume_one(); check_true(++resumes < MAX_RESUMES); }
            while (status == TURBOWASM_YIELDED);
            check_equal(status, TURBOWASM_OK);
            check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK); expect_pair();
            check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            turbowasm_component_exec_call_destroy(&call); create_call(); resumes = 0u;
            do { check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES); }
            while (calls() != 2u);
            turbowasm_component_exec_call_destroy(&call); close_fixture();
        }
    }
    it("unwinds every allocation failure in large argument and result arrays") {
        unsigned wide; const char *names[] = {"local", "external"}; size_t i;
        for (wide = 0u; wide != 2u; ++wide) {
            setup(wide != 0u);
            for (i = 0u; i != 2u; ++i) {
                size_t offset, baseline; bool succeeded = false;
                check_equal(invoke(names[i], &result), TURBOWASM_OK);
                check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK); baseline = allocations.live;
                for (offset = 1u; offset != MAX_FAILURES; ++offset) {
                    turbowasm_status status;
                    allocations.fail_at = allocations.attempts + offset;
                    status = invoke(names[i], &result); allocations.fail_at = 0u;
                    if (status == TURBOWASM_OK) {
                        expect_pair(); check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
                        succeeded = true; break;
                    }
                    check_equal(status, TURBOWASM_OUT_OF_MEMORY);
                    check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
                    check_equal(allocations.live, baseline);
                }
                check_true(succeeded);
            }
            close_fixture();
        }
    }
}
