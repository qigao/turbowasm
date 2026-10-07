#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_local_lower.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_RESUMES = 1024, MAX_FAILURES = 128, MAX_INIT_FAILURES = 4096 };
static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_exec_call call;
static turbowasm_component_value result;
static struct { size_t live, attempts, fail_at; } allocations;
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
static turbowasm_status invoke(const char *name, turbowasm_component_value *out) {
    turbowasm_trap trap;
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&binary.config);
    turbowasm_status status = turbowasm_component_exec_invoke_export(&exec,
        (const uint8_t *)name, (uint32_t)strlen(name), NULL, 0u, out, &trap);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
static uint32_t number(const char *name) {
    turbowasm_component_value out = {0};
    check_equal(invoke(name, &out), TURBOWASM_OK);
    check_equal(out.kind, TURBOWASM_COMPONENT_TYPE_U32); return out.as.u32;
}
static void create_call(const char *name) {
    check_equal(turbowasm_component_exec_call_create(&call, &exec,
        (const uint8_t *)name, (uint32_t)strlen(name), NULL, 0u), TURBOWASM_OK);
}
static turbowasm_status resume_one(void) {
    turbowasm_execution_options options = {0};
    options.has_fuel_limit = true; options.fuel = 1u;
    return turbowasm_component_exec_call_resume(&call, &options);
}
static void expect_text(void) {
    static const uint8_t text[] = {0x41,0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
    check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_STRING);
    check_equal(result.as.string.size, sizeof(text));
    check_equal(memcmp(result.as.string.data, text, sizeof(text)), 0);
}
spec("local canonical lowering") {
    before_each() {
        turbowasm_runtime_config config;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_binary_load_with_config(&binary,
            component_local_lower_bytes, sizeof(component_local_lower_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        {
            uint32_t i;
            for (i = 0u; i < exec.core_instance_count; ++i) {
                turbowasm_jit_backend backend = {0};
                if (exec.core_instances[i].impl == NULL) continue;
                check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
                check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl,
                    &backend, 1u), TURBOWASM_OK);
            }
        }
#endif
    }
    after_each() {
        allocations.fail_at = 0u;
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        turbowasm_component_exec_call_destroy(&call);
        turbowasm_component_exec_destroy(&exec); turbowasm_component_binary_destroy(&binary);
        check_equal(allocations.live, 0u);
    }
    it("resolves local export and instance aliases before consumer start runs") {
        check_equal(number("started"), 42u);
        check_equal(number("calls"), 1u); check_equal(number("posts"), 1u);
        check_equal(number("run"), 42u);
        check_equal(number("calls"), 2u); check_equal(number("posts"), 2u);
    }
    it("transcodes a local string across memory64 UTF8 and memory32 UTF16") {
        check_equal(invoke("text", &result), TURBOWASM_OK); expect_text();
    }
    it("moves owned resources and lends local abstract representations") {
        check_equal(number("resources"), 42u);
        check_equal(number("drops"), 1u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("suspends local post-return in the caller coroutine without replay") {
        turbowasm_status status; unsigned resumes = 0u; bool post_yield = false;
        create_call("run");
        do {
            status = resume_one(); check_true(++resumes < MAX_RESUMES);
            if (!exec.may_leave) {
                post_yield = true;
                check_equal(status, TURBOWASM_YIELDED);
                check_equal(turbowasm_component_exec_call_result_count(&call), 0u);
#ifdef TURBOWASM_TEST_MIR
                {
                    const turbowasm_component_core_call_adapter *adapter = &exec.functions[0];
                    turbowasm_instance_impl *core = adapter->instance->impl;
                    check_equal(core->jit_functions[adapter->function_index].state, TURBOWASM_JIT_COMPILED);
                    core = adapter->post_return_instance->impl;
                    check_equal(core->jit_functions[adapter->post_return_function_index].state, TURBOWASM_JIT_COMPILED);
                }
#endif
            }
        } while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK); check_true(post_yield);
        check_true(exec.may_leave);
        check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK);
        check_equal(result.as.u32, 42u); check_equal(number("calls"), 2u); check_equal(number("posts"), 2u);
    }
    it("unwinds local post-return cancellation and permits a later call") {
        unsigned resumes = 0u;
        create_call("run");
        do { check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES); }
        while (exec.may_leave);
        turbowasm_component_exec_call_destroy(&call);
        check_true(exec.may_leave); check_equal(number("calls"), 2u);
        check_equal(number("run"), 42u); check_equal(number("calls"), 3u);
    }
    it("retains local resource loans and strings across fuel suspension") {
        const char *names[] = {"resources", "text"}; unsigned i;
        for (i = 0u; i != 2u; ++i) {
            turbowasm_status status; unsigned resumes = 0u;
            create_call(names[i]);
            do { status = resume_one(); check_true(++resumes < MAX_RESUMES); }
            while (status == TURBOWASM_YIELDED);
            check_equal(status, TURBOWASM_OK);
            check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK);
            if (i == 0u) check_equal(result.as.u32, 42u); else expect_text();
            check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            turbowasm_component_exec_call_destroy(&call);
        }
        check_equal(exec.resource_table.live_count, 0u); check_equal(number("drops"), 1u);
    }
    it("releases every private string allocation when local conversion fails") {
        size_t offset, baseline; bool succeeded = false;
        check_equal(invoke("text", &result), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        baseline = allocations.live;
        for (offset = 1u; offset != MAX_FAILURES; ++offset) {
            turbowasm_status status;
            allocations.fail_at = allocations.attempts + offset;
            status = invoke("text", &result); allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                expect_text(); check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
                succeeded = true; break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_true(exec.may_leave); check_equal(allocations.live, baseline);
        }
        check_true(succeeded);
    }
    it("releases a local owned result exactly once when post-return fails") {
        turbowasm_status status; unsigned resumes = 0u;
        check_equal(invoke("resources-fail", &result), TURBOWASM_TRAPPED);
        check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(exec.resource_table.live_count, 0u); check_equal(number("drops"), 1u);
        create_call("resources-fail");
        do { status = resume_one(); check_true(++resumes < MAX_RESUMES); }
        while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_exec_call_trap(&call), TURBOWASM_TRAP_UNREACHABLE);
        check_equal(exec.resource_table.live_count, 0u); check_equal(number("drops"), 2u);
    }
    it("releases a cancelled local borrow so the original owner can be dropped") {
        unsigned resumes = 0u;
        create_call("resources");
        do { check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES); }
        while (number("borrows") == 0u);
        check_equal(exec.resource_table.live_count, 1u);
        check_not_equal(invoke("cleanup", NULL), TURBOWASM_OK);
        check_equal(number("drops"), 0u);
        turbowasm_component_exec_call_destroy(&call);
        check_equal(exec.resource_table.live_count, 1u);
        check_equal(invoke("cleanup", NULL), TURBOWASM_OK);
        check_equal(number("drops"), 1u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("commits a composite owned result only after every field lowers successfully") {
        check_equal(number("pair"), 42u);
        check_equal(number("drops"), 1u); check_equal(exec.resource_table.live_count, 0u);
        check_equal(invoke("fail-realloc", NULL), TURBOWASM_OK);
        check_equal(invoke("pair", &result), TURBOWASM_TRAPPED);
        check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(number("drops"), 2u); check_equal(exec.resource_table.live_count, 0u);
        check_true(exec.may_leave);
    }
    it("cleans early function maps and lazy start adapters after allocation failure") {
        size_t offset, baseline; bool succeeded = false;
        turbowasm_component_exec_destroy(&exec); baseline = allocations.live;
        for (offset = 1u; offset != MAX_INIT_FAILURES; ++offset) {
            turbowasm_status status;
            allocations.fail_at = allocations.attempts + offset;
            status = turbowasm_component_exec_init(&exec, &binary);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                check_equal(number("started"), 42u); succeeded = true;
                turbowasm_component_exec_destroy(&exec); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_false(exec.initialized);
            turbowasm_component_exec_destroy(&exec);
            check_equal(allocations.live, baseline);
        }
        check_true(succeeded); check_equal(allocations.live, baseline);
    }
}
