#include "component_api_internal.h"
#include "instance_internal.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"
#include "fixtures/component_host_tasks.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { OWNER_COUNT = 3, BYTE_LIMIT = 65536, FAILURE_LIMIT = 64 };
static turbowasm_component component;
static turbowasm_component_instance instance;
static turbowasm_component_instance_public_impl *impl;
static turbowasm_component_host_task owners[OWNER_COUNT];
static turbowasm_component_host_budget budget;
static turbowasm_component_host_value resource, output;
static struct { size_t live, attempts, fail_at; } allocations;
static turbowasm_component_instance destination;
static turbowasm_component_endpoint *pair_ends[2][2];
static turbowasm_component_value endpoint_value, payload;
static turbowasm_component_endpoint_codec endpoint_codec;
static turbowasm_component_buffer write_buffer;
static bool close_on_allocate;

static uint32_t endpoint_type(turbowasm_component_instance_public_impl *owner, bool future) {
    const turbowasm_component_type_graph *graph = &owner->exec.binary->type_graph;
    uint32_t i;
    for (i = 0u; i < graph->count; ++i)
        if (graph->types[i].kind == (future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM)) return i;
    return UINT32_MAX;
}
static turbowasm_status open_pair(unsigned index, bool future, bool guest) {
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
    turbowasm_status status = turbowasm_component_endpoint_domain_pair_open(&impl->exec.task_domain,
        &impl->exec.binary->type_graph, endpoint_type(impl, future), guest, &pair_ends[index][0], &pair_ends[index][1]);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
static void close_pair(unsigned index) {
    turbowasm_component_endpoint *ends[2] = {pair_ends[index][0], pair_ends[index][1]};
    bool close[2];
    unsigned i;
    for (i = 0u; i < 2u; ++i) {
        close[i] = ends[i] != NULL && !ends[i]->closed;
        if (close[i] && ends[i]->operation != NULL) {
            turbowasm_component_event event;
            if (ends[i]->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                check_equal(turbowasm_component_endpoint_cancel(ends[i]), TURBOWASM_OK);
            (void)turbowasm_component_endpoint_take(ends[i], &event);
        }
    }
    pair_ends[index][0] = pair_ends[index][1] = NULL;
    for (i = 0u; i < 2u; ++i) if (close[i]) check_equal(turbowasm_component_endpoint_close(ends[i]), TURBOWASM_OK);
}

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (close_on_allocate) {
        close_on_allocate = false;
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
    }
    if (++allocations.attempts == allocations.fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++allocations.live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context; if (p != NULL) { check_true(allocations.live != 0u); --allocations.live; } free(p);
}
static turbowasm_name name(const char *text) {
    return (turbowasm_name){(const uint8_t *)text, (uint32_t)strlen(text)};
}
static turbowasm_status create(unsigned owner, const char *export_name,
    const turbowasm_component_host_value *values, size_t count, bool move) {
    return turbowasm_component_host_task_create(&owners[owner], impl, name(export_name), values, count, move, &budget);
}
static void attach(void) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < impl->exec.core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0};
        if (impl->exec.core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(impl->exec.core_instances[i].impl, &backend, 1u), TURBOWASM_OK);
    }
#endif
}
static void compiled(unsigned owner) {
#ifdef TURBOWASM_TEST_MIR
    const turbowasm_component_task *task = turbowasm_component_host_task_view(&owners[owner]);
    check_equal(((turbowasm_instance_impl *)task->binding.instance->impl)->jit_functions[
        task->binding.function_index].state, TURBOWASM_JIT_COMPILED);
    if (task->binding.callback_instance != NULL && task->phase != TURBOWASM_COMPONENT_TASK_INITIAL)
        check_equal(((turbowasm_instance_impl *)task->binding.callback_instance->impl)->jit_functions[
            task->binding.callback_index].state, TURBOWASM_JIT_COMPILED);
#else
    (void)owner;
#endif
}
static uint32_t drops(void) {
    turbowasm_component_host_value value = {0};
    turbowasm_trap trap;
    size_t count;
    check_equal(turbowasm_component_instance_invoke(&instance, name("drops"), NULL, 0u, &value, 1u, &count, &trap), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    if (value.as.u32 != 0u) {
        const turbowasm_component_task_binding *binding;
        const turbowasm_component_type *function, *handle, *definition;
        const turbowasm_component_exec_core_function *dtor;
        check_equal(turbowasm_component_exec_async_export(&impl->exec,
            (const uint8_t *)"consume", 7u, &binding), TURBOWASM_OK);
        function = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
        handle = turbowasm_component_type_graph_get(binding->graph, function->as.function.params[0].as.indexed);
        definition = turbowasm_component_resource_definition(binding->graph, handle->as.handle.resource_type);
        dtor = &impl->exec.core_functions[definition->as.resource.destructor_index];
        check_equal(((turbowasm_instance_impl *)impl->exec.core_instances[dtor->instance_index].impl)->jit_functions[
            dtor->function_index].state, TURBOWASM_JIT_COMPILED);
    }
#endif
    return value.as.u32;
}
static void make(int32_t rep) {
    turbowasm_component_host_value value = {.kind = TURBOWASM_COMPONENT_HOST_S32, .as.s32 = rep};
    turbowasm_trap trap;
    size_t count;
    check_equal(turbowasm_component_instance_invoke(&instance, name("make"), &value, 1u, &resource, 1u, &count, &trap), TURBOWASM_OK);
}
static bool interrupt(void *context) { (void)context; return true; }
static bool reenter(void *context) {
    size_t *calls = context, count = 99u, used = budget.used;
    void *owner = owners[0].impl;
    const turbowasm_component_task *view = turbowasm_component_host_task_view(&owners[0]);
    turbowasm_component_host_value value = {0};
    bool cancellation = view->cancellation_requested;
    ++*calls;
    check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_component_host_task_request_cancel(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_component_host_task_take_result(&owners[0], &value, &count), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
    check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_INVALID_ARGUMENT);
    check_true(owners[0].impl == owner); check_equal(budget.used, used);
    check_equal(view->cancellation_requested, cancellation);
    check_equal((int)value.kind, 0); check_equal(count, (size_t)99);
    return false;
}
static void finish(unsigned owner) {
    turbowasm_status status = TURBOWASM_YIELDED;
    turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 8u};
    unsigned turns;
    for (turns = 0u; turns < 512u && status == TURBOWASM_YIELDED; ++turns)
        status = turbowasm_component_host_task_resume(&owners[owner], &options);
    check_equal(status, TURBOWASM_OK); compiled(owner);
}
static void take(unsigned owner, uint32_t expected) {
    size_t count = 99u;
    check_equal(turbowasm_component_host_task_take_result(&owners[owner], &output, &count), TURBOWASM_OK);
    check_equal(count, (size_t)1); check_equal(output.kind, TURBOWASM_COMPONENT_HOST_U32);
    check_equal(output.as.u32, expected);
}

spec("Retained Component host task owners") {
    before_each() {
        turbowasm_runtime_config config;
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        memset(&allocations, 0, sizeof(allocations)); budget = (turbowasm_component_host_budget){BYTE_LIMIT, 0u};
        close_on_allocate = false;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_async_private(&component, component_host_tasks_bytes,
            sizeof(component_host_tasks_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create_async_private(&instance, &component, &limits), TURBOWASM_OK);
        impl = turbowasm_component_instance_public_impl_get(&instance); attach();
    }
    after_each() {
        unsigned i;
        allocations.fail_at = 0u;
        for (i = 0u; i < OWNER_COUNT; ++i) {
            const turbowasm_component_task *view = turbowasm_component_host_task_view(&owners[i]);
            if (view != NULL && view->state < TURBOWASM_EXECUTION_COMPLETED) {
                turbowasm_execution_options options = {.should_interrupt = interrupt};
                while (view->domain->backpressure != 0u)
                    (void)turbowasm_component_task_backpressure(view->domain, false);
                (void)turbowasm_component_host_task_request_cancel(&owners[i]);
                (void)turbowasm_component_host_task_resume(&owners[i], &options);
            }
            (void)turbowasm_component_host_task_destroy(&owners[i]); check_null(owners[i].impl);
        }
        if (endpoint_codec.table != NULL)
            check_equal(turbowasm_component_endpoint_codec_rollback(&endpoint_codec), TURBOWASM_OK);
        if ((int)endpoint_value.kind != 0) {
            turbowasm_component_endpoint *taken = NULL;
            (void)turbowasm_component_endpoint_take_value(&endpoint_value, &taken);
            (void)turbowasm_component_value_destroy(&endpoint_value);
        }
        for (i = 0u; i < 2u; ++i) close_pair(i);
        memset(&endpoint_codec, 0, sizeof(endpoint_codec)); memset(&write_buffer, 0, sizeof(write_buffer));
        (void)turbowasm_component_value_destroy(&payload);
        turbowasm_component_instance_destroy(&destination);
        (void)turbowasm_component_host_value_destroy(&resource);
        (void)turbowasm_component_host_value_destroy(&output);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(budget.used, (size_t)0); check_equal(allocations.live, (size_t)0); impl = NULL;
    }
    it("keeps public binary and synchronous instance gates closed") {
        turbowasm_component ordinary = {0}; turbowasm_component_instance sync = {0};
        check_equal(turbowasm_component_load_borrowed(&ordinary, component_host_tasks_bytes,
            sizeof(component_host_tasks_bytes)), TURBOWASM_UNSUPPORTED);
        check_null(ordinary.impl);
        check_equal(turbowasm_component_instance_create(&sync, &component), TURBOWASM_UNSUPPORTED);
        check_null(sync.impl);
    }
    it("retains each pair's creation instance until both host or guest ends close") {
        unsigned future, guest;
        uint32_t refs = impl->ref_count;
        for (future = 0u; future < 2u; ++future) for (guest = 0u; guest < 2u; ++guest) {
            check_equal(open_pair(0u, future != 0u, guest != 0u), TURBOWASM_OK);
            check_equal(impl->ref_count, refs + 1u);
            check_equal(turbowasm_component_endpoint_close(pair_ends[0][0]), TURBOWASM_OK);
            check_equal(impl->ref_count, refs + 1u);
            close_pair(0u); check_equal(impl->ref_count, refs);
            turbowasm_component_endpoint_domain_collect(&impl->exec.task_domain);
            check_equal(impl->exec.task_domain.pair_count, 0u);
        }
    }
    it("does not leak an instance reference after pair allocation or handle registration failure") {
        size_t failure, live = allocations.live;
        uint32_t refs = impl->ref_count;
        bool complete = false;
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = open_pair(0u, false, true); allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) { complete = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(pair_ends[0][0]); check_null(pair_ends[0][1]);
            check_equal(impl->ref_count, refs); check_equal(allocations.live, live);
            check_equal(impl->exec.task_domain.pair_count, 0u); check_equal(impl->exec.resource_table.live_count, 0u);
        }
        check_true(complete); close_pair(0u);
    }
    it("rejects creation-instance reference overflow before allocating or publishing a pair") {
        uint32_t refs = impl->ref_count;
        size_t attempts = allocations.attempts;
        turbowasm_status status;
        impl->ref_count = UINT32_MAX;
        status = open_pair(0u, false, false);
        impl->ref_count = refs;
        check_equal(status, TURBOWASM_INVALID_ARGUMENT); check_equal(allocations.attempts, attempts);
        check_null(pair_ends[0][0]); check_null(pair_ends[0][1]);
        check_equal(impl->exec.task_domain.pair_count, 0u);
    }
    it("keeps host pair storage and the instance alive after all public handles close") {
        check_equal(open_pair(0u, false, false), TURBOWASM_OK);
        check_equal(open_pair(1u, true, false), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(impl->ref_count, 2u); close_pair(0u); check_equal(impl->ref_count, 1u);
        check_greater(allocations.live, (size_t)0);
        close_pair(1u); impl = NULL; check_equal(allocations.live, (size_t)0);
    }
    it("releases the final instance after allocation failure even when the allocator closes public handles") {
        allocations.attempts = 0u; allocations.fail_at = 1u; close_on_allocate = true;
        check_equal(open_pair(0u, false, false), TURBOWASM_OUT_OF_MEMORY);
        allocations.fail_at = 0u; impl = NULL;
        check_null(instance.impl); check_null(component.impl);
        check_null(pair_ends[0][0]); check_null(pair_ends[0][1]); check_equal(allocations.live, (size_t)0);
    }
    it("returns pair keepalives after forwarding consumes intermediate ends") {
        unsigned future;
        uint32_t refs = impl->ref_count;
        for (future = 0u; future < 2u; ++future) {
            check_equal(open_pair(0u, future != 0u, false), TURBOWASM_OK);
            check_equal(open_pair(1u, future != 0u, false), TURBOWASM_OK);
            check_equal(impl->ref_count, refs + 2u);
            check_equal(turbowasm_component_endpoint_forward(pair_ends[0][0], pair_ends[1][1]), TURBOWASM_OK);
            check_true(pair_ends[0][0]->closed); check_true(pair_ends[1][1]->closed);
            check_equal(impl->ref_count, refs + 2u);
            close_pair(0u); check_equal(impl->ref_count, refs + 1u);
            close_pair(1u); check_equal(impl->ref_count, refs);
            turbowasm_component_endpoint_domain_collect(&impl->exec.task_domain);
            check_equal(impl->exec.task_domain.pair_count, 0u);
        }
    }
    it("closes a forwarded self pair after its public instance and component handles disappear") {
        turbowasm_component_endpoint *reader, *writer;
        check_equal(open_pair(0u, false, false), TURBOWASM_OK);
        reader = pair_ends[0][0]; writer = pair_ends[0][1];
        pair_ends[0][0] = pair_ends[0][1] = NULL;
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(turbowasm_component_endpoint_forward(reader, writer), TURBOWASM_OK);
        impl = NULL; check_equal(allocations.live, (size_t)0);
    }
    it("retains a guest-created pair after the creating task and public instance handles close") {
        size_t count;
        uint64_t packed;
        check_equal(create(0u, "pair-stream", NULL, 0u, false), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_OK);
        check_equal(output.kind, TURBOWASM_COMPONENT_HOST_U64); packed = output.as.u64;
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        pair_ends[0][0] = turbowasm_component_endpoint_get(&impl->exec.resource_table, (uint32_t)packed,
            TURBOWASM_COMPONENT_HANDLE_STREAM_READ);
        pair_ends[0][1] = turbowasm_component_endpoint_get(&impl->exec.resource_table, (uint32_t)(packed >> 32),
            TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE);
        check_not_null(pair_ends[0][0]); check_not_null(pair_ends[0][1]); check_equal(impl->ref_count, 2u);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(impl->ref_count, 1u); close_pair(0u); impl = NULL;
        check_equal(allocations.live, (size_t)0);
    }
    it("keeps the creation type graph alive through foreign canonical publication and compiled guest reads") {
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        turbowasm_component_instance_public_impl *original = impl, *target;
        turbowasm_component_host_value handle = {.kind = TURBOWASM_COMPONENT_HOST_U32};
        turbowasm_component_event event;
        uint32_t guest_handle = 0u;
        size_t count;
        check_equal(turbowasm_component_instance_create_async_private(&destination, &component, &limits), TURBOWASM_OK);
        target = turbowasm_component_instance_public_impl_get(&destination);
        impl = target; attach(); impl = original;
        check_equal(open_pair(0u, false, false), TURBOWASM_OK);
        payload.kind = TURBOWASM_COMPONENT_TYPE_U32; payload.as.u32 = 42u;
        write_buffer.values = &payload; write_buffer.length = 1u;
        check_equal(turbowasm_component_endpoint_submit(pair_ends[0][1], &write_buffer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_into_value(pair_ends[0][0], &endpoint_value), TURBOWASM_OK);
        endpoint_codec.table = &target->exec.resource_table;
        check_equal(turbowasm_component_endpoint_codec_lower(&endpoint_codec, &target->exec.binary->type_graph,
            turbowasm_component_type_ref_indexed(endpoint_type(target, false)), &endpoint_value, &guest_handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_commit(&endpoint_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&endpoint_value), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(original->ref_count, 1u);
        impl = target; handle.as.u32 = guest_handle;
        check_equal(create(0u, "read-stream", &handle, 1u, false), TURBOWASM_OK); finish(0u); take(0u, 16u);
        {
            const turbowasm_component_task *view = turbowasm_component_host_task_view(&owners[0]);
            turbowasm_component_value received = {0};
            check_equal(turbowasm_component_canonical_lift_value(view->binding.graph,
                turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32), &view->binding.memory,
                128u, &received), TURBOWASM_OK);
            check_equal(received.as.u32, 42u); check_equal(turbowasm_component_value_destroy(&received), TURBOWASM_OK);
        }
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_take(pair_ends[0][1], &event), TURBOWASM_OK);
        check_equal(event.payload, 16u); check_false(write_buffer.leased);
        check_equal(turbowasm_component_endpoint_close(pair_ends[0][1]), TURBOWASM_OK);
        pair_ends[0][0] = pair_ends[0][1] = NULL;
        check_equal(create(0u, "drop-stream-read", &handle, 1u, false), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], NULL, &count), TURBOWASM_OK);
        check_equal(count, (size_t)0);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(target->exec.resource_table.live_count, 0u);
    }
    it("delivers one scalar result and rejects early or repeated delivery without changing outputs") {
        size_t count = 99u;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_YIELDED);
        check_equal(count, (size_t)99); check_equal((int)output.kind, 0);
        finish(0u); take(0u, 42u); count = 99u;
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_INVALID_ARGUMENT);
        check_equal(count, (size_t)99); check_equal(output.as.u32, 42u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(impl->exec.task_domain.count, 0u); check_equal(budget.used, (size_t)0);
    }
    it("delivers a unit result once without requiring a host value") {
        size_t count = 99u;
        check_equal(create(0u, "unit", NULL, 0u, false), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], NULL, &count), TURBOWASM_OK);
        check_equal(count, (size_t)0);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], NULL, &count), TURBOWASM_INVALID_ARGUMENT);
    }
    it("keeps live destruction unchanged while backpressure delays parameter preparation") {
        size_t charge;
        check_equal(turbowasm_component_task_backpressure(&impl->exec.task_domain, true), TURBOWASM_OK);
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK); charge = budget.used;
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(budget.used, charge); check_equal(impl->exec.task_domain.count, 1u);
        check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_INITIAL);
        check_equal(turbowasm_component_task_backpressure(&impl->exec.task_domain, false), TURBOWASM_OK);
        finish(0u); take(0u, 42u);
    }
    it("rejects reentrant host owner operations while the Runtime polls interruption") {
        size_t calls = 0u;
        turbowasm_execution_options options = {.should_interrupt = reenter, .interrupt_context = &calls};
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], &options), TURBOWASM_OK);
        check_greater(calls, (size_t)0); compiled(0u); take(0u, 42u);
    }
    it("retries terminal delivery after another task releases the shared byte quota") {
        size_t charge, used, count = 99u;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK); charge = budget.used;
        finish(0u); check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        budget.limit = charge - 1u;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OUT_OF_MEMORY);
        check_null(owners[0].impl); check_equal(budget.used, (size_t)0);
        budget.limit = 2u * charge;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        check_equal(create(1u, "answer", NULL, 0u, false), TURBOWASM_OK);
        finish(0u); used = budget.used; check_equal(used, budget.limit);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_OUT_OF_MEMORY);
        check_equal(count, (size_t)99); check_equal((int)output.kind, 0); check_equal(budget.used, used);
        finish(1u); check_equal(turbowasm_component_host_task_destroy(&owners[1]), TURBOWASM_OK);
        take(0u, 42u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(budget.used, (size_t)0);
    }
    it("copies memory32 and memory64 strings before deferred entry and resumes guest realloc with fuel") {
        unsigned wide;
        for (wide = 0u; wide < 2u; ++wide) {
            uint8_t bytes[] = {'h', 0xc3, 0xa9, 0, 'x'};
            turbowasm_component_host_value text = {.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {bytes, sizeof(bytes)}};
            size_t count;
            check_equal(create(0u, wide ? "echo64" : "echo32", &text, 1u, false), TURBOWASM_OK);
            memset(bytes, 0xff, sizeof(bytes)); memset(&text, 0, sizeof(text)); finish(0u);
            check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_OK);
            check_equal(count, (size_t)1); check_equal(output.as.string.size, (size_t)5);
            check_equal(memcmp(output.as.string.data, "h\xc3\xa9\0x", 5u), 0);
            check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            check_null(impl->exec.task_domain.auxiliary); check_true(impl->exec.may_leave);
        }
    }
    it("lowers indirect tuple parameters on the retained guest realloc stack") {
        turbowasm_component_host_value fields[17] = {0};
        turbowasm_component_host_value tuple = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE, .as.tuple = {fields, 17u}};
        unsigned i;
        for (i = 0u; i < 17u; ++i) { fields[i].kind = TURBOWASM_COMPONENT_HOST_S32; fields[i].as.s32 = (int32_t)i; }
        fields[0].as.s32 = 26;
        check_equal(create(0u, "wide", &tuple, 1u, false), TURBOWASM_OK);
        memset(fields, 0, sizeof(fields)); finish(0u); take(0u, 42u);
    }
    it("does not turn a cancellation request into acknowledgement at a noncancellable yield") {
        check_equal(create(0u, "yield", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_host_task_request_cancel(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_RETURNED);
        take(0u, 43u); compiled(0u);
    }
    it("cancels a moved own task before entry and releases its destructor once") {
        size_t charge;
        make(42); check_equal(turbowasm_component_task_backpressure(&impl->exec.task_domain, true), TURBOWASM_OK);
        check_equal(create(0u, "consume", &resource, 1u, true), TURBOWASM_OK);
        check_equal((int)resource.kind, 0); charge = budget.used; check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_task_request_cancel(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_OK);
        check_equal(budget.used, charge); check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u); check_equal(impl->exec.async_resource_owners, 0u);
    }
    it("frees a cancelled owner even when its unentered own parameter destructor traps") {
        make(-1);
        check_equal(turbowasm_component_task_backpressure(&impl->exec.task_domain, true), TURBOWASM_OK);
        check_equal(create(0u, "consume", &resource, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_request_cancel(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_null(owners[0].impl); check_equal(budget.used, (size_t)0);
        check_equal(drops(), 1u); check_equal(impl->resource_count, 0u);
        check_equal(impl->exec.async_resource_owners, 0u); check_equal(impl->exec.task_domain.count, 0u);
    }
    it("delivers a moved own result that survives task destruction and can move into another task") {
        size_t count;
        make(42); check_equal(create(0u, "echo-own", &resource, 1u, true), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_OK);
        check_equal(count, (size_t)1); check_equal(output.kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 0u); check_equal(impl->exec.async_resource_owners, 1u);
        check_equal(create(1u, "consume", &output, 1u, true), TURBOWASM_OK); finish(1u); take(1u, 42u);
        check_equal(turbowasm_component_host_task_destroy(&owners[1]), TURBOWASM_OK);
        check_equal(drops(), 1u); check_equal(impl->exec.async_resource_owners, 0u);
    }
    it("keeps host borrow loans until terminal result delivery") {
        turbowasm_component_host_value borrowed = {0};
        make(42); check_equal(turbowasm_component_host_value_borrow(&resource, &borrowed), TURBOWASM_OK);
        check_equal(create(0u, "borrow", &borrowed, 1u, false), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_value_destroy(&resource), TURBOWASM_INVALID_ARGUMENT);
        take(0u, 42u);
        check_equal(turbowasm_component_host_value_destroy(&resource), TURBOWASM_OK); check_equal(drops(), 1u);
    }
    it("resumes callbacks and acknowledges callback cancellation") {
        check_equal(create(0u, "callback", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_host_task_request_cancel(&owners[0]), TURBOWASM_OK);
        finish(0u); check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
    }
    it("does not publish a canonical return until Core exits successfully") {
        size_t count = 99u;
        check_equal(create(0u, "trap-after-return", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_TRAPPED); compiled(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_TRAPPED);
        check_equal(count, (size_t)99); check_equal((int)output.kind, 0);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED); check_null(owners[0].impl);
    }
    it("releases an undelivered own result after a post-return guest trap") {
        make(42); check_equal(create(0u, "own-trap-after-return", &resource, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_TRAPPED); compiled(0u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_equal(drops(), 1u); check_equal(impl->exec.async_resource_owners, 0u);
    }
    it("preserves own inputs on task quota and invalid signature rejection") {
        size_t used;
        make(42); check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        check_equal(create(1u, "answer", NULL, 0u, false), TURBOWASM_OK); used = budget.used;
        check_equal(create(2u, "consume", &resource, 1u, true), TURBOWASM_OUT_OF_MEMORY);
        check_equal(resource.kind, TURBOWASM_COMPONENT_HOST_OWN); check_equal(budget.used, used);
        finish(0u); check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(create(2u, "consume", &resource, 0u, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(resource.kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(create(2u, "consume", &resource, 1u, true), TURBOWASM_OK); finish(2u);
    }
    it("rolls back every host task allocation before consuming own inputs") {
        size_t failure, live, used;
        uint32_t refs;
        bool complete = false;
        make(42); live = allocations.live; refs = impl->ref_count; used = budget.used;
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure; status = create(0u, "consume", &resource, 1u, true);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) { complete = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(owners[0].impl);
            check_equal(resource.kind, TURBOWASM_COMPONENT_HOST_OWN); check_equal(impl->ref_count, refs);
            check_equal(allocations.live, live); check_equal(budget.used, used);
            check_equal(impl->exec.task_domain.count, 0u); check_equal(impl->exec.async_resource_owners, 0u);
        }
        check_true(complete); check_greater(failure, (size_t)3); finish(0u); take(0u, 42u);
    }
    it("retains a completed canonical result when public result promotion allocation fails") {
        size_t failure, count = 99u, live, used;
        bool complete = false;
        make(42); check_equal(create(0u, "echo-own", &resource, 1u, true), TURBOWASM_OK); finish(0u);
        live = allocations.live; used = budget.used;
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = turbowasm_component_host_task_take_result(&owners[0], &output, &count); allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) { complete = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(count, (size_t)99); check_equal((int)output.kind, 0);
            check_equal(allocations.live, live); check_equal(budget.used, used); check_equal(drops(), 0u);
        }
        check_true(complete); check_greater(failure, (size_t)1); check_equal(count, (size_t)1);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK); check_equal(drops(), 1u);
    }
    it("retains the instance after both public handles close") {
        size_t count;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &count), TURBOWASM_OK);
        check_equal(output.as.u32, 42u); check_equal(count, (size_t)1);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK); impl = NULL;
    }
    it("propagates guest realloc traps without entering the export or leaking byte reservations") {
        uint8_t bytes[13] = {0};
        turbowasm_component_host_value text = {.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {bytes, sizeof(bytes)}};
        check_equal(create(0u, "echo32", &text, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_equal(budget.used, (size_t)0); check_true(impl->exec.may_leave); check_null(impl->exec.task_domain.auxiliary);
    }
    it("rolls back an earlier own reservation when a later parameter realloc traps") {
        uint8_t bytes[13] = {0};
        turbowasm_component_host_value values[2] = {0};
        make(42); values[0] = resource; memset(&resource, 0, sizeof(resource));
        values[1].kind = TURBOWASM_COMPONENT_HOST_STRING;
        values[1].as.string.data = bytes; values[1].as.string.size = sizeof(bytes);
        check_equal(create(0u, "consume-text", values, 2u, true), TURBOWASM_OK);
        check_equal((int)values[0].kind, 0); check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_TRAPPED);
        check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_null(owners[0].impl); check_equal(drops(), 1u);
        check_equal(impl->exec.async_resource_owners, 0u); check_equal(impl->resource_count, 0u);
        check_equal(budget.used, (size_t)0); check_true(impl->exec.may_leave);
        check_null(impl->exec.task_domain.auxiliary);
    }
}
