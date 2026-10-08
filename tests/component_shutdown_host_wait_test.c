#include "component_api_internal.h"
#include "instance_internal.h"
#include "fixtures/component_resource_reentry.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { INSTANCE_COUNT = 2, MAX_POLLS = 1024, MODE_WAIT = 4, WAIT_TOKEN = 42, WAIT_RESULT = 7 };
typedef struct wait_context {
    turbowasm_component_instance instance;
    turbowasm_component_instance_public_impl *impl;
    unsigned submitted, completed, reentries;
    bool spawn_once;
} wait_context;
static turbowasm_component component;
static wait_context contexts[INSTANCE_COUNT];
static size_t live_allocations, allocation_calls, fail_at;
static wait_context *allocator_reentry;

static void reject_reentry(wait_context *context) {
    turbowasm_component_shutdown_wait wait = {0};
    void *carrier = context->instance.impl;
    ++context->reentries;
    check_equal(turbowasm_component_instance_shutdown_yield_reason_private(context->impl), TURBOWASM_YIELD_NONE);
    check_false(turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, &wait));
    check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, WAIT_RESULT),
        TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_component_instance_request_shutdown_private(context->impl), TURBOWASM_INVALID_ARGUMENT);
    turbowasm_component_instance_destroy(&context->instance);
    check_true(context->instance.impl == carrier);
}
static void *allocate(void *context, size_t size) {
    void *p;
    (void)context; ++allocation_calls;
    if (fail_at != 0u && allocation_calls == fail_at) return NULL;
    if (allocator_reentry != NULL) {
        wait_context *target = allocator_reentry;
        allocator_reentry = NULL;
        reject_reentry(target);
    }
    p = malloc(size);
    if (p != NULL) ++live_allocations;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(live_allocations != 0u); --live_allocations; }
    free(p);
}
static turbowasm_component_core_call_adapter *binding(wait_context *context, const char *name) {
    const turbowasm_component_binary *binary = context->impl->exec.binary;
    uint32_t i;
    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *item = &binary->exports[i];
        if (item->kind == TURBOWASM_COMPONENT_EXTERN_FUNCTION && item->name.size == strlen(name) &&
            memcmp(item->name.bytes, name, item->name.size) == 0)
            return &context->impl->exec.functions[context->impl->exec.function_adapter_indices[item->item_index]];
    }
    check(false); return NULL;
}
/* Inspect the fixture's Core counters after Component admission has closed. */
static uint32_t number(wait_context *context, const char *name) {
    turbowasm_component_core_call_adapter *fn = binding(context, name);
    turbowasm_value result = {0}; size_t count = 0u; turbowasm_trap trap;
    check_equal(turbowasm_instance_invoke(fn->instance, fn->function_index, NULL, 0u,
        &result, 1u, &count, &trap), TURBOWASM_OK);
    check_equal(count, (size_t)1); check_equal(result.kind, TURBOWASM_VALUE_I32);
    return (uint32_t)result.as.i32;
}
static void make(wait_context *context) {
    turbowasm_component_host_value argument = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = MODE_WAIT};
    turbowasm_name name = {(const uint8_t *)"make", 4u};
    size_t count = 99u; turbowasm_trap trap;
    check_equal(turbowasm_component_instance_invoke(&context->instance, name, &argument, 1u,
        NULL, 0u, &count, &trap), TURBOWASM_OK);
    check_equal(count, (size_t)0);
}
static bool can_bind(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    (void)context; (void)instance; (void)function; (void)graph; (void)type;
    return true;
}
static turbowasm_status wait_host(void *opaque, turbowasm_host_call *call,
    turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    const turbowasm_component_value *arguments, size_t count,
    turbowasm_component_value *result, turbowasm_trap *trap) {
    wait_context *context = opaque;
    turbowasm_host_wait wait = {0}; int completion = 0;
    turbowasm_status status;
    (void)instance; (void)function; (void)graph; (void)type; (void)arguments; (void)result;
    check_equal(count, (size_t)0); check_true(turbowasm_host_call_can_wait(call));
    reject_reentry(context); ++context->submitted;
    status = turbowasm_host_call_wait(call, WAIT_TOKEN, &wait, &completion);
    if (status != TURBOWASM_OK) return status;
    ++context->completed; reject_reentry(context);
    if (completion != WAIT_RESULT) { *trap = TURBOWASM_TRAP_UNREACHABLE; return TURBOWASM_TRAPPED; }
    if (context->spawn_once) {
        turbowasm_component_core_call_adapter *fn = binding(context, "make");
        turbowasm_value mode = {.kind = TURBOWASM_VALUE_I32, .as.i32 = MODE_WAIT};
        size_t created = 99u;
        context->spawn_once = false;
        /* Real nested guest resource.new reuses the just-consumed table slot;
         * that earlier slot belongs to the next finite cleanup pass. */
        status = turbowasm_instance_invoke_from_host(call, fn->instance, fn->function_index,
            &mode, 1u, NULL, 0u, &created, trap);
        if (status != TURBOWASM_OK) return status;
        check_equal(created, (size_t)0);
    }
    *trap = TURBOWASM_TRAP_NONE; return TURBOWASM_OK;
}
static void create(unsigned index) {
    turbowasm_component_exec_async_limits limits = {1u, 8u};
    turbowasm_component_exec_imports imports = {0};
    wait_context *context = &contexts[index];
    imports.context = context; imports.can_bind = can_bind; imports.invoke = wait_host;
    check_equal(turbowasm_component_instance_create_async_with_import_sets_private(&context->instance,
        &component, &limits, &imports, 1u), TURBOWASM_OK);
    context->impl = turbowasm_component_instance_public_impl_get(&context->instance);
#ifdef TURBOWASM_TEST_MIR
    {
        uint32_t i;
        for (i = 0u; i < context->impl->exec.core_instance_count; ++i) {
            turbowasm_jit_backend backend = {0};
            if (context->impl->exec.core_instances[i].impl == NULL) continue;
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(context->impl->exec.core_instances[i].impl,
                &backend, 1u), TURBOWASM_OK);
        }
    }
#endif
}
static turbowasm_component_shutdown_wait pending(wait_context *context) {
    turbowasm_component_shutdown_wait wait = {0};
    check_equal(turbowasm_component_instance_shutdown_yield_reason_private(context->impl), TURBOWASM_YIELD_HOST_WAIT);
    check_true(turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, &wait));
    check_true(wait.instance == context->impl); check_equal(wait.wait.operation_token, (uintptr_t)WAIT_TOKEN);
#ifdef TURBOWASM_TEST_MIR
    {
        const turbowasm_component_binary *binary = context->impl->exec.binary;
        uint32_t i; bool checked = false;
        for (i = 0u; i < binary->type_graph.count; ++i) {
            const turbowasm_component_type *type = &binary->type_graph.types[i];
            if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE && type->as.resource.has_destructor) {
                const turbowasm_component_exec_core_function *fn =
                    &context->impl->exec.core_functions[type->as.resource.destructor_index];
                turbowasm_instance_impl *core = context->impl->exec.core_instances[fn->instance_index].impl;
                check_equal(core->jit_functions[fn->function_index].state, TURBOWASM_JIT_COMPILED); checked = true;
            }
        }
        check_true(checked);
    }
#endif
    return wait;
}
static void start(wait_context *context) {
    check_equal(turbowasm_component_instance_request_shutdown_private(context->impl), TURBOWASM_OK);
    check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
}
static bool interrupt(void *context) { return *(bool *)context; }

spec("Component shutdown destructor host waits") {
    before_each() {
        turbowasm_runtime_config config;
        memset(contexts, 0, sizeof(contexts)); allocator_reentry = NULL; fail_at = 0u;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_async_private(&component, component_resource_reentry_bytes,
            sizeof(component_resource_reentry_bytes), &config), TURBOWASM_OK);
        create(0u);
    }
    after_each() {
        unsigned i;
        allocator_reentry = NULL; fail_at = 0u;
        for (i = 0u; i < INSTANCE_COUNT; ++i) {
            wait_context *context = &contexts[i];
            unsigned polls = 0u;
            if (context->impl == NULL) continue;
            context->spawn_once = false;
            if (!context->impl->admission_closed)
                (void)turbowasm_component_instance_request_shutdown_private(context->impl);
            while (!context->impl->shutdown_complete && polls++ < MAX_POLLS) {
                turbowasm_component_shutdown_wait wait = {0};
                if (turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, &wait))
                    (void)turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, WAIT_RESULT);
                (void)turbowasm_component_instance_poll_shutdown_private(context->impl, NULL);
            }
            check_true(context->impl->shutdown_complete);
            turbowasm_component_instance_destroy(&context->instance); context->impl = NULL;
            check_null(context->instance.impl);
        }
        turbowasm_component_destroy(&component); check_equal(live_allocations, (size_t)0);
    }
    it("keeps an imported destructor and instance alive until real host completion") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait wait;
        void *carrier = context->instance.impl; size_t allocations;
        make(context); start(context); wait = pending(context);
        check_equal(context->submitted, 1u); check_equal(context->completed, 0u);
        check_equal(context->impl->exec.resource_table.live_count, 0u);
        check_equal(number(context, "entered"), 1u); check_equal(number(context, "finished"), 0u);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        check_equal(context->submitted, 1u);
        turbowasm_component_destroy(&component);
        turbowasm_component_instance_destroy(&context->instance); check_true(context->instance.impl == carrier);
        allocations = allocation_calls;
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, WAIT_RESULT), TURBOWASM_OK);
        check_equal(allocation_calls, allocations); check_equal(context->completed, 0u);
        check_false(context->impl->shutdown_complete);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_OK);
        check_equal(context->completed, 1u); check_equal(number(context, "finished"), 1u);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_OK);
        check_equal(context->submitted, 1u);
        check_false(turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, &wait));
    }
    it("authenticates successive waits and rejects wrong tokens, generations and repeated completion") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait first, second, wrong;
        make(context); make(context); start(context); first = pending(context);
        wrong = first; ++wrong.wait.operation_token;
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wrong, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        wrong = first; ++wrong.wait.generation;
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wrong, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, first, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, first, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        second = pending(context);
        check_equal(second.shutdown_generation, first.shutdown_generation);
        check_not_equal(second.wait.generation, first.wait.generation);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, first, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, second, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_OK);
        check_equal(context->submitted, 2u); check_equal(context->completed, 2u);
        check_equal(number(context, "entered"), 2u); check_equal(number(context, "finished"), 2u);
    }
    it("rejects foreign-instance tickets even when Runtime wait and pass numbers coincide") {
        turbowasm_component_shutdown_wait first, second;
        create(1u); make(&contexts[0]); make(&contexts[1]);
        start(&contexts[0]); start(&contexts[1]); first = pending(&contexts[0]); second = pending(&contexts[1]);
        check_equal(first.wait.generation, second.wait.generation);
        check_equal(first.shutdown_generation, second.shutdown_generation);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(contexts[1].impl, first, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(contexts[0].impl, second, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(contexts[0].completed, 0u); check_equal(contexts[1].completed, 0u);
    }
    it("rejects a previous pass ticket when a destructor creates a new cleanup obligation") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait first, second;
        make(context); context->spawn_once = true; start(context); first = pending(context);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, first, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        check_null(context->impl->shutdown); check_equal(context->impl->exec.resource_table.live_count, 1u);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        second = pending(context);
        check_equal(first.wait.generation, second.wait.generation);
        check_equal(first.wait.operation_token, second.wait.operation_token);
        check_not_equal(first.shutdown_generation, second.shutdown_generation);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, first, WAIT_RESULT), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, second, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_OK);
        check_equal(number(context, "finished"), 2u);
    }
    it("uses fresh fuel and cooperative interruption after host completion without replaying the destructor") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait wait;
        turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 1u};
        bool stopped = true; unsigned polls = 0u; turbowasm_status status;
        make(context); start(context); wait = pending(context);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, WAIT_RESULT), TURBOWASM_OK);
        options.should_interrupt = interrupt; options.interrupt_context = &stopped;
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_instance_shutdown_yield_reason_private(context->impl), TURBOWASM_YIELD_INTERRUPTION);
        stopped = false;
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_instance_shutdown_yield_reason_private(context->impl), TURBOWASM_YIELD_FUEL);
        do { status = turbowasm_component_instance_poll_shutdown_private(context->impl, &options); check_true(++polls < MAX_POLLS); }
        while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK); check_equal(context->submitted, 1u); check_equal(context->completed, 1u);
        check_equal(number(context, "entered"), 1u); check_equal(number(context, "finished"), 1u);
    }
    it("records a post-wait trap once and completes sibling destructors before reporting it") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait wait;
        make(context); make(context); start(context); wait = pending(context);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, -1), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        check_equal(context->impl->shutdown_status, TURBOWASM_TRAPPED); wait = pending(context);
        check_equal(turbowasm_component_instance_shutdown_complete_host_wait_private(context->impl, wait, WAIT_RESULT), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_TRAPPED);
        check_true(context->impl->shutdown_complete); check_equal(number(context, "entered"), 2u); check_equal(number(context, "finished"), 1u);
        check_equal(context->submitted, 2u); check_equal(context->completed, 2u);
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_TRAPPED);
    }
    it("preserves query outputs and rejects allocator and callback reentry") {
        wait_context *context = &contexts[0];
        turbowasm_component_shutdown_wait wait = {.instance = context->impl, .shutdown_generation = 99u,
            .wait = {.generation = 77u, .operation_token = 66u}};
        check_false(turbowasm_component_instance_shutdown_pending_host_wait_private(NULL, &wait));
        check_false(turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, &wait));
        check_equal(wait.shutdown_generation, (uint64_t)99); check_equal(wait.wait.generation, (uint64_t)77);
        check_equal(wait.wait.operation_token, (uintptr_t)66);
        make(context); check_equal(turbowasm_component_instance_request_shutdown_private(context->impl), TURBOWASM_OK);
        allocator_reentry = context;
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_YIELDED);
        check_null(allocator_reentry); check_equal(context->reentries, 2u);
        check_false(turbowasm_component_instance_shutdown_pending_host_wait_private(context->impl, NULL));
    }
    it("traps pass-counter exhaustion before consuming a resource") {
        wait_context *context = &contexts[0];
        make(context); check_equal(turbowasm_component_instance_request_shutdown_private(context->impl), TURBOWASM_OK);
        context->impl->shutdown_generation = UINT64_MAX;
        check_equal(turbowasm_component_instance_poll_shutdown_private(context->impl, NULL), TURBOWASM_TRAPPED);
        check_null(context->impl->shutdown); check_equal(context->impl->exec.resource_table.live_count, 1u);
        check_equal(context->submitted, 0u); check_true(context->impl->admission_closed);
        context->impl->shutdown_generation = 0u;
    }
    it("rejects malformed imports or missing capability bindings without publishing an instance") {
        turbowasm_component_instance instance = {0};
        turbowasm_component_exec_async_limits limits = {1u, 8u}; size_t live = live_allocations;
        check_equal(turbowasm_component_instance_create_async_with_import_sets_private(&instance, &component,
            &limits, NULL, 1u), TURBOWASM_INVALID_ARGUMENT);
        check_null(instance.impl); check_equal(live_allocations, live);
        check_equal(turbowasm_component_instance_create_async_private(&instance, &component, &limits), TURBOWASM_UNSUPPORTED);
        check_null(instance.impl); check_equal(live_allocations, live);
    }
    it("rolls back every allocation failure in retained construction with copied imports") {
        turbowasm_component_instance instance = {0};
        turbowasm_component_exec_async_limits limits = {1u, 8u};
        turbowasm_component_exec_imports imports = {0};
        size_t before = allocation_calls, live = live_allocations, count, failure;
        imports.context = &contexts[1]; imports.can_bind = can_bind; imports.invoke = wait_host;
        check_equal(turbowasm_component_instance_create_async_with_import_sets_private(&instance,
            &component, &limits, &imports, 1u), TURBOWASM_OK);
        count = allocation_calls - before; check_greater(count, (size_t)0);
        turbowasm_component_instance_destroy(&instance); check_equal(live_allocations, live);
        for (failure = 1u; failure <= count; ++failure) {
            fail_at = allocation_calls + failure;
            check_equal(turbowasm_component_instance_create_async_with_import_sets_private(&instance,
                &component, &limits, &imports, 1u), TURBOWASM_OUT_OF_MEMORY);
            fail_at = 0u;
            check_null(instance.impl); check_equal(live_allocations, live);
        }
    }
}
