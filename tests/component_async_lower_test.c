#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_lower.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_task task;
static turbowasm_component_value value;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context; if (p != NULL) --live; free(p);
}
static void compiled(turbowasm_instance *instance, uint32_t index) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)instance->impl)->jit_functions[index].state, TURBOWASM_JIT_COMPILED);
#else
    (void)instance; (void)index;
#endif
}
static void create(const char *name) {
    const turbowasm_component_task_binding *binding = NULL;
    check_equal(turbowasm_component_exec_async_export(&exec, (const uint8_t *)name, (uint32_t)strlen(name), &binding), TURBOWASM_OK);
    check_equal(turbowasm_component_task_create(&task, &exec.task_domain, binding), TURBOWASM_OK);
}
static uint32_t poll(const turbowasm_execution_options *options) {
    uint32_t pending = UINT32_MAX;
    turbowasm_status status = turbowasm_component_exec_async_poll(&exec, 3, options, &pending);
    check_equal(status, pending ? TURBOWASM_YIELDED : TURBOWASM_OK);
    check_equal(pending, exec.async_call_count); return pending;
}
static void result(uint32_t expected) {
    check_equal(task.state, TURBOWASM_EXECUTION_COMPLETED);
    compiled(task.binding.instance, task.binding.function_index);
    check_equal(turbowasm_component_task_take_result(&task, &value), TURBOWASM_OK);
    check_equal(value.as.u32, expected);
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
}
static void cleanup_calls(void) {
    uint32_t i;
    check_equal(turbowasm_component_task_destroy(&task), TURBOWASM_OK);
    check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_OK);
    /* Failure unwind can leave guest-owned empty sets whose drop instruction was
     * never reached. Release them after their subtasks and waiters are gone. */
    for (i = 0; i < exec.resource_table.capacity; ++i) {
        uint32_t handle; turbowasm_component_handle_kind kind; void *object;
        if (turbowasm_component_handle_at(&exec.resource_table, i, &handle, &kind, &object) &&
            kind == TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET)
            check_equal(turbowasm_component_task_set_drop(&exec.task_domain, handle), TURBOWASM_OK);
    }
}

spec("instantiated async canonical lower") {
    before_each() {
        turbowasm_component_exec_async_limits limits = {5, 16};
        live = 0; allowance = SIZE_MAX;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binary, component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        { uint32_t i;
          for (i = 0; i < exec.core_instance_count; ++i) {
              turbowasm_jit_backend backend = {0};
              if (exec.core_instances[i].impl == NULL) continue;
              check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
              check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl, &backend, 1), TURBOWASM_OK);
          } }
#endif
    }
    after_each() {
        allowance = SIZE_MAX;
        cleanup_calls(); check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binary); turbowasm_runtime_scope_leave(scope);
        check_equal(live, 0u);
    }
    it("links a local lower and reclaims eager children while their parent continues") {
        create("eager"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(41);
        check_equal(exec.async_call_count, 0u); check_equal(exec.task_domain.count, 1u);
        compiled(exec.async_functions[0].instance, exec.async_functions[0].function_index);
        check_equal(turbowasm_component_task_destroy(&task), TURBOWASM_OK);
        create("batch"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(2);
        check_equal(exec.async_call_count, 0u); check_equal(exec.task_domain.count, 1u);
    }
    it("keeps callbacks and canonical result memory until terminal event delivery and drop") {
        create("deferred"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_YIELDED);
        check_equal(exec.async_call_count, 1u); check_equal(exec.task_domain.count, 2u);
        check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_equal(poll(NULL), 1u);
        check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(44);
        check_equal(poll(NULL), 0u); check_equal(exec.resource_table.live_count, 0u);
        compiled(exec.async_functions[1].callback_instance, exec.async_functions[1].callback_index);
    }
    it("retains the callee after eager task.return until its final callback exits") {
        create("early"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(43);
        check_equal(exec.async_call_count, 1u); check_equal(exec.resource_table.live_count, 0u);
        check_equal(turbowasm_component_task_destroy(&task), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_equal(poll(NULL), 0u); check_equal(exec.task_domain.count, 0u);
    }
    it("allows a polled callback to create and retire nested calls without invalidating the queue") {
        uint32_t adapter; const turbowasm_component_task_binding *child;
        create("nested"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_YIELDED);
        check_equal(exec.async_call_count, 1u); check_equal(poll(NULL), 1u);
        check_equal(exec.task_domain.count, 2u);
        check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(42);
        adapter = exec.canon_lower_contexts[binary.canon_lower_count - 1u].local_adapter_index;
        child = &exec.async_functions[adapter]; compiled(child->callback_instance, child->callback_index);
        check_equal(poll(NULL), 0u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("rejects call-table exhaustion and aborts the retained child after its caller unwinds") {
        turbowasm_component_resource_table_destroy(&exec.resource_table);
        check_true(turbowasm_component_resource_table_init(&exec.resource_table, 1));
        create("deferred");
        check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(exec.async_call_count, 1u); check_equal(exec.resource_table.live_count, 1u);
        cleanup_calls(); check_equal(exec.async_call_count, 0u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("lowers memory32 strings into memory64 and returns them through the caller realloc") {
        create("text"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(5);
        check_equal(exec.async_call_count, 0u); check_true(exec.may_leave);
        compiled(exec.async_functions[3].instance, exec.async_functions[3].function_index);
    }
    it("admits memory-free unit lowerings") {
        create("unit"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(48);
        check_equal(exec.async_call_count, 0u);
    }
    it("polls retained conversion quanta without repeating guest entry or losing auxiliary ownership") {
        turbowasm_execution_options options = {0}; turbowasm_status status; unsigned turns;
        options.has_fuel_limit = true; options.fuel = 8; create("text");
        status = turbowasm_component_task_resume(&task, &options);
        for (turns = 0; turns < 1000 && status == TURBOWASM_YIELDED; ++turns) {
            (void)poll(&options);
            status = turbowasm_component_task_resume(&task, &options);
        }
        check_less(turns, 1000u); check_greater(turns, 2u); check_equal(status, TURBOWASM_OK);
        result(5); check_equal(poll(NULL), 0u); check_true(exec.may_leave); check_null(exec.task_domain.auxiliary);
    }
    it("drives guest subtask cancellation through the retained parent wait") {
        create("cancel"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_TRAPPED);
        check_equal(poll(NULL), 1u);
        check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(4);
        check_equal(poll(NULL), 0u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("unwinds unpublished child traps and out-of-bounds results without retaining frames") {
        const char *names[] = {"trap", "oob"}; unsigned i;
        for (i = 0; i < 2; ++i) {
            create(names[i]); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_TRAPPED);
            check_equal(poll(NULL), 0u); check_equal(exec.task_domain.count, 1u);
            check_equal(turbowasm_component_task_destroy(&task), TURBOWASM_OK);
        }
    }
    it("preserves a published child failure until the waiting caller observes it") {
        uint32_t pending = 0;
        create("late-trap"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_exec_async_poll(&exec, 1, NULL, &pending), TURBOWASM_TRAPPED);
        check_equal(pending, 1u); check_equal(exec.resource_table.live_count, 2u);
        check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_TRAPPED);
        cleanup_calls(); check_equal(exec.async_call_count, 0u); check_equal(exec.resource_table.live_count, 0u);
    }
    it("aborts retained realloc only after the exported caller releases its execution") {
        turbowasm_execution_options options = {0}; unsigned turns = 0;
        options.has_fuel_limit = true; options.fuel = 8; create("text");
        do {
            check_equal(turbowasm_component_task_resume(&task, &options), TURBOWASM_YIELDED);
            check_less(++turns, 100u);
        } while (exec.task_domain.auxiliary == NULL);
        check_false(exec.may_leave); check_equal(exec.async_call_count, 1u);
        check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&task), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_OK);
        check_equal(exec.async_call_count, 0u); check_true(exec.may_leave); check_null(exec.task_domain.auxiliary);
    }
    it("recovers allocation failures before and after deferred subtask publication") {
        size_t baseline, budget; bool succeeded = false;
        create("deferred"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_YIELDED);
        check_equal(poll(NULL), 1u); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(44);
        cleanup_calls(); turbowasm_component_resource_table_destroy(&exec.resource_table);
        check_true(turbowasm_component_resource_table_init(&exec.resource_table, 16)); baseline = live;
        for (budget = 0; budget < 150; ++budget) {
            turbowasm_status status;
            create("deferred"); allowance = budget; status = turbowasm_component_task_resume(&task, NULL);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_YIELDED) {
                check_equal(poll(NULL), 1u);
                check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(44); succeeded = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup_calls(); turbowasm_component_resource_table_destroy(&exec.resource_table);
            check_true(turbowasm_component_resource_table_init(&exec.resource_table, 16));
            check_equal(exec.async_call_count, 0u); check_equal(live, baseline); if (succeeded) break;
        }
        check_true(succeeded); check_greater(budget, 5u);
    }
    it("rolls back runtime allocation failures through the owning call registry") {
        size_t baseline, budget; bool succeeded = false;
        create("eager"); check_equal(turbowasm_component_task_resume(&task, NULL), TURBOWASM_OK); result(41);
        cleanup_calls(); baseline = live;
        for (budget = 0; budget < 100; ++budget) {
            turbowasm_status status;
            create("eager"); allowance = budget; status = turbowasm_component_task_resume(&task, NULL);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { result(41); succeeded = true; }
            else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup_calls(); check_equal(exec.async_call_count, 0u); check_equal(exec.task_domain.count, 0u);
            check_equal(live, baseline); if (succeeded) break;
        }
        check_true(succeeded); check_greater(budget, 5u);
    }
}
