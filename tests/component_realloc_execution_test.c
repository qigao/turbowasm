#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_realloc_execution.h"
#include "fixtures/component_realloc_execution64.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_RESUMES = 1024, TEXT_SIZE = 5, MODE_TRAP = 1, MODE_THROW = 2, MODE_RECURSE = 3 };
static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_exec_call call;
static turbowasm_component_value result;
static size_t live_allocations;
static unsigned provider_calls;
static bool memory64;

static void *allocate(void *context, size_t size) {
    void *p = malloc(size); (void)context;
    if (p != NULL) ++live_allocations;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(live_allocations != 0u); --live_allocations; }
    free(p);
}
static bool can_bind(void *context, turbowasm_component_name instance,
    turbowasm_component_name function, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type) {
    (void)context; (void)instance; (void)function; (void)graph; (void)type;
    return true;
}
static turbowasm_status text_host(void *context, turbowasm_host_call *host,
    turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    const turbowasm_component_value *arguments, size_t count,
    turbowasm_component_value *out, turbowasm_trap *trap) {
    (void)context; (void)host; (void)instance; (void)function; (void)graph; (void)type; (void)arguments;
    check_equal(count, 0u); ++provider_calls;
    out->as.string.data = turbowasm_rt_malloc(TEXT_SIZE);
    if (out->as.string.data == NULL) return TURBOWASM_OUT_OF_MEMORY;
    out->kind = TURBOWASM_COMPONENT_TYPE_STRING;
    out->as.string.size = TEXT_SIZE;
    memcpy(out->as.string.data, "hello", TEXT_SIZE);
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static void setup(bool wide) {
    turbowasm_runtime_config config;
    turbowasm_component_exec_imports imports = {0};
    memory64 = wide; provider_calls = 0u;
    turbowasm_runtime_config_init(&config);
    config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
    check_equal(turbowasm_component_binary_load_with_config(&binary,
        wide ? component_realloc_execution64_bytes : component_realloc_execution_bytes,
        wide ? sizeof(component_realloc_execution64_bytes) : sizeof(component_realloc_execution_bytes),
        &config), TURBOWASM_OK);
    imports.can_bind = can_bind; imports.invoke = text_host;
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
static void close_fixture(void) {
    check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
    turbowasm_component_exec_call_destroy(&call);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);
    check_equal(live_allocations, 0u);
}
static turbowasm_status invoke(const char *name, const turbowasm_component_value *args,
    size_t count, turbowasm_component_value *out, turbowasm_trap *trap) {
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&binary.config);
    turbowasm_status status = turbowasm_component_exec_invoke_export(&exec,
        (const uint8_t *)name, (uint32_t)strlen(name), args, count, out, trap);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
static uint64_t number(const char *name) {
    turbowasm_component_value out = {0}; turbowasm_trap trap;
    check_equal(invoke(name, NULL, 0u, &out, &trap), TURBOWASM_OK);
    check_equal(out.kind, memory64 ? TURBOWASM_COMPONENT_TYPE_U64 : TURBOWASM_COMPONENT_TYPE_U32);
    return memory64 ? out.as.u64 : out.as.u32;
}
static void mode(unsigned value) {
    turbowasm_component_value arg = {0}; turbowasm_trap trap;
    arg.kind = memory64 ? TURBOWASM_COMPONENT_TYPE_U64 : TURBOWASM_COMPONENT_TYPE_U32;
    if (memory64) arg.as.u64 = value; else arg.as.u32 = value;
    check_equal(invoke("mode", &arg, 1u, NULL, &trap), TURBOWASM_OK);
}
static void create_call(void) {
    check_equal(turbowasm_component_exec_call_create(&call, &exec,
        (const uint8_t *)"run", (uint32_t)strlen("run"), NULL, 0u), TURBOWASM_OK);
}
static turbowasm_status resume_one(void) {
    turbowasm_execution_options options = {0};
    options.has_fuel_limit = true; options.fuel = 1u;
    return turbowasm_component_exec_call_resume(&call, &options);
}
static void suspend_in_realloc(void) {
    unsigned resumes = 0u;
    do {
        check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES);
    } while (number("entered") == 0u);
    check_false(exec.may_leave);
    check_equal(turbowasm_component_exec_call_yield_reason_get(&call), TURBOWASM_YIELD_FUEL);
    check_equal(number("finished"), 0u); check_equal(provider_calls, 1u);
    check_equal(turbowasm_component_exec_call_result_count(&call), 0u);
#ifdef TURBOWASM_TEST_MIR
    {
        const turbowasm_component_exec_realloc_context *context = &exec.canon_lower_contexts[0].realloc_context;
        turbowasm_instance_impl *core = context->instance->impl;
        check_equal(core->jit_functions[context->function_index].state, TURBOWASM_JIT_COMPILED);
    }
#endif
}
static bool interrupt(void *context) { return *(const bool *)context; }

spec("canonical guest realloc execution control") {
    after_each() { close_fixture(); }
    it("shares fuel with memory32 and memory64 realloc without replaying the host provider") {
        unsigned wide;
        for (wide = 0u; wide != 2u; ++wide) {
            turbowasm_status status; unsigned resumes = 0u;
            setup(wide != 0u); create_call(); suspend_in_realloc();
            do { status = resume_one(); check_true(++resumes < MAX_RESUMES); }
            while (status == TURBOWASM_YIELDED);
            check_equal(status, TURBOWASM_OK); check_true(exec.may_leave);
            check_equal(provider_calls, 1u); check_equal(number("finished"), 1u);
            check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK);
            check_equal(result.as.string.size, TEXT_SIZE);
            check_equal(memcmp(result.as.string.data, "hello", TEXT_SIZE), 0);
            close_fixture();
        }
    }
    it("unwinds cancelled realloc and frees the unpublished host result") {
        unsigned wide;
        for (wide = 0u; wide != 2u; ++wide) {
            turbowasm_trap trap;
            setup(wide != 0u); create_call(); suspend_in_realloc();
            turbowasm_component_exec_call_destroy(&call);
            check_true(exec.may_leave); check_equal(provider_calls, 1u);
            check_equal(number("finished"), 0u);
            check_equal(invoke("run", NULL, 0u, &result, &trap), TURBOWASM_OK);
            check_equal(provider_calls, 2u); check_equal(number("entered"), 2u);
            check_equal(number("finished"), 1u);
            close_fixture();
        }
    }
    it("restores canonical leave and publishes no result after realloc traps or throws") {
        unsigned wide, failure;
        for (wide = 0u; wide != 2u; ++wide) {
            setup(wide != 0u);
            for (failure = MODE_TRAP; failure <= MODE_THROW; ++failure) {
                turbowasm_status status; unsigned resumes = 0u;
                mode(failure); create_call();
                do { status = resume_one(); check_true(++resumes < MAX_RESUMES); }
                while (status == TURBOWASM_YIELDED);
                check_equal(status, TURBOWASM_TRAPPED);
                check_equal(turbowasm_component_exec_call_trap(&call), TURBOWASM_TRAP_UNREACHABLE);
                check_equal(turbowasm_component_exec_call_result_count(&call), 0u);
                check_true(exec.may_leave); check_equal(number("finished"), 0u);
                check_equal(provider_calls, failure);
                turbowasm_component_exec_call_destroy(&call);
            }
            close_fixture();
        }
    }
    it("applies the caller depth limit to guest realloc frames") {
        unsigned wide;
        for (wide = 0u; wide != 2u; ++wide) {
            turbowasm_trap trap;
            setup(wide != 0u); mode(MODE_RECURSE);
            check_equal(invoke("run", NULL, 0u, &result, &trap), TURBOWASM_OK);
            check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            check_equal(invoke("deep", NULL, 0u, &result, &trap), TURBOWASM_TRAPPED);
            check_equal(trap, TURBOWASM_TRAP_CALL_STACK_EXHAUSTED);
            check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_true(exec.may_leave); check_equal(number("entered"), 2u);
            check_equal(number("finished"), 1u);
            close_fixture();
        }
    }
    it("shares interruption control through the realloc frame") {
        unsigned wide;
        for (wide = 0u; wide != 2u; ++wide) {
            turbowasm_execution_options options = {0}; bool interrupted = true;
            setup(wide != 0u); create_call(); suspend_in_realloc();
            options.should_interrupt = interrupt; options.interrupt_context = &interrupted;
            check_equal(turbowasm_component_exec_call_resume(&call, &options), TURBOWASM_YIELDED);
            check_equal(turbowasm_component_exec_call_yield_reason_get(&call), TURBOWASM_YIELD_INTERRUPTION);
            interrupted = false;
            check_equal(turbowasm_component_exec_call_resume(&call, &options), TURBOWASM_OK);
            check_true(exec.may_leave); check_equal(provider_calls, 1u);
            check_equal(number("entered"), 1u); check_equal(number("finished"), 1u);
            close_fixture();
        }
    }
}
