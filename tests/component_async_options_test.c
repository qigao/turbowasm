#include "component_api_internal.h"
#include "component_endpoint_builtin.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_host_tasks.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { OWNER_COUNT = 3 };
static turbowasm_name name(const char *text) {
    return (turbowasm_name){(const uint8_t *)text, (uint32_t)strlen(text)};
}
static turbowasm_component component;
static turbowasm_component_instance instance;
static turbowasm_component_async_transfer_result public_results[OWNER_COUNT];
static turbowasm_component_instance_public_impl *impl;
static turbowasm_component_host_task tasks[OWNER_COUNT];
static turbowasm_component_host_endpoint ends[OWNER_COUNT][2];
static turbowasm_component_host_transfer transfers[OWNER_COUNT];
static turbowasm_component_host_value output;
static turbowasm_component_host_value compound_cells[3];
static size_t allocation_calls, live_allocations, fail_at;
static bool close_loader;
static turbowasm_component_async_options *mutate_options;

static void *allocate(void *context, size_t size) {
    void *p;
    (void)context; ++allocation_calls;
    if (mutate_options != NULL) {
        memset(mutate_options, 0, sizeof(*mutate_options)); mutate_options = NULL;
    }
    if (close_loader) { close_loader = false; turbowasm_component_destroy(&component); }
    if (fail_at != 0u && allocation_calls == fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++live_allocations;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(live_allocations != 0u); --live_allocations; }
    free(p);
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
static void configure(const turbowasm_component_async_options *options) {
    turbowasm_component_instance_destroy(&instance);
    check_equal(turbowasm_component_instance_create_async_with_options_private(&instance,
        &component, options, NULL, 0u), TURBOWASM_OK);
    impl = turbowasm_component_instance_public_impl_get(&instance); attach();
}
static turbowasm_status create_task(unsigned index, const char *name,
    const turbowasm_component_host_value *arguments, size_t count) {
    turbowasm_name export_name = {(const uint8_t *)name, (uint32_t)strlen(name)};
    return turbowasm_component_host_task_create(&tasks[index], impl, export_name,
        arguments, count, false, &impl->host_budget);
}
static void finish(unsigned index) {
    check_equal(turbowasm_component_host_task_resume(&tasks[index], NULL), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    {
        const turbowasm_component_task *view = turbowasm_component_host_task_view(&tasks[index]);
        turbowasm_instance_impl *core = view->core_instance->impl;
        check_equal(core->jit_functions[view->binding.function_index].state, TURBOWASM_JIT_COMPILED);
    }
#endif
}
static void cancel_task(unsigned index) {
    check_equal(turbowasm_component_host_task_request_cancel(&tasks[index]), TURBOWASM_OK);
    check_equal(turbowasm_component_host_task_destroy(&tasks[index]), TURBOWASM_OK);
}
static uint32_t stream_type(void) {
    const turbowasm_component_type_graph *graph = &impl->exec.binary->type_graph;
    uint32_t i;
    for (i = 0u; i < graph->count; ++i) {
        const turbowasm_component_type *type = &graph->types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_STREAM && type->as.async_value.has_payload &&
            type->as.async_value.payload.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE &&
            type->as.async_value.payload.as.inline_type == TURBOWASM_COMPONENT_TYPE_U32)
            return i;
    }
    check(false); return UINT32_MAX;
}
static void pair(unsigned index) {
    check_equal(turbowasm_component_host_endpoint_pair_create(&ends[index][0], &ends[index][1],
        impl, stream_type(), &impl->host_budget), TURBOWASM_OK);
}
static void cancel_transfer(unsigned index) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_host_transfer_cancel(&transfers[index]), TURBOWASM_OK);
    check_equal(turbowasm_component_host_transfer_poll(&transfers[index], &event), TURBOWASM_OK);
}
static size_t task_charge(void) {
    size_t charge;
    check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK);
    charge = impl->host_budget.used; check_greater(charge, (size_t)1);
    cancel_task(0u); check_equal(impl->host_budget.used, (size_t)0);
    return charge;
}

spec("Instance-owned Component async options") {
    before_each() {
        turbowasm_runtime_config config;
        turbowasm_component_async_options options = {2u, 8u, 1u, 16384u};
        close_loader = false; mutate_options = NULL; fail_at = 0u;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_async_private(&component, component_host_tasks_bytes,
            sizeof(component_host_tasks_bytes), &config), TURBOWASM_OK);
        configure(&options);
    }
    after_each() {
        unsigned i;
        close_loader = false; mutate_options = NULL; fail_at = 0u;
        for (i = 0u; i < OWNER_COUNT; ++i) {
            const turbowasm_component_task *view = turbowasm_component_host_task_view(&tasks[i]);
            if (view != NULL && view->state < TURBOWASM_EXECUTION_COMPLETED)
                (void)turbowasm_component_host_task_request_cancel(&tasks[i]);
            check_equal(turbowasm_component_host_task_destroy(&tasks[i]), TURBOWASM_OK);
        }
        for (i = 0u; i < OWNER_COUNT; ++i) {
            turbowasm_component_host_transfer_state state;
            if (turbowasm_component_host_transfer_state_get(&transfers[i], &state) == TURBOWASM_OK && !state.terminal)
                cancel_transfer(i);
            check_equal(turbowasm_component_host_transfer_destroy(&transfers[i]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_endpoint_destroy(&ends[i][0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_endpoint_destroy(&ends[i][1]), TURBOWASM_OK);
        }
        for (i = 0u; i < OWNER_COUNT; ++i)
            check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[i]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&compound_cells[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&compound_cells[2]), TURBOWASM_OK);
        memset(compound_cells, 0, sizeof(compound_cells));
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        if (instance.impl != NULL) {
            check_equal(impl->host_budget.used, (size_t)0); check_equal(impl->host_transfer_count, 0u);
            check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
            check_equal(turbowasm_component_instance_poll_shutdown_private(impl, NULL), TURBOWASM_OK);
        }
        turbowasm_component_instance_destroy(&instance); impl = NULL;
        turbowasm_component_destroy(&component); check_equal(live_allocations, (size_t)0);
    }
    it("uses the designed task and typed endpoint interface without private graph indices") {
        turbowasm_component_type_token type = {0}, payload_type = {0};
        turbowasm_component_async_endpoint_type info = {0};
        turbowasm_component_async_endpoint_state endpoint_state;
        turbowasm_component_async_task_state state;
        turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 8u};
        turbowasm_status status = TURBOWASM_YIELDED;
        size_t count = 99u;
        unsigned turn;
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-stream"), 0u, &type), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_endpoint_type_get(&instance, type, &info), TURBOWASM_OK);
        check_false(info.future); check_true(info.has_payload);
        check_equal(turbowasm_component_instance_type_child(&instance, type,
            TURBOWASM_COMPONENT_TYPE_EDGE_PAYLOAD, 0u, &payload_type), TURBOWASM_OK);
        check_true(payload_type.inline_type); check_equal(payload_type.id, (uint32_t)TURBOWASM_COMPONENT_TYPE_U32);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_into_value(&ends[0][0], &output), TURBOWASM_OK);
        check_null(ends[0][0].impl); check_equal(output.kind, TURBOWASM_COMPONENT_HOST_STREAM);
        check_equal(turbowasm_component_async_endpoint_from_value(&ends[0][0], &output), TURBOWASM_OK);
        check_equal((int)output.kind, 0);
        check_equal(turbowasm_component_async_endpoint_into_value(&ends[0][0], &output), TURBOWASM_OK);
        check_equal(turbowasm_component_async_task_create(&tasks[0], &instance, name("echo-stream"), &output, 1u),
            TURBOWASM_INVALID_ARGUMENT);
        check_not_null(output.as.stream.impl); check_null(tasks[0].impl);
        check_equal(turbowasm_component_async_task_create_move(&tasks[0], &instance, name("echo-stream"), &output, 1u), TURBOWASM_OK);
        check_equal((int)output.kind, 0);
        check_equal(turbowasm_component_async_task_state_get(&tasks[0], &state), TURBOWASM_OK);
        check_false(state.terminal); check_equal(state.status, TURBOWASM_YIELDED);
        for (turn = 0u; turn < 128u && status == TURBOWASM_YIELDED; ++turn)
            status = turbowasm_component_async_task_resume(&tasks[0], &options);
        check_equal(status, TURBOWASM_OK);
        check_equal(turbowasm_component_async_task_state_get(&tasks[0], &state), TURBOWASM_OK);
        check_true(state.terminal); check_false(state.cancelled); check_false(state.result_taken);
        check_equal(state.result_count, (size_t)1);
        check_equal(turbowasm_component_async_task_take_result(&tasks[0], &output, &count), TURBOWASM_OK);
        check_equal(count, (size_t)1);
        check_equal(turbowasm_component_async_task_state_get(&tasks[0], &state), TURBOWASM_OK); check_true(state.result_taken);
        check_equal(turbowasm_component_async_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_from_value(&ends[0][0], &output), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_destroy(&ends[0][1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_state_get(&ends[0][0], &endpoint_state), TURBOWASM_OK);
        check_true(endpoint_state.peer_dropped); check_true(endpoint_state.readable);
        check_equal(turbowasm_component_async_endpoint_destroy(&ends[0][0]), TURBOWASM_OK);
        check_equal(impl->host_budget.used, (size_t)0);
    }
    it("authenticates instance type tokens and preserves failed query and pair outputs") {
        turbowasm_component_instance other = {0};
        turbowasm_component_type_token tuple = {0}, future = {0}, sentinel;
        turbowasm_component_async_endpoint_type info = {0};
        check_equal(turbowasm_component_instance_create_async_with_options(&other, &component, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-compound64"), 0u, &tuple), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_type_child(&instance, tuple,
            TURBOWASM_COMPONENT_TYPE_EDGE_FIELD, 2u, &future), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_endpoint_type_get(&instance, future, &info), TURBOWASM_OK);
        check_true(info.future); sentinel = future;
        turbowasm_status query = turbowasm_component_instance_parameter_type(&instance, name("missing"), 0u, &future);
        check_equal(query, TURBOWASM_LINK_ERROR); check_equal(future.scope, sentinel.scope); check_equal(future.id, sentinel.id);
        query = turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &other, future);
        turbowasm_component_instance_destroy(&other);
        check_equal(query, TURBOWASM_INVALID_ARGUMENT); check_null(ends[0][0].impl); check_null(ends[0][1].impl);
        check_equal(turbowasm_component_instance_type_child(&instance, tuple,
            TURBOWASM_COMPONENT_TYPE_EDGE_FIELD, 99u, &future), TURBOWASM_INVALID_ARGUMENT);
        check_equal(future.id, sentinel.id);
    }
    it("drains the designed shutdown interface only after task cancellation is acknowledged and retired") {
        turbowasm_component_async_task_state task_state;
        turbowasm_component_async_shutdown_state state;
        size_t count = 99u;
        void *carrier = instance.impl;
        check_equal(turbowasm_component_async_task_create(&tasks[0], &instance, name("answer"), NULL, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown(&instance), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown(&instance, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_instance_shutdown_state_get(&instance, &state), TURBOWASM_OK);
        check_true(state.requested); check_false(state.complete);
        check_equal(turbowasm_component_async_task_state_get(&tasks[0], &task_state), TURBOWASM_OK);
        check_true(task_state.cancellation_requested);
        turbowasm_component_instance_destroy(&instance); check_equal(instance.impl, carrier);
        check_equal(turbowasm_component_async_task_resume(&tasks[0], NULL), TURBOWASM_INTERRUPTED);
        check_equal(turbowasm_component_async_task_state_get(&tasks[0], &task_state), TURBOWASM_OK);
        check_true(task_state.cancelled); check_equal(task_state.status, TURBOWASM_INTERRUPTED);
        check_equal(turbowasm_component_async_task_take_result(&tasks[0], &output, &count), TURBOWASM_INTERRUPTED);
        check_equal(count, (size_t)99); check_equal((int)output.kind, 0);
        check_equal(turbowasm_component_async_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_poll_shutdown(&instance, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_shutdown_state_get(&instance, &state), TURBOWASM_OK);
        check_true(state.complete); check_equal(state.status, TURBOWASM_OK);
    }
    it("publishes a received prefix and unsent write tail with their original endpoint roles") {
        turbowasm_component_async_options options = {2u, 16u, 2u, 65536u};
        turbowasm_component_type_token type;
        turbowasm_component_host_value values[3] = {
            {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 42u},
            {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 43u},
            {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 44u}};
        turbowasm_component_async_transfer_state state;
        size_t used, limit;
        unsigned failure;
        bool delivered = false;
        configure(&options);
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-stream"), 0u, &type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_write(&transfers[1], &ends[0][1], values, 3u), TURBOWASM_OK);
        values[1].as.u32 = values[2].as.u32 = 0u;
        check_null(ends[0][1].impl);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[1], &public_results[1]), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_async_transfer_read(&transfers[0], &ends[0][0], 1u, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_false(state.terminal); check_equal(state.progress, 1u);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_request_cancel(&transfers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_state_get(&transfers[1], &state), TURBOWASM_OK);
        check_true(state.terminal); check_equal(state.progress, 1u);
        check_equal(state.outcome, TURBOWASM_COMPONENT_ASYNC_TRANSFER_COMPLETED);
        used = impl->host_budget.used; limit = impl->host_budget.limit;
        impl->host_budget.limit = used;
        turbowasm_status status = turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]);
        impl->host_budget.limit = limit;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(public_results[0].endpoint.impl);
        check_equal((int)public_results[0].values.kind, 0); check_equal(impl->host_budget.used, used);
        for (failure = 1u; failure < 16u; ++failure) {
            fail_at = allocation_calls + failure;
            status = turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]);
            fail_at = 0u;
            if (status == TURBOWASM_OK) { delivered = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(public_results[0].endpoint.impl);
            check_equal((int)public_results[0].values.kind, 0); check_equal(impl->host_budget.used, used);
        }
        check_true(delivered); check_greater(failure, 1u);
        check_equal(public_results[0].first_index, 0u); check_equal(public_results[0].logical_count, 1u);
        check_equal(public_results[0].values.kind, TURBOWASM_COMPONENT_HOST_LIST);
        check_equal(public_results[0].values.as.list.items[0].as.u32, 42u);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[2]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[1], &public_results[1]), TURBOWASM_OK);
        check_equal(public_results[1].first_index, 1u); check_equal(public_results[1].logical_count, 2u);
        check_equal(public_results[1].values.as.list.count, (size_t)2);
        check_equal(public_results[1].values.as.list.items[0].as.u32, 43u);
        check_equal(public_results[1].values.as.list.items[1].as.u32, 44u);
        check_equal(turbowasm_component_async_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_destroy(&transfers[1]), TURBOWASM_OK);
        check_greater(impl->host_budget.used, (size_t)0);
        check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[1]), TURBOWASM_OK);
        check_equal(impl->host_budget.used, (size_t)0);
    }
    it("cancels a pending transfer once and returns every unsent copied value") {
        turbowasm_component_type_token type;
        turbowasm_component_host_value value = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 77u};
        turbowasm_component_async_transfer_state state;
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-stream"), 0u, &type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_write(&transfers[0], &ends[0][1], &value, 1u), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_async_transfer_destroy(&transfers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_async_transfer_request_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_request_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_false(state.terminal);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_equal(state.outcome, TURBOWASM_COMPONENT_ASYNC_TRANSFER_CANCELLED);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]), TURBOWASM_OK);
        check_equal(public_results[0].first_index, 0u); check_equal(public_results[0].logical_count, 1u);
        check_equal(public_results[0].values.as.list.items[0].as.u32, 77u);
    }
    it("delivers typed and unit futures in either submission order and preserves logical unit counts") {
        turbowasm_component_async_options options = {2u, 16u, 2u, 65536u};
        turbowasm_component_type_token type;
        turbowasm_component_async_endpoint_state end_state;
        turbowasm_component_host_value value = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 91u};
        unsigned variant;
        configure(&options);
        for (variant = 0u; variant < 4u; ++variant) {
            bool unit = variant >= 2u;
            check_equal(turbowasm_component_instance_parameter_type(&instance,
                name(unit ? "echo-unit-future" : "echo-future"), 0u, &type), TURBOWASM_OK);
            check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
            if ((variant & 1u) != 0u)
                check_equal(turbowasm_component_async_transfer_read(&transfers[0], &ends[0][0], 1u, 0u), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_write(&transfers[1], &ends[0][1], unit ? NULL : &value, 1u), TURBOWASM_OK);
            if ((variant & 1u) == 0u)
                check_equal(turbowasm_component_async_transfer_read(&transfers[0], &ends[0][0], 1u, 0u), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_poll(&transfers[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_take_result(&transfers[1], &public_results[1]), TURBOWASM_OK);
            check_equal(public_results[0].logical_count, 1u); check_equal(public_results[1].logical_count, 0u);
            if (unit) { check_equal((int)public_results[0].values.kind, 0); check_equal((int)public_results[1].values.kind, 0); }
            else check_equal(public_results[0].values.as.list.items[0].as.u32, 91u);
            check_equal(turbowasm_component_async_endpoint_state_get(&public_results[0].endpoint, &end_state), TURBOWASM_OK);
            check_true(end_state.future); check_true(end_state.finished); check_true(end_state.readable);
            check_equal(turbowasm_component_async_transfer_destroy(&transfers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_destroy(&transfers[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[1]), TURBOWASM_OK);
            check_equal(impl->host_budget.used, (size_t)0);
        }
    }
    it("returns peer closure separately from successful zero-length stream readiness") {
        turbowasm_component_type_token type;
        turbowasm_component_async_transfer_state state;
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-stream"), 0u, &type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_destroy(&ends[0][1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_read(&transfers[0], &ends[0][0], 1u, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_equal(state.outcome, TURBOWASM_COMPONENT_ASYNC_TRANSFER_PEER_DROPPED); check_equal(state.progress, 0u);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]), TURBOWASM_OK);
        check_equal(public_results[0].logical_count, 0u);
    }
    it("moves a composite payload atomically after every admission allocation succeeds") {
        turbowasm_component_async_options options = {2u, 32u, 2u, 131072u};
        turbowasm_component_type_token type, future;
        turbowasm_component_host_value rep = {.kind = TURBOWASM_COMPONENT_HOST_S32, .as.s32 = 42};
        turbowasm_component_host_value tuple = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE,
            .as.tuple = {compound_cells, 3u}};
        uint8_t text[] = {'a', 'b', 0, 'c'};
        size_t count, used, live, refs;
        turbowasm_trap trap;
        void *resource, *reader, *writer;
        unsigned failure;
        bool admitted = false;
        configure(&options);
        check_equal(turbowasm_component_instance_invoke(&instance, name("make"), &rep, 1u, &output, 1u, &count, &trap), TURBOWASM_OK);
        compound_cells[0] = (turbowasm_component_host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {text, sizeof(text)}};
        compound_cells[1] = output; memset(&output, 0, sizeof(output));
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-compound-stream"), 0u, &type), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_parameter_type(&instance, name("echo-future"), 0u, &future), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[0][0], &ends[0][1], &instance, type), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_pair_create(&ends[1][0], &ends[1][1], &instance, future), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_into_value(&ends[1][0], &compound_cells[2]), TURBOWASM_OK);
        resource = compound_cells[1].as.own; reader = compound_cells[2].as.future.impl; writer = ends[0][1].impl;
        used = impl->host_budget.used; live = live_allocations; refs = impl->ref_count;
        check_equal(turbowasm_component_async_transfer_write(&transfers[1], &ends[0][1], &tuple, 1u), TURBOWASM_INVALID_ARGUMENT);
        for (failure = 1u; failure < 64u; ++failure) {
            fail_at = allocation_calls + failure;
            turbowasm_status status = turbowasm_component_async_transfer_write_move(&transfers[1], &ends[0][1], &tuple, 1u);
            fail_at = 0u;
            if (status == TURBOWASM_OK) { admitted = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(transfers[1].impl);
            check_equal((const void *)compound_cells[1].as.own, resource); check_equal(compound_cells[2].as.future.impl, reader);
            check_equal(ends[0][1].impl, writer); check_equal(impl->host_budget.used, used);
            check_equal(live_allocations, live); check_equal((size_t)impl->ref_count, refs);
        }
        check_true(admitted); check_greater(failure, 2u);
        check_equal((int)compound_cells[1].kind, 0); check_equal((int)compound_cells[2].kind, 0); check_null(ends[0][1].impl);
        text[0] = 'z';
        check_equal(turbowasm_component_async_transfer_read(&transfers[0], &ends[0][0], 1u, 4096u), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_poll(&transfers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[0], &public_results[0]), TURBOWASM_OK);
        turbowasm_component_host_value *received = public_results[0].values.as.list.items[0].as.tuple.items;
        check_equal(received[0].as.string.size, sizeof(text)); check_equal(received[0].as.string.data[0], (uint8_t)'a');
        check_equal(received[1].kind, TURBOWASM_COMPONENT_HOST_OWN); check_equal(received[2].kind, TURBOWASM_COMPONENT_HOST_FUTURE);
        check_equal(turbowasm_component_async_transfer_take_result(&transfers[1], &public_results[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_destroy(&transfers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_transfer_result_destroy(&public_results[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_async_endpoint_destroy(&ends[1][1]), TURBOWASM_OK);
        check_equal(impl->host_budget.used, (size_t)0);
    }
    it("initializes finite independent defaults and copies options before allocator reentry") {
        turbowasm_component_async_options options;
        turbowasm_component_async_options_init_private(&options);
        check_equal(options.tasks, 64u); check_equal(options.handles, 4096u); check_equal(options.transfers, 64u);
        check_equal(options.host_bytes, (size_t)(16u * 1024u * 1024u));
        mutate_options = &options; configure(&options);
        check_null(mutate_options); check_equal(options.host_bytes, (size_t)0);
        check_equal(impl->exec.task_domain.limit, 64u); check_equal(impl->exec.resource_table.max_entries, 4096u);
        check_equal(impl->host_transfer_limit, 64u); check_equal(impl->host_budget.limit, (size_t)(16u * 1024u * 1024u));
        check_true(impl->host_budget_owned);
    }
    it("keeps the binary and allocator configuration alive when the first constructor allocation closes its loader") {
        turbowasm_component_async_options options = {1u, 2u, 1u, 16384u};
        close_loader = true; configure(&options); check_null(component.impl); check_false(close_loader);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK); finish(0u);
    }
    it("rejects zero, unlimited and unrepresentable options before allocation") {
        turbowasm_component_async_options valid = {2u, 8u, 1u, 16384u};
        unsigned invalid; size_t calls = allocation_calls;
        for (invalid = 0u; invalid < 9u; ++invalid) {
            turbowasm_component_async_options options = valid;
            turbowasm_component_instance empty = {0};
            switch (invalid) {
                case 0: options.tasks = 0u; break;
                case 1: options.tasks = UINT32_MAX; break;
                case 2: options.handles = 0u; break;
                case 3: options.handles = TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS + 1u; break;
                case 4: options.transfers = 0u; break;
                case 5: options.transfers = UINT32_MAX; break;
                case 6: options.host_bytes = 0u; break;
                case 7: options.host_bytes = SIZE_MAX; break;
                default: break;
            }
            check_equal(turbowasm_component_instance_create_async_with_options_private(&empty, &component,
                invalid == 8u ? NULL : &options, NULL, 0u), TURBOWASM_INVALID_ARGUMENT);
            check_null(empty.impl); check_equal(allocation_calls, calls);
        }
    }
    it("holds task quota through completed Core exit and recovers it on owner destruction") {
        turbowasm_component_async_options options = {1u, 8u, 2u, 16384u};
        size_t used;
        configure(&options); check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK);
        used = impl->host_budget.used; finish(0u);
        check_equal(create_task(1u, "answer", NULL, 0u), TURBOWASM_OUT_OF_MEMORY);
        check_null(tasks[1].impl); check_equal(impl->host_budget.used, used);
        check_equal(turbowasm_component_host_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(create_task(1u, "answer", NULL, 0u), TURBOWASM_OK); cancel_task(1u);
    }
    it("enforces exact host-byte boundaries without changing the immutable limit") {
        size_t bytes = task_charge();
        turbowasm_component_async_options options = {2u, 8u, 1u, bytes - 1u};
        configure(&options);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OUT_OF_MEMORY);
        check_null(tasks[0].impl); check_equal(impl->host_budget.used, (size_t)0);
        options.host_bytes = bytes; configure(&options);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK);
        check_equal(impl->host_budget.used, bytes);
        check_equal(create_task(1u, "answer", NULL, 0u), TURBOWASM_OUT_OF_MEMORY); check_null(tasks[1].impl);
        cancel_task(0u); check_equal(create_task(1u, "answer", NULL, 0u), TURBOWASM_OK); cancel_task(1u);
        check_equal(impl->host_budget.limit, bytes);
    }
    it("shares byte quota with result promotion and retries after a sibling releases storage") {
        size_t bytes = task_charge(), count = 99u;
        turbowasm_component_async_options options = {2u, 8u, 1u, 2u * bytes};
        configure(&options);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK);
        check_equal(create_task(1u, "answer", NULL, 0u), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&tasks[0], &output, &count), TURBOWASM_OUT_OF_MEMORY);
        check_equal(count, (size_t)99); check_equal((int)output.kind, 0);
        cancel_task(1u);
        check_equal(turbowasm_component_host_task_take_result(&tasks[0], &output, &count), TURBOWASM_OK);
        check_equal(output.as.u32, 42u); check_equal(count, (size_t)1);
        check_equal(impl->host_budget.limit, options.host_bytes);
    }
    it("bounds actual canonical handle registration separately from host transfers") {
        turbowasm_component_async_options options = {2u, 2u, 3u, 16384u};
        turbowasm_component_endpoint *read = NULL, *write = NULL, *extra_read = NULL, *extra_write = NULL;
        turbowasm_runtime_scope scope;
        configure(&options); scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&impl->exec.task_domain, &impl->exec.binary->type_graph,
            stream_type(), true, &read, &write), TURBOWASM_OK);
        check_equal(impl->exec.resource_table.live_count, 2u);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&impl->exec.task_domain, &impl->exec.binary->type_graph,
            stream_type(), true, &extra_read, &extra_write), TURBOWASM_OUT_OF_MEMORY);
        check_null(extra_read); check_null(extra_write); check_equal(impl->exec.resource_table.live_count, 2u);
        check_equal(turbowasm_component_endpoint_close(read), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_close(write), TURBOWASM_OK);
        turbowasm_runtime_scope_leave(scope);
        check_equal(impl->host_transfer_limit, 3u); check_equal(impl->exec.resource_table.live_count, 0u);
    }
    it("holds independent transfer quota through terminal delivery until destruction") {
        turbowasm_component_value values[2] = {{.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u},
            {.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 43u}};
        void *writer; size_t used;
        pair(0u); pair(1u); writer = ends[1][1].impl;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &ends[0][1], &values[0], 1u,
            &impl->host_budget, NULL), TURBOWASM_OK);
        used = impl->host_budget.used;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &ends[1][1], &values[1], 1u,
            &impl->host_budget, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_true(ends[1][1].impl == writer); check_null(transfers[1].impl); check_equal(values[1].as.u32, 43u);
        check_equal(impl->host_budget.used, used); check_equal(impl->host_transfer_count, 1u);
        cancel_transfer(0u);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &ends[1][1], &values[1], 1u,
            &impl->host_budget, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &ends[1][1], &values[1], 1u,
            &impl->host_budget, NULL), TURBOWASM_OK);
        check_equal(impl->exec.task_domain.count, 0u); check_equal(impl->exec.resource_table.live_count, 0u);
    }
    it("rejects a substitute budget for tasks, arguments, results, endpoints and transfers") {
        turbowasm_component_host_budget substitute = {16384u, 0u};
        turbowasm_component_host_arguments arguments = {0}; turbowasm_component_host_result result = {0};
        turbowasm_component_value value = {.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u}, endpoint = {0};
        turbowasm_name name = {(const uint8_t *)"answer", 6u}; const turbowasm_component_task *view;
        const turbowasm_component_type *type; size_t used;
        check_equal(turbowasm_component_host_task_create(&tasks[0], impl, name, NULL, 0u, false, &substitute), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_pair_create(&ends[0][0], &ends[0][1], impl,
            stream_type(), &substitute), TURBOWASM_INVALID_ARGUMENT);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK);
        view = turbowasm_component_host_task_view(&tasks[0]); type = turbowasm_component_type_graph_get(view->binding.graph, view->binding.function_type);
        used = impl->host_budget.used;
        check_equal(turbowasm_component_host_arguments_prepare(&arguments, impl, view->binding.graph, view->binding.function_type,
            NULL, 0u, false, true, &substitute), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_result_prepare(&result, impl, view->binding.graph, type->as.function.result,
            &value, &substitute), TURBOWASM_INVALID_ARGUMENT);
        check_equal(impl->host_budget.used, used); check_equal(value.as.u32, 42u);
        pair(0u);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &ends[0][1], &value, 1u,
            &substitute, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_not_null(ends[0][1].impl); check_equal(value.as.u32, 42u); check_equal(substitute.used, (size_t)0);
        check_equal(turbowasm_component_host_endpoint_into_value(&ends[0][0], &endpoint), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_from_value(&ends[0][0], impl, &endpoint, &substitute), TURBOWASM_INVALID_ARGUMENT);
        check_not_null(turbowasm_component_endpoint_value_get(&endpoint));
        check_equal(turbowasm_component_value_destroy(&endpoint), TURBOWASM_OK);
    }
    it("keeps its byte budget alive after public carriers close while a task owns deferred string storage") {
        uint8_t text[] = {'a', 0, 'b'}; size_t count = 99u;
        turbowasm_component_host_value argument = {.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {text, sizeof(text)}};
        check_equal(create_task(0u, "echo32", &argument, 1u), TURBOWASM_OK);
        memset(text, 'x', sizeof(text));
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&tasks[0], &output, &count), TURBOWASM_OK);
        check_equal(output.as.string.data, "a\0b", 3u); check_equal(output.as.string.size, (size_t)3);
        check_equal(turbowasm_component_host_task_destroy(&tasks[0]), TURBOWASM_OK); impl = NULL;
    }
    it("holds delivered string capacity until destruction and makes that quota reusable") {
        uint8_t text[] = {'a', 0, 'b'};
        size_t count, limit;
        turbowasm_component_host_value argument = {.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {text, sizeof(text)}};
        check_equal(create_task(0u, "echo32", &argument, 1u), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_host_task_take_result(&tasks[0], &output, &count), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(impl->host_budget.used, sizeof(text)); limit = impl->host_budget.limit;
        impl->host_budget.limit = sizeof(text);
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OUT_OF_MEMORY); check_null(tasks[0].impl);
        check_equal(output.as.string.data, text, sizeof(text));
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(impl->host_budget.used, (size_t)0); impl->host_budget.limit = limit;
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK); cancel_task(0u);
    }
    it("rolls back every configured constructor allocation without publishing an instance") {
        turbowasm_component_async_options options = {2u, 8u, 1u, 16384u};
        turbowasm_component_instance local = {0};
        size_t before = allocation_calls, live = live_allocations, count, failure;
        check_equal(turbowasm_component_instance_create_async_with_options_private(&local, &component, &options, NULL, 0u), TURBOWASM_OK);
        count = allocation_calls - before; turbowasm_component_instance_destroy(&local);
        check_equal(live_allocations, live);
        for (failure = 1u; failure <= count; ++failure) {
            fail_at = allocation_calls + failure;
            check_equal(turbowasm_component_instance_create_async_with_options_private(&local, &component,
                &options, NULL, 0u), TURBOWASM_OUT_OF_MEMORY);
            fail_at = 0u; check_null(local.impl); check_equal(live_allocations, live);
        }
    }
    it("returns owned byte quota and keeps inputs intact after each task startup allocation failure") {
        size_t before = allocation_calls, count, failure, live = live_allocations;
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK); count = allocation_calls - before;
        cancel_task(0u); check_equal(live_allocations, live);
        for (failure = 1u; failure <= count; ++failure) {
            fail_at = allocation_calls + failure;
            check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OUT_OF_MEMORY);
            fail_at = 0u; check_null(tasks[0].impl); check_equal(impl->host_budget.used, (size_t)0);
            check_equal(impl->exec.task_domain.count, 0u); check_equal(live_allocations, live);
        }
        check_equal(create_task(0u, "answer", NULL, 0u), TURBOWASM_OK); cancel_task(0u);
    }
    it("releases its retained binary when loader closure and allocation failure coincide") {
        turbowasm_component_async_options options = {2u, 8u, 1u, 16384u};
        turbowasm_component_instance_destroy(&instance); impl = NULL;
        close_loader = true; fail_at = allocation_calls + 1u;
        check_equal(turbowasm_component_instance_create_async_with_options_private(&instance,
            &component, &options, NULL, 0u), TURBOWASM_OUT_OF_MEMORY);
        fail_at = 0u; check_null(instance.impl); check_null(component.impl); check_equal(live_allocations, (size_t)0);
    }
    it("rolls back every endpoint-pair allocation into the instance budget") {
        size_t before = allocation_calls, count, failure, live = live_allocations;
        pair(0u); count = allocation_calls - before;
        check_equal(turbowasm_component_host_endpoint_destroy(&ends[0][0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&ends[0][1]), TURBOWASM_OK);
        check_equal(impl->host_budget.used, (size_t)0); check_equal(live_allocations, live);
        for (failure = 1u; failure <= count; ++failure) {
            fail_at = allocation_calls + failure;
            check_equal(turbowasm_component_host_endpoint_pair_create(&ends[0][0], &ends[0][1],
                impl, stream_type(), &impl->host_budget), TURBOWASM_OUT_OF_MEMORY);
            fail_at = 0u; check_null(ends[0][0].impl); check_null(ends[0][1].impl);
            check_equal(impl->host_budget.used, (size_t)0); check_equal(impl->exec.task_domain.pair_count, 0u);
            check_equal(live_allocations, live);
        }
        pair(0u);
    }
    it("returns transfer count and bytes on every allocation failure before moving the writer or payload") {
        turbowasm_component_value value = {.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u};
        size_t before, count, failure, used, live; void *writer;
        pair(0u); used = impl->host_budget.used; live = live_allocations; before = allocation_calls;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &ends[0][1], &value, 1u,
            &impl->host_budget, NULL), TURBOWASM_OK);
        count = allocation_calls - before; cancel_transfer(0u);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &ends[0][1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        writer = ends[0][1].impl;
        for (failure = 1u; failure <= count; ++failure) {
            value = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u};
            fail_at = allocation_calls + failure;
            check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &ends[0][1], &value, 1u,
                &impl->host_budget, NULL), TURBOWASM_OUT_OF_MEMORY);
            fail_at = 0u; check_null(transfers[0].impl); check_true(ends[0][1].impl == writer);
            check_equal(value.kind, TURBOWASM_COMPONENT_TYPE_U32); check_equal(value.as.u32, 42u);
            check_equal(impl->host_budget.used, used); check_equal(impl->host_transfer_count, 0u);
            check_equal(live_allocations, live);
        }
    }
}
