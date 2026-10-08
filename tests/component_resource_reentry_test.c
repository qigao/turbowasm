#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_resource_reentry.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_RESUMES = 1024, MODE_NORMAL = 0, MODE_TRAP, MODE_THROW, MODE_RECURSE, MODE_WAIT };
enum { WAIT_TOKEN = 42, WAIT_RESULT = 7 };
static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_exec_call calls[2];
static size_t live_allocations;
static unsigned submitted, completed, cancelled;

static void *allocate(void *context, size_t size) {
    void *p = malloc(size);
    (void)context;
    if (p != NULL) ++live_allocations;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(live_allocations != 0u); --live_allocations; }
    free(p);
}
static turbowasm_status invoke(const char *name,
    const turbowasm_component_value *arguments, size_t count,
    turbowasm_component_value *result, turbowasm_trap *trap) {
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&binary.config);
    turbowasm_status status = turbowasm_component_exec_invoke_export(&exec,
        (const uint8_t *)name, (uint32_t)strlen(name), arguments, count, result, trap);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
static uint32_t number(const char *name) {
    turbowasm_component_value result = {0}; turbowasm_trap trap;
    check_equal(invoke(name, NULL, 0u, &result, &trap), TURBOWASM_OK);
    check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_U32);
    return result.as.u32;
}
static void make(uint32_t mode) {
    turbowasm_component_value value = {0}; turbowasm_trap trap;
    value.kind = TURBOWASM_COMPONENT_TYPE_U32; value.as.u32 = mode;
    check_equal(invoke("make", &value, 1u, NULL, &trap), TURBOWASM_OK);
}
static void create_call(size_t index) {
    check_equal(turbowasm_component_exec_call_create(&calls[index], &exec,
        (const uint8_t *)"cleanup", (uint32_t)strlen("cleanup"), NULL, 0u), TURBOWASM_OK);
}
static turbowasm_status resume_one(size_t index) {
    turbowasm_execution_options options = {0};
    options.has_fuel_limit = true; options.fuel = 1u;
    return turbowasm_component_exec_call_resume(&calls[index], &options);
}
static void suspend_in_destructor(size_t index, uint32_t entered) {
    unsigned resumes = 0u;
    do {
        check_equal(resume_one(index), TURBOWASM_YIELDED);
        check_true(++resumes < MAX_RESUMES);
    } while (number("entered") != entered);
    check_equal(turbowasm_component_exec_call_yield_reason_get(&calls[index]), TURBOWASM_YIELD_FUEL);
    check_equal(exec.resource_table.live_count, 0u);
#ifdef TURBOWASM_TEST_MIR
    {
        uint32_t i; bool checked = false;
        for (i = 0u; i < binary.type_graph.count; ++i) {
            const turbowasm_component_type *type = &binary.type_graph.types[i];
            if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE && type->as.resource.has_destructor) {
                const turbowasm_component_exec_core_function *fn =
                    &exec.core_functions[type->as.resource.destructor_index];
                turbowasm_instance_impl *core = exec.core_instances[fn->instance_index].impl;
                check_equal(core->jit_functions[fn->function_index].state, TURBOWASM_JIT_COMPILED);
                checked = true;
            }
        }
        check_true(checked);
    }
#endif
}
static void finish(size_t index) {
    turbowasm_status status; unsigned resumes = 0u;
    do {
        status = resume_one(index); check_true(++resumes < MAX_RESUMES);
    } while (status == TURBOWASM_YIELDED);
    check_equal(status, TURBOWASM_OK);
    check_equal(turbowasm_component_exec_call_trap(&calls[index]), TURBOWASM_TRAP_NONE);
}
static bool interrupt(void *context) { return *(const bool *)context; }
static bool can_bind(void *context, turbowasm_component_name instance,
    turbowasm_component_name function, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type) {
    (void)context; (void)instance; (void)function; (void)graph; (void)type;
    return true;
}
static turbowasm_status wait_host(void *context, turbowasm_host_call *call,
    turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    const turbowasm_component_value *arguments, size_t count,
    turbowasm_component_value *result, turbowasm_trap *trap) {
    turbowasm_host_wait wait = {0}; int completion = 0; turbowasm_status status;
    (void)context; (void)instance; (void)function; (void)graph; (void)type;
    (void)arguments; (void)result;
    check_equal(count, 0u); check_true(turbowasm_host_call_can_wait(call));
    ++submitted;
    status = turbowasm_host_call_wait(call, WAIT_TOKEN, &wait, &completion);
    if (status == TURBOWASM_INTERRUPTED) ++cancelled;
    if (status != TURBOWASM_OK) return status;
    check_equal(completion, WAIT_RESULT); ++completed;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

spec("canonical resource destructor re-entry") {
    before_each() {
        turbowasm_runtime_config config;
        turbowasm_component_exec_imports imports = {0};
        submitted = completed = cancelled = 0u;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_binary_load_with_config(&binary,
            component_resource_reentry_bytes, sizeof(component_resource_reentry_bytes), &config), TURBOWASM_OK);
        imports.can_bind = can_bind; imports.invoke = wait_host;
        check_equal(turbowasm_component_exec_init_with_imports(&exec, &binary, &imports), TURBOWASM_OK);
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
        turbowasm_component_exec_call_destroy(&calls[0]);
        turbowasm_component_exec_call_destroy(&calls[1]);
        turbowasm_component_exec_destroy(&exec);
        turbowasm_component_binary_destroy(&binary);
        check_equal(live_allocations, 0u);
    }
    it("shares fuel through destructor suspension and consumes the handle once") {
        make(MODE_NORMAL); create_call(0u); suspend_in_destructor(0u, 1u);
        check_equal(number("finished"), 0u);
        check_equal(turbowasm_component_exec_call_result_count(&calls[0]), 0u);
        finish(0u); check_equal(number("entered"), 1u); check_equal(number("finished"), 1u);
        check_equal(resume_one(0u), TURBOWASM_INVALID_ARGUMENT);
        check_equal(exec.resource_table.live_count, 0u);
    }
    it("cancels a suspended destructor without restoring or replaying its handle") {
        turbowasm_trap trap;
        make(MODE_NORMAL); create_call(0u); suspend_in_destructor(0u, 1u);
        turbowasm_component_exec_call_destroy(&calls[0]);
        check_equal(number("entered"), 1u); check_equal(number("finished"), 0u);
        check_equal(exec.resource_table.live_count, 0u);
        check_not_equal(invoke("cleanup", NULL, 0u, NULL, &trap), TURBOWASM_OK);
        check_equal(number("entered"), 1u);
        make(MODE_NORMAL); check_equal(invoke("cleanup", NULL, 0u, NULL, &trap), TURBOWASM_OK);
        check_equal(number("entered"), 2u); check_equal(number("finished"), 1u);
    }
    it("propagates destructor traps and converts uncaught Core exceptions to traps") {
        uint32_t mode;
        for (mode = MODE_TRAP; mode <= MODE_THROW; ++mode) {
            turbowasm_status status; unsigned resumes = 0u;
            make(mode); create_call(0u);
            do { status = resume_one(0u); check_true(++resumes < MAX_RESUMES); }
            while (status == TURBOWASM_YIELDED);
            check_equal(status, TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_exec_call_trap(&calls[0]), TURBOWASM_TRAP_UNREACHABLE);
            check_equal(exec.resource_table.live_count, 0u);
            check_equal(number("entered"), mode); check_equal(number("finished"), 0u);
            turbowasm_component_exec_call_destroy(&calls[0]);
        }
    }
    it("counts caller and destructor frames against one call-depth limit") {
        turbowasm_trap trap;
        make(MODE_RECURSE);
        check_equal(invoke("cleanup", NULL, 0u, NULL, &trap), TURBOWASM_OK);
        make(MODE_RECURSE);
        check_equal(invoke("deep-cleanup", NULL, 0u, NULL, &trap), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_CALL_STACK_EXHAUSTED);
        check_equal(number("entered"), 2u); check_equal(number("finished"), 1u);
        check_equal(exec.resource_table.live_count, 0u);
        make(MODE_NORMAL);
        check_equal(invoke("cleanup", NULL, 0u, NULL, &trap), TURBOWASM_OK);
    }
    it("keeps control contexts distinct when two destructor calls are suspended") {
        make(MODE_NORMAL); create_call(0u); suspend_in_destructor(0u, 1u);
        make(MODE_NORMAL); create_call(1u); suspend_in_destructor(1u, 2u);
        finish(0u); check_equal(number("finished"), 1u);
        turbowasm_component_exec_call_destroy(&calls[1]);
        check_equal(number("entered"), 2u); check_equal(number("finished"), 1u);
        check_equal(exec.resource_table.live_count, 0u);
    }
    it("observes interruption in the suspended destructor and resumes without replay") {
        turbowasm_execution_options options = {0}; bool interrupted = true;
        make(MODE_NORMAL); create_call(0u); suspend_in_destructor(0u, 1u);
        options.should_interrupt = interrupt; options.interrupt_context = &interrupted;
        check_equal(turbowasm_component_exec_call_resume(&calls[0], &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_exec_call_yield_reason_get(&calls[0]), TURBOWASM_YIELD_INTERRUPTION);
        check_equal(number("entered"), 1u); check_equal(number("finished"), 0u);
        interrupted = false;
        check_equal(turbowasm_component_exec_call_resume(&calls[0], &options), TURBOWASM_OK);
        check_equal(number("entered"), 1u); check_equal(number("finished"), 1u);
    }
    it("retains the original host-wait owner through a destructor callback") {
        turbowasm_host_wait wait = {0};
        make(MODE_WAIT); create_call(0u);
        check_equal(turbowasm_component_exec_call_resume(&calls[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_exec_call_yield_reason_get(&calls[0]), TURBOWASM_YIELD_HOST_WAIT);
        check_true(turbowasm_component_exec_call_pending_host_wait(&calls[0], &wait));
        check_equal(wait.operation_token, WAIT_TOKEN);
        check_equal(submitted, 1u); check_equal(completed, 0u);
        check_equal(exec.resource_table.live_count, 0u);
        check_equal(turbowasm_component_exec_call_complete_host_wait(&calls[0], wait, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_call_resume(&calls[0], NULL), TURBOWASM_OK);
        check_equal(submitted, 1u); check_equal(completed, 1u); check_equal(cancelled, 0u);
        check_equal(number("finished"), 1u);
        turbowasm_component_exec_call_destroy(&calls[0]);
        make(MODE_WAIT); create_call(0u);
        check_equal(turbowasm_component_exec_call_resume(&calls[0], NULL), TURBOWASM_YIELDED);
        turbowasm_component_exec_call_destroy(&calls[0]);
        check_equal(submitted, 2u); check_equal(completed, 1u); check_equal(cancelled, 1u);
        check_equal(number("entered"), 2u); check_equal(number("finished"), 1u);
        check_equal(exec.resource_table.live_count, 0u);
    }
}
