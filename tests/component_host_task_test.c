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
static bool reenter_endpoint_on_allocate;
static turbowasm_component_host_endpoint host_ends[OWNER_COUNT];
static turbowasm_component_endpoint loose_ends[2];
static turbowasm_component_host_transfer transfers[OWNER_COUNT];
static turbowasm_component_value transfer_inputs[OWNER_COUNT];
static bool reenter_transfer_on_allocate;
static const turbowasm_component_endpoint *copy_reentry_end;
static bool shutdown_on_allocate;

static uint32_t endpoint_type(turbowasm_component_instance_public_impl *owner, bool future) {
    const turbowasm_component_type_graph *graph = &owner->exec.binary->type_graph;
    uint32_t i;
    for (i = 0u; i < graph->count; ++i)
        if (graph->types[i].kind == (future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM)) return i;
    return UINT32_MAX;
}
static uint32_t payload_endpoint_type(bool future, bool has_payload, turbowasm_component_type_kind payload_kind) {
    const turbowasm_component_type_graph *graph = &impl->exec.binary->type_graph;
    uint32_t i;
    for (i = 0u; i < graph->count; ++i) {
        const turbowasm_component_type *definition = &graph->types[i], *payload_type;
        turbowasm_component_type_kind kind;
        if (definition->kind != (future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM) ||
            definition->as.async_value.has_payload != has_payload) continue;
        if (!has_payload) return i;
        if (definition->as.async_value.payload.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
            kind = definition->as.async_value.payload.as.inline_type;
        else {
            payload_type = turbowasm_component_type_graph_get(graph, definition->as.async_value.payload.as.indexed);
            kind = payload_type->kind;
        }
        if (kind == payload_kind) return i;
    }
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
    if (shutdown_on_allocate) {
        bool closed = impl->admission_closed;
        shutdown_on_allocate = false;
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_INVALID_ARGUMENT);
        check_equal(impl->admission_closed, closed);
    }
    if (reenter_transfer_on_allocate && transfers[0].impl != NULL &&
        (copy_reentry_end == NULL || copy_reentry_end->waitable.delivering)) {
        turbowasm_component_host_transfer_state state = {.progress = 99u};
        turbowasm_component_event event;
        uint32_t count = 99u;
        reenter_transfer_on_allocate = false;
        copy_reentry_end = NULL;
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_INVALID_ARGUMENT);
        check_false(impl->admission_closed);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_INVALID_ARGUMENT);
        check_equal(state.progress, 99u);
        check_null(turbowasm_component_host_transfer_values(&transfers[0], &count)); check_equal(count, 99u);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[2]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_INVALID_ARGUMENT);
    }
    if (reenter_endpoint_on_allocate) {
        turbowasm_component_value value = {0};
        turbowasm_component_event event;
        reenter_endpoint_on_allocate = false;
        check_null(turbowasm_component_host_endpoint_view(&host_ends[0]));
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &value), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_submit(&host_ends[0], NULL, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_take(&host_ends[0], &event), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_cancel(&host_ends[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal((int)value.kind, 0);
    }
    if (close_on_allocate) {
        close_on_allocate = reenter_endpoint_on_allocate = false;
        memset(loose_ends, 0, sizeof(loose_ends));
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
    check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_INVALID_ARGUMENT);
    check_false(impl->admission_closed);
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

static void transfer_text(turbowasm_component_value *value, const char *text) {
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
    value->kind = TURBOWASM_COMPONENT_TYPE_STRING; value->as.string.size = strlen(text);
    value->as.string.data = turbowasm_rt_malloc(value->as.string.size);
    turbowasm_runtime_scope_leave(scope);
    check_not_null(value->as.string.data); memcpy(value->as.string.data, text, value->as.string.size);
}
static void transfer_own(turbowasm_component_value *out, int32_t rep) {
    const turbowasm_component_binary *binary = impl->exec.binary;
    turbowasm_component_exec_resource_codec codec = {0};
    turbowasm_component_canonical_memory memory = {0};
    turbowasm_component_core_call_adapter *binding = NULL;
    const turbowasm_component_type *function;
    turbowasm_value argument = {.kind = TURBOWASM_VALUE_I32, .as.i32 = rep}, result_value;
    turbowasm_trap trap;
    turbowasm_runtime_scope scope;
    size_t count;
    uint32_t i;
    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *item = &binary->exports[i];
        if (item->kind == TURBOWASM_COMPONENT_EXTERN_FUNCTION && item->name.size == 4u &&
            memcmp(item->name.bytes, "make", 4u) == 0) {
            binding = &impl->exec.functions[impl->exec.function_adapter_indices[item->item_index]]; break;
        }
    }
    check_not_null(binding);
    check_equal(turbowasm_instance_invoke(binding->instance, binding->function_index,
        &argument, 1u, &result_value, 1u, &count, &trap), TURBOWASM_OK);
    check_equal(count, (size_t)1);
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)binding->instance->impl)->jit_functions[
        binding->function_index].state, TURBOWASM_JIT_COMPILED);
#endif
    function = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    turbowasm_component_exec_resource_codec_bind(&codec, &impl->exec, &memory);
    scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
    turbowasm_status status = memory.resource_lift(memory.resource_context, binding->graph,
        function->as.function.result, (uint32_t)result_value.as.i32, out);
    turbowasm_runtime_scope_leave(scope);
    check_equal(status, TURBOWASM_OK);
}
static void transfer_pair(uint32_t type) {
    check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl, type, &budget),
        TURBOWASM_OK);
}
static void transfer_compound(int32_t rep) {
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
    transfer_inputs[0].kind = TURBOWASM_COMPONENT_TYPE_TUPLE; transfer_inputs[0].as.tuple.count = 3u;
    transfer_inputs[0].as.tuple.items = turbowasm_rt_calloc(3u, sizeof(turbowasm_component_value));
    turbowasm_runtime_scope_leave(scope);
    check_not_null(transfer_inputs[0].as.tuple.items);
    transfer_text(&transfer_inputs[0].as.tuple.items[0], "abc");
    transfer_own(&transfer_inputs[0].as.tuple.items[1], rep);
    check_equal(open_pair(0u, true, false), TURBOWASM_OK);
    scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
    turbowasm_status status = turbowasm_component_endpoint_into_value(pair_ends[0][0],
        &transfer_inputs[0].as.tuple.items[2]);
    turbowasm_runtime_scope_leave(scope); check_equal(status, TURBOWASM_OK);
}
static size_t compound_bytes(void) {
    return 3u * sizeof(turbowasm_component_value) + 3u +
        turbowasm_component_exec_resource_adopt_size() + sizeof(turbowasm_component_endpoint_value_owner);
}

spec("Retained Component host task owners") {
    before_each() {
        turbowasm_runtime_config config;
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        memset(&allocations, 0, sizeof(allocations)); budget = (turbowasm_component_host_budget){BYTE_LIMIT, 0u};
        close_on_allocate = false;
        shutdown_on_allocate = false;
        reenter_transfer_on_allocate = false;
        copy_reentry_end = NULL;
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
        shutdown_on_allocate = false;
        reenter_transfer_on_allocate = false;
        copy_reentry_end = NULL;
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
        for (i = 0u; i < OWNER_COUNT; ++i) {
            turbowasm_component_host_transfer_state state;
            turbowasm_component_event event;
            if (turbowasm_component_host_transfer_state_get(&transfers[i], &state) == TURBOWASM_OK && !state.terminal) {
                (void)turbowasm_component_host_transfer_cancel(&transfers[i]);
                (void)turbowasm_component_host_transfer_poll(&transfers[i], &event);
            }
            (void)turbowasm_component_host_transfer_destroy(&transfers[i]); check_null(transfers[i].impl);
            (void)turbowasm_component_value_destroy(&transfer_inputs[i]);
        }
        if (endpoint_codec.table != NULL)
            check_equal(turbowasm_component_endpoint_codec_rollback(&endpoint_codec), TURBOWASM_OK);
        if ((int)endpoint_value.kind != 0) {
            turbowasm_component_endpoint *taken = NULL;
            if (turbowasm_component_endpoint_take_value(&endpoint_value, &taken) == TURBOWASM_OK && pair_ends[0][0] == NULL)
                pair_ends[0][0] = taken;
            (void)turbowasm_component_value_destroy(&endpoint_value);
        }
        for (i = 0u; i < OWNER_COUNT; ++i) {
            const turbowasm_component_endpoint *view = turbowasm_component_host_endpoint_view(&host_ends[i]);
            if (view != NULL && view->operation != NULL) {
                turbowasm_component_event event;
                if (view->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                    (void)turbowasm_component_host_endpoint_cancel(&host_ends[i]);
                (void)turbowasm_component_host_endpoint_take(&host_ends[i], &event);
            }
            check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[i]), TURBOWASM_OK);
        }
        for (i = 0u; i < 2u; ++i) close_pair(i);
        for (i = 0u; i < 2u; ++i) if (loose_ends[i].initialized && !loose_ends[i].closed)
            check_equal(turbowasm_component_endpoint_close(&loose_ends[i]), TURBOWASM_OK);
        if (destination.impl != NULL) {
            turbowasm_component_instance_public_impl *target = turbowasm_component_instance_public_impl_get(&destination);
            for (i = 0u; i < target->exec.resource_table.capacity; ++i) {
                uint32_t handle; turbowasm_component_handle_kind kind; void *object;
                if (turbowasm_component_handle_at(&target->exec.resource_table, i, &handle, &kind, &object)) {
                    turbowasm_component_endpoint *end = turbowasm_component_endpoint_get(&target->exec.resource_table, handle, kind);
                    if (end != NULL) check_equal(turbowasm_component_endpoint_close(end), TURBOWASM_OK);
                }
            }
        }
        memset(&endpoint_codec, 0, sizeof(endpoint_codec)); memset(&write_buffer, 0, sizeof(write_buffer));
        (void)turbowasm_component_value_destroy(&payload);
        turbowasm_component_instance_destroy(&destination);
        (void)turbowasm_component_host_value_destroy(&resource);
        (void)turbowasm_component_host_value_destroy(&output);
        if (instance.impl != NULL) {
            check_equal(impl->host_activity, 0u);
            check_null(impl->host_owners);
        }
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
    it("closes admission without consuming new move inputs or changing call outputs") {
        turbowasm_component_host_arguments arguments = {0};
        turbowasm_component_call call = {0};
        const turbowasm_component_task_binding *binding;
        turbowasm_component_host_value value = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 99u};
        turbowasm_trap trap = TURBOWASM_TRAP_UNREACHABLE;
        size_t count = 99u, used, attempts;
        void *owned;
        make(42); owned = resource.as.own;
        check_equal(turbowasm_component_exec_async_export(&impl->exec, name("consume").bytes, 7u, &binding), TURBOWASM_OK);
        used = budget.used; attempts = allocations.attempts;
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_true(impl->admission_closed);
        check_equal(create(0u, "consume", &resource, 1u, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_prepare(&arguments, impl, binding->graph, binding->function_type,
            &resource, 1u, true, true, &budget), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_instance_invoke(&instance, name("drops"), NULL, 0u, &value, 1u, &count, &trap),
            TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_call_create(&call, &instance, name("drops"), NULL, 0u), TURBOWASM_INVALID_ARGUMENT);
        check_true(resource.as.own == owned); check_null(arguments.impl); check_null(call.impl);
        check_equal(value.as.u32, 99u); check_equal(count, (size_t)99); check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(budget.used, used); check_equal(allocations.attempts, attempts);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resource), TURBOWASM_OK);
    }
    it("requests cancellation of every initial host root without freeing its carrier") {
        size_t used;
        void *first, *second;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK);
        check_equal(create(1u, "unit", NULL, 0u, false), TURBOWASM_OK);
        first = owners[0].impl; second = owners[1].impl; used = budget.used;
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_true(owners[0].impl == first); check_true(owners[1].impl == second); check_equal(budget.used, used);
        check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(turbowasm_component_host_task_view(&owners[1])->phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_destroy(&owners[1]), TURBOWASM_OK);
        check_null(impl->host_owners); check_equal(budget.used, (size_t)0);
    }
    it("delivers an already completed task result while admission remains closed") {
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OK); finish(0u);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_false(turbowasm_component_host_task_view(&owners[0])->cancellation_requested);
        shutdown_on_allocate = true;
        take(0u, 42u); check_false(shutdown_on_allocate);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
        check_null(impl->host_owners); check_equal(impl->host_activity, 0u);
    }
    it("requires real guest acknowledgement of cancellation at a noncancellable yield") {
        check_equal(create(0u, "yield", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_true(turbowasm_component_host_task_view(&owners[0])->cancellation_requested);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_RETURNED);
        take(0u, 43u); compiled(0u);
    }
    it("drives the existing callback to acknowledge shutdown cancellation") {
        check_equal(create(0u, "callback", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        finish(0u);
        check_equal(turbowasm_component_host_task_view(&owners[0])->phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
    }
    it("requests cancellation while an exclusive callback entry is suspended by fuel") {
        turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 1u};
        const turbowasm_component_task *view;
        check_equal(create(0u, "callback", NULL, 0u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_task_resume(&owners[0], &options), TURBOWASM_YIELDED);
        view = turbowasm_component_host_task_view(&owners[0]);
        check_true(impl->exec.task_domain.exclusive == view);
        check_null(impl->exec.task_domain.active); check_equal(view->phase, TURBOWASM_COMPONENT_TASK_STARTED);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_true(view->cancellation_requested);
        finish(0u); check_equal(view->phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
    }
    it("preserves a resolved task across fuel suspension and retains its later primary trap") {
        turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 1u};
        const turbowasm_component_task *view;
        unsigned turns;
        check_equal(create(0u, "trap-after-return", NULL, 0u, false), TURBOWASM_OK);
        view = turbowasm_component_host_task_view(&owners[0]);
        for (turns = 0u; turns < 64u && view->phase != TURBOWASM_COMPONENT_TASK_RETURNED; ++turns)
            check_equal(turbowasm_component_host_task_resume(&owners[0], &options), TURBOWASM_YIELDED);
        check_equal(view->phase, TURBOWASM_COMPONENT_TASK_RETURNED); check_equal(view->state, TURBOWASM_EXECUTION_YIELDED);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_false(view->cancellation_requested);
        check_equal(turbowasm_component_host_task_resume(&owners[0], NULL), TURBOWASM_TRAPPED); compiled(0u);
        check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_null(owners[0].impl); check_null(impl->host_owners);
    }
    it("allows an admitted synchronous call to resume and deliver while new calls are closed") {
        turbowasm_component_call call = {0};
        check_equal(turbowasm_component_call_create(&call, &instance, name("drops"), NULL, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&call, &output), TURBOWASM_OK);
        check_equal(output.kind, TURBOWASM_COMPONENT_HOST_U32); check_equal(output.as.u32, 0u);
        turbowasm_component_call_destroy(&call); check_null(call.impl); check_equal(impl->host_activity, 0u);
    }
    it("cancels a partial transfer without consuming its acknowledgement or unsent cells") {
        turbowasm_component_event event;
        turbowasm_component_host_transfer_state state;
        const turbowasm_component_value *values;
        uint32_t i, count;
        size_t used;
        transfer_pair(endpoint_type(impl, false));
        for (i = 0u; i < OWNER_COUNT; ++i)
            transfer_inputs[i] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 40u + i};
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            OWNER_COUNT, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        used = budget.used;
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(budget.used, used);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[1], &state), TURBOWASM_OK);
        check_false(state.terminal); check_equal(state.progress, 1u);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
        check_equal(event.payload, 18u);
        values = turbowasm_component_host_transfer_values(&transfers[1], &count);
        check_equal(count, 2u); check_equal(values[0].as.u32, 41u); check_equal(values[1].as.u32, 42u);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[1], &host_ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[2], &host_ends[0], 1u, 0u, &budget, NULL),
            TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_endpoint_submit(&host_ends[0], &write_buffer, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OK);
    }
    it("cancels an endpoint operation while keeping the caller buffer leased until event delivery") {
        turbowasm_component_event event;
        transfer_pair(endpoint_type(impl, false)); write_buffer.length = 1u; write_buffer.values = &payload;
        check_equal(turbowasm_component_host_endpoint_submit(&host_ends[0], &write_buffer, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_true(write_buffer.leased);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_host_endpoint_take(&host_ends[0], &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_false(write_buffer.leased);
    }
    it("allows retained canonical endpoint ownership to move after shutdown admission closes") {
        transfer_pair(endpoint_type(impl, false));
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        shutdown_on_allocate = true;
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[0], impl, &endpoint_value, &budget), TURBOWASM_OK);
        check_false(shutdown_on_allocate); check_equal((int)endpoint_value.kind, 0);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
    }
    it("rejects shutdown from owner admission allocators and restores activity after allocation failure") {
        size_t baseline = budget.used;
        shutdown_on_allocate = true;
        allocations.fail_at = allocations.attempts + 1u;
        check_equal(create(0u, "answer", NULL, 0u, false), TURBOWASM_OUT_OF_MEMORY);
        allocations.fail_at = 0u;
        check_false(shutdown_on_allocate); check_false(impl->admission_closed);
        check_equal(budget.used, baseline); check_equal(impl->host_activity, 0u); check_null(impl->host_owners);
        shutdown_on_allocate = true;
        transfer_pair(endpoint_type(impl, false)); check_false(shutdown_on_allocate);
        shutdown_on_allocate = true;
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_false(shutdown_on_allocate); check_false(impl->admission_closed); check_equal(impl->host_activity, 0u);
        check_equal(turbowasm_component_instance_request_shutdown_private(impl), TURBOWASM_OK);
    }
    it("moves scalar stream and future operations into retained transfers in either admission order") {
        unsigned future, read_first;
        for (future = 0u; future < 2u; ++future) for (read_first = 0u; read_first < 2u; ++read_first) {
            turbowasm_component_event event;
            turbowasm_component_host_transfer_state state;
            const turbowasm_component_value *values;
            uint32_t count = 99u;
            transfer_pair(endpoint_type(impl, future != 0u));
            transfer_inputs[0] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u};
            if (read_first) check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0],
                1u, 0u, &budget, NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1],
                transfer_inputs, 1u, &budget, NULL), TURBOWASM_OK);
            if (!read_first) check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0],
                1u, 0u, &budget, NULL), TURBOWASM_OK);
            check_null(host_ends[0].impl); check_null(host_ends[1].impl); check_equal((int)transfer_inputs[0].kind, 0);
            check_equal(impl->host_transfer_count, 2u);
            check_null(turbowasm_component_host_transfer_values(&transfers[0], &count)); check_equal(count, 99u);
            check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
            check_equal(event.payload, future ? 0u : 16u);
            check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
            check_true(state.terminal); check_true(state.readable); check_equal(state.progress, 1u);
            values = turbowasm_component_host_transfer_values(&transfers[0], &count);
            check_equal(count, 1u); check_not_null(values); check_equal(values[0].as.u32, 42u);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_INVALID_ARGUMENT);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[1], &host_ends[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OK);
            check_equal(impl->host_transfer_count, 0u);
            check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
            check_equal(budget.used, (size_t)0);
        }
    }
    it("delivers a unit future without allocating payload cells") {
        turbowasm_component_event event;
        uint32_t count = 99u;
        transfer_pair(payload_endpoint_type(true, false, TURBOWASM_COMPONENT_TYPE_UNDEFINED));
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], NULL, 1u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(event.payload, 0u);
        check_null(turbowasm_component_host_transfer_values(&transfers[0], &count)); check_equal(count, 1u);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
    }
    it("holds partial stream progress and returns only the unsent tail after cancellation acknowledgement") {
        turbowasm_component_event event;
        turbowasm_component_host_transfer_state state;
        const turbowasm_component_value *values;
        uint32_t i, count;
        transfer_pair(endpoint_type(impl, false));
        for (i = 0u; i < OWNER_COUNT; ++i)
            transfer_inputs[i] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 40u + i};
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            OWNER_COUNT, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[1], &state), TURBOWASM_OK);
        check_false(state.terminal); check_equal(state.progress, 1u);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[1], &host_ends[1]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
        check_equal(event.payload, 18u);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[1], &state), TURBOWASM_OK);
        check_equal(state.event.payload, 18u);
        values = turbowasm_component_host_transfer_values(&transfers[1], &count);
        check_equal(count, 2u); check_equal(values[0].as.u32, 41u); check_equal(values[1].as.u32, 42u);
    }
    it("preserves zero length stream admission and releases its storage on acknowledged cancellation") {
        turbowasm_component_event event;
        turbowasm_component_host_transfer_state state;
        transfer_pair(endpoint_type(impl, false));
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 0u, 0u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_equal(state.length, 0u); check_equal(state.progress, 0u); check_true(state.terminal);
    }
    it("retains the entire composite string own and nested future payload through terminal delivery") {
        turbowasm_component_event event;
        const turbowasm_component_value *values;
        uint32_t count;
        transfer_pair(payload_endpoint_type(false, true, TURBOWASM_COMPONENT_TYPE_TUPLE)); transfer_compound(42);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, compound_bytes(), &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
        values = turbowasm_component_host_transfer_values(&transfers[0], &count);
        check_equal(count, 1u); check_equal(values[0].kind, TURBOWASM_COMPONENT_TYPE_TUPLE);
        check_equal(values[0].as.tuple.items[0].as.string.data, "abc", 3u);
        check_equal(values[0].as.tuple.items[1].as.resource_rep.as.i32, 42);
        check_not_null(turbowasm_component_endpoint_value_get(&values[0].as.tuple.items[2]));
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OK); check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK); check_equal(drops(), 1u);
        check_true(pair_ends[0][0]->closed);
    }
    it("reports receive quota failure on both ends without moving a partial composite batch") {
        turbowasm_component_event event = {.payload = 99u};
        turbowasm_component_host_transfer_state state;
        const turbowasm_component_value *values;
        uint32_t count;
        transfer_pair(payload_endpoint_type(false, true, TURBOWASM_COMPONENT_TYPE_TUPLE)); transfer_compound(-1);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, compound_bytes() - 1u,
            &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OUT_OF_MEMORY);
        check_equal(event.payload, 99u);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OUT_OF_MEMORY);
        check_equal(event.payload, 99u);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_true(state.terminal); check_equal(state.progress, 0u); check_equal(state.status, TURBOWASM_OUT_OF_MEMORY);
        values = turbowasm_component_host_transfer_values(&transfers[1], &count);
        check_equal(count, 1u); check_equal(values[0].as.tuple.items[1].as.resource_rep.as.i32, -1);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OUT_OF_MEMORY);
        check_equal(drops(), 1u); check_true(pair_ends[0][0]->closed);
    }
    it("rejects transfer count exhaustion before moving either input owner and recovers after cleanup") {
        turbowasm_component_event event;
        void *end;
        size_t used, attempts;
        transfer_pair(endpoint_type(impl, false)); impl->host_transfer_limit = 1u;
        transfer_inputs[0] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u};
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        end = host_ends[1].impl; used = budget.used; attempts = allocations.attempts;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_true(host_ends[1].impl == end); check_equal(transfer_inputs[0].as.u32, 42u);
        check_equal(budget.used, used); check_equal(allocations.attempts, attempts);
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_OK);
    }
    it("rolls back every transfer allocation and restores its endpoint values references and byte charge") {
        unsigned read;
        transfer_pair(endpoint_type(impl, false));
        for (read = 0u; read < 2u; ++read) {
            size_t failure, live = allocations.live, used = budget.used;
            uint32_t refs = impl->ref_count;
            void *end = host_ends[read].impl;
            bool complete = false;
            transfer_inputs[0] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_U32, .as.u32 = 42u};
            for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
                turbowasm_component_event event;
                allocations.attempts = 0u; allocations.fail_at = failure;
                turbowasm_status status = read == 0u
                    ? turbowasm_component_host_transfer_read(&transfers[0], &host_ends[read], 1u, 0u, &budget, NULL)
                    : turbowasm_component_host_transfer_write_move(&transfers[0], &host_ends[read], transfer_inputs, 1u, &budget, NULL);
                allocations.fail_at = 0u;
                if (status == TURBOWASM_OK) {
                    check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_OK);
                    check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
                    check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[read]), TURBOWASM_OK);
                    check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK); complete = true;
                } else {
                    check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(transfer_inputs[0].as.u32, 42u);
                }
                check_true(host_ends[read].impl == end); check_null(transfers[0].impl);
                check_equal(impl->ref_count, refs); check_equal(impl->host_transfer_count, 0u);
                check_equal(allocations.live, live); check_equal(budget.used, used);
                if (complete) break;
            }
            check_true(complete); check_equal(failure, (size_t)3);
        }
    }
    it("guards transfer and endpoint admission against allocator reentry and retains closed public handles") {
        turbowasm_component_event event;
        transfer_pair(endpoint_type(impl, false));
        reenter_endpoint_on_allocate = reenter_transfer_on_allocate = close_on_allocate = true;
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_false(reenter_endpoint_on_allocate); check_false(reenter_transfer_on_allocate); check_false(close_on_allocate);
        check_null(instance.impl); check_null(component.impl);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(event.payload, 1u);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(budget.used, (size_t)0);
    }
    it("charges exact transfer bytes before allocation and preserves inputs one byte short") {
        turbowasm_component_event event;
        size_t used, charge, extra, attempts;
        void *end;
        uint32_t refs;
        transfer_pair(endpoint_type(impl, false)); used = budget.used; end = host_ends[0].impl; refs = impl->ref_count;
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        charge = budget.used - used;
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        /* Model another owner reserving the rest of this shared finite budget. */
        extra = budget.limit - budget.used - charge + 1u; budget.used += extra; attempts = allocations.attempts;
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(allocations.attempts, attempts); check_equal(impl->ref_count, refs);
        check_true(host_ends[0].impl == end); check_equal(budget.used, used + extra);
        --budget.used; --extra;
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        check_equal(budget.used, budget.limit);
        check_equal(turbowasm_component_host_transfer_cancel(&transfers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        budget.used -= extra; check_equal(budget.used, used);
    }
    it("keeps a foreign creation graph alive after returning and closing the endpoint and both public instances") {
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        turbowasm_component_instance_public_impl *original = impl, *receiving;
        turbowasm_component_event event;
        turbowasm_component_host_transfer_state state;
        transfer_pair(endpoint_type(impl, true));
        check_equal(turbowasm_component_instance_create_async_private(&destination, &component, &limits), TURBOWASM_OK);
        receiving = turbowasm_component_instance_public_impl_get(&destination);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], receiving, &endpoint_value, &budget), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[2], 1u, 0u, &budget, NULL), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
        check_equal(event.payload, 1u);
        check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[0], &host_ends[2]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[2]), TURBOWASM_OK);
        check_equal(original->ref_count, 1u); check_true(original->exec.initialized);
        turbowasm_component_instance_destroy(&destination);
        check_equal(receiving->ref_count, 1u); check_true(receiving->exec.initialized);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_true(state.terminal); check_equal(state.event.payload, 1u);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
        check_equal(allocations.live, (size_t)0); check_equal(budget.used, (size_t)0);
    }
    it("validates direction type length capacity and retained provenance before moving an endpoint") {
        size_t used, attempts;
        void *reader, *writer;
        uint32_t refs;
        transfer_pair(endpoint_type(impl, true)); used = budget.used; attempts = allocations.attempts;
        reader = host_ends[0].impl; writer = host_ends[1].impl; refs = impl->ref_count;
        transfer_inputs[0] = (turbowasm_component_value){.kind = TURBOWASM_COMPONENT_TYPE_S32, .as.s32 = 42};
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_TYPE_MISMATCH);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[1], 1u, 0u, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 0u, 0u, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 2u, 0u, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, SIZE_MAX, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], UINT32_MAX, 0u, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_true(host_ends[0].impl == reader); check_true(host_ends[1].impl == writer);
        check_equal(allocations.attempts, attempts); check_equal(budget.used, used); check_equal(impl->ref_count, refs);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
        transfer_pair(payload_endpoint_type(false, true, TURBOWASM_COMPONENT_TYPE_TUPLE)); transfer_compound(42);
        turbowasm_component_value *resource_value = &transfer_inputs[0].as.tuple.items[1];
        resource_value->kind = TURBOWASM_COMPONENT_TYPE_BORROW; used = budget.used; attempts = allocations.attempts;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_TYPE_MISMATCH);
        resource_value->kind = TURBOWASM_COMPONENT_TYPE_OWN;
        void *release_context = resource_value->release_context;
        resource_value->release_context = NULL;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[0], &host_ends[1], transfer_inputs,
            1u, &budget, NULL), TURBOWASM_INVALID_ARGUMENT);
        resource_value->release_context = release_context;
        check_equal(allocations.attempts, attempts); check_equal(budget.used, used);
        check_equal(transfer_inputs[0].kind, TURBOWASM_COMPONENT_TYPE_TUPLE); check_equal(drops(), 0u);
    }
    it("enforces payload capacity across rendezvous batches while retaining earlier committed elements") {
        turbowasm_component_event event;
        turbowasm_component_host_transfer_state state;
        const turbowasm_component_value *values;
        const char *texts[] = {"abc", "de", "f"};
        uint32_t i, count;
        transfer_pair(payload_endpoint_type(false, true, TURBOWASM_COMPONENT_TYPE_STRING));
        check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 3u, 5u, &budget, NULL), TURBOWASM_OK);
        for (i = 0u; i < 3u; ++i) {
            transfer_text(&transfer_inputs[0], texts[i]);
            check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], transfer_inputs,
                1u, &budget, NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event),
                i == 2u ? TURBOWASM_OUT_OF_MEMORY : TURBOWASM_OK);
            if (i < 2u) {
                check_equal(turbowasm_component_host_transfer_take_endpoint(&transfers[1], &host_ends[1]), TURBOWASM_OK);
                check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OK);
            }
        }
        check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_host_transfer_state_get(&transfers[0], &state), TURBOWASM_OK);
        check_equal(state.progress, 2u); check_true(state.terminal);
        values = turbowasm_component_host_transfer_values(&transfers[0], &count);
        check_equal(count, 2u); check_equal(values[0].as.string.data, "abc", 3u); check_equal(values[1].as.string.data, "de", 2u);
        values = turbowasm_component_host_transfer_values(&transfers[1], &count);
        check_equal(count, 1u); check_equal(values[0].as.string.data, "f", 1u);
    }
    it("receives real guest strings into owned transfers across memory32 and memory64 fuel suspension") {
        unsigned wide;
        for (wide = 0u; wide < 2u; ++wide) {
            turbowasm_component_host_value arguments[2];
            uint8_t text[] = "abc";
            turbowasm_component_event event;
            turbowasm_runtime_scope scope;
            turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 1u};
            const turbowasm_component_value *values;
            uint64_t handles;
            uint32_t count;
            size_t result_count;
            check_equal(create(0u, "pair-text", NULL, 0u, false), TURBOWASM_OK); finish(0u);
            check_equal(turbowasm_component_host_task_take_result(&owners[0], &output, &result_count), TURBOWASM_OK);
            check_equal(result_count, (size_t)1); check_equal(output.kind, TURBOWASM_COMPONENT_HOST_U64);
            handles = output.as.u64;
            check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            pair_ends[0][0] = turbowasm_component_endpoint_get(&impl->exec.resource_table, (uint32_t)handles,
                TURBOWASM_COMPONENT_HANDLE_STREAM_READ);
            pair_ends[0][1] = turbowasm_component_endpoint_get(&impl->exec.resource_table, (uint32_t)(handles >> 32),
                TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE);
            check_not_null(pair_ends[0][0]); check_not_null(pair_ends[0][1]);
            check_equal(turbowasm_component_endpoint_detach_readable(pair_ends[0][0]), TURBOWASM_OK);
            scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
            turbowasm_status status = turbowasm_component_endpoint_into_value(pair_ends[0][0], &endpoint_value);
            turbowasm_runtime_scope_leave(scope); check_equal(status, TURBOWASM_OK);
            check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[0], impl, &endpoint_value, &budget), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_read(&transfers[0], &host_ends[0], 1u, 3u, &budget, NULL), TURBOWASM_OK);
            copy_reentry_end = pair_ends[0][0]; reenter_transfer_on_allocate = true;
            arguments[0] = (turbowasm_component_host_value){.kind = TURBOWASM_COMPONENT_HOST_U32,
                .as.u32 = (uint32_t)(handles >> 32)};
            arguments[1] = (turbowasm_component_host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING,
                .as.string = {text, 3u}};
            check_equal(create(0u, wide ? "write-text64" : "write-text32", arguments, 2u, false), TURBOWASM_OK);
            check_equal(turbowasm_component_host_task_resume(&owners[0], &options), TURBOWASM_YIELDED);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_YIELDED);
            finish(0u); take(0u, 16u);
            check_false(reenter_transfer_on_allocate); copy_reentry_end = NULL;
            check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            check_equal(turbowasm_component_host_transfer_poll(&transfers[0], &event), TURBOWASM_OK);
            values = turbowasm_component_host_transfer_values(&transfers[0], &count);
            check_equal(count, 1u); check_equal(values[0].kind, TURBOWASM_COMPONENT_TYPE_STRING);
            check_equal(values[0].as.string.size, (size_t)3); check_equal(values[0].as.string.data, "abc", 3u);
            check_equal(turbowasm_component_host_transfer_destroy(&transfers[0]), TURBOWASM_OK);
            check_equal(create(0u, "drop-text-write", arguments, 1u, false), TURBOWASM_OK); finish(0u);
            check_equal(turbowasm_component_host_task_take_result(&owners[0], NULL, &result_count), TURBOWASM_OK);
            check_equal(result_count, (size_t)0);
            check_equal(turbowasm_component_host_task_destroy(&owners[0]), TURBOWASM_OK);
            check_equal(impl->exec.resource_table.live_count, 0u);
            pair_ends[0][0] = pair_ends[0][1] = NULL;
        }
    }
    it("creates finite host stream and future owners with balanced byte and instance reservations") {
        unsigned future;
        uint32_t refs = impl->ref_count;
        for (future = 0u; future < 2u; ++future) {
            size_t charge;
            check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
                endpoint_type(impl, future != 0u), &budget), TURBOWASM_OK);
            charge = budget.used; check_greater(charge, (size_t)0); check_equal(impl->ref_count, refs + 3u);
            check_true(turbowasm_component_host_endpoint_view(&host_ends[0])->readable);
            check_false(turbowasm_component_host_endpoint_view(&host_ends[1])->readable);
            check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_OK);
            check_equal(budget.used, charge / 2u); check_equal(impl->ref_count, refs + 2u);
            check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
            check_equal(budget.used, (size_t)0); check_equal(impl->ref_count, refs);
            check_equal(impl->exec.task_domain.pair_count, 0u);
        }
    }
    it("rolls back every host endpoint pair allocation without publishing a half owner") {
        size_t failure, live = allocations.live;
        uint32_t refs = impl->ref_count;
        bool complete = false;
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
                endpoint_type(impl, false), &budget); allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) { complete = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(host_ends[0].impl); check_null(host_ends[1].impl);
            check_equal(impl->ref_count, refs); check_equal(allocations.live, live);
            check_equal(budget.used, (size_t)0); check_equal(impl->exec.task_domain.pair_count, 0u);
        }
        check_true(complete); check_greater(failure, (size_t)3);
    }
    it("preflights byte quota and retries canonical endpoint promotion after allocation failure") {
        size_t charge, attempts, used;
        turbowasm_component_host_budget short_budget;
        void *value_owner;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        charge = budget.used;
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
        budget.limit = charge - 1u; attempts = allocations.attempts;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OUT_OF_MEMORY);
        check_equal(allocations.attempts, attempts); check_equal(budget.used, (size_t)0);
        budget.limit = charge;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        check_null(host_ends[0].impl); used = budget.used; value_owner = endpoint_value.as.endpoint.owner;
        short_budget = (turbowasm_component_host_budget){charge / 2u - 1u, 0u};
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], impl, &endpoint_value,
            &short_budget), TURBOWASM_OUT_OF_MEMORY);
        check_equal(short_budget.used, (size_t)0); check_true(endpoint_value.as.endpoint.owner == value_owner);
        allocations.attempts = 0u; allocations.fail_at = 1u;
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], impl, &endpoint_value,
            &budget), TURBOWASM_OUT_OF_MEMORY); allocations.fail_at = 0u;
        check_equal(budget.used, used); check_true(endpoint_value.as.endpoint.owner == value_owner);
        check_null(host_ends[2].impl);
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], impl, &endpoint_value,
            &budget), TURBOWASM_OK);
        check_equal((int)endpoint_value.kind, 0); check_equal(budget.used, budget.limit);
    }
    it("preserves readable ownership when canonical value allocation fails and rejects a writable move") {
        void *owner;
        size_t used;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        owner = host_ends[0].impl; used = budget.used;
        allocations.attempts = 0u; allocations.fail_at = 1u;
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OUT_OF_MEMORY);
        allocations.fail_at = 0u;
        check_true(host_ends[0].impl == owner); check_equal(budget.used, used); check_equal((int)endpoint_value.kind, 0);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[1], &endpoint_value), TURBOWASM_INVALID_ARGUMENT);
    }
    it("rejects promotion while a canonical readable end is reserved for publication") {
        uint32_t handle;
        size_t attempts, used;
        void *value_owner;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        endpoint_codec.table = &impl->exec.resource_table;
        check_equal(turbowasm_component_endpoint_codec_lower(&endpoint_codec, &impl->exec.binary->type_graph,
            turbowasm_component_type_ref_indexed(endpoint_type(impl, false)), &endpoint_value, &handle), TURBOWASM_OK);
        attempts = allocations.attempts; used = budget.used; value_owner = endpoint_value.as.endpoint.owner;
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], impl, &endpoint_value,
            &budget), TURBOWASM_TYPE_MISMATCH);
        check_equal(allocations.attempts, attempts); check_equal(budget.used, used);
        check_true(endpoint_value.as.endpoint.owner == value_owner);
        check_equal(turbowasm_component_endpoint_codec_rollback(&endpoint_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], impl, &endpoint_value,
            &budget), TURBOWASM_OK);
    }
    it("rejects caller-owned canonical endpoint storage that has no creation-instance keepalive") {
        turbowasm_runtime_scope scope;
        void *value_owner;
        size_t attempts;
        check_equal(turbowasm_component_endpoint_pair_open(&impl->exec.binary->type_graph, endpoint_type(impl, false),
            NULL, NULL, &loose_ends[0], &loose_ends[1]), TURBOWASM_OK);
        scope = turbowasm_runtime_scope_enter(&impl->exec.binary->config);
        check_equal(turbowasm_component_endpoint_into_value(&loose_ends[0], &endpoint_value), TURBOWASM_OK);
        turbowasm_runtime_scope_leave(scope);
        value_owner = endpoint_value.as.endpoint.owner; attempts = allocations.attempts;
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[0], impl, &endpoint_value,
            &budget), TURBOWASM_TYPE_MISMATCH);
        check_true(endpoint_value.as.endpoint.owner == value_owner); check_equal(allocations.attempts, attempts);
        check_equal(budget.used, (size_t)0); check_null(host_ends[0].impl);
    }
    it("guards endpoint ownership during allocator reentry on a canonical move") {
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        reenter_endpoint_on_allocate = true;
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        check_false(reenter_endpoint_on_allocate); check_null(host_ends[0].impl);
    }
    it("keeps busy owners intact until cancellation acknowledgement delivers the borrowed buffer") {
        void *owner;
        size_t used;
        turbowasm_component_event event;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        payload.kind = TURBOWASM_COMPONENT_TYPE_U32; payload.as.u32 = 42u;
        write_buffer.values = &payload; write_buffer.length = 1u;
        check_equal(turbowasm_component_host_endpoint_submit(&host_ends[1], &write_buffer, NULL), TURBOWASM_OK);
        owner = host_ends[1].impl; used = budget.used;
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_TRAPPED);
        check_true(host_ends[1].impl == owner); check_equal(budget.used, used); check_true(write_buffer.leased);
        check_equal(turbowasm_component_host_endpoint_cancel(&host_ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_take(&host_ends[1], &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_false(write_buffer.leased);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
    }
    it("retains both creation and receiving instances when a host owner promotes a foreign canonical end") {
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        turbowasm_component_instance_public_impl *target;
        check_equal(turbowasm_component_instance_create_async_private(&destination, &component, &limits), TURBOWASM_OK);
        target = turbowasm_component_instance_public_impl_get(&destination);
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, true), &budget), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_from_value(&host_ends[2], target, &endpoint_value,
            &budget), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        turbowasm_component_instance_destroy(&destination);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_destroy(&host_ends[2]), TURBOWASM_OK);
        impl = NULL; check_equal(budget.used, (size_t)0); check_equal(allocations.live, (size_t)0);
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
    it("keeps the creation type graph and host writer alive through foreign publication and compiled guest reads") {
        turbowasm_component_exec_async_limits limits = {2u, 16u};
        turbowasm_component_instance_public_impl *original = impl, *target;
        turbowasm_component_host_value handle = {.kind = TURBOWASM_COMPONENT_HOST_U32};
        turbowasm_component_event event;
        uint32_t guest_handle = 0u;
        size_t count;
        check_equal(turbowasm_component_instance_create_async_private(&destination, &component, &limits), TURBOWASM_OK);
        target = turbowasm_component_instance_public_impl_get(&destination);
        impl = target; attach(); impl = original;
        check_equal(turbowasm_component_host_endpoint_pair_create(&host_ends[0], &host_ends[1], impl,
            endpoint_type(impl, false), &budget), TURBOWASM_OK);
        payload.kind = TURBOWASM_COMPONENT_TYPE_U32; payload.as.u32 = 42u;
        check_equal(turbowasm_component_host_transfer_write_move(&transfers[1], &host_ends[1], &payload,
            1u, &budget, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_host_endpoint_into_value(&host_ends[0], &endpoint_value), TURBOWASM_OK);
        endpoint_codec.table = &target->exec.resource_table;
        check_equal(turbowasm_component_endpoint_codec_lower(&endpoint_codec, &target->exec.binary->type_graph,
            turbowasm_component_type_ref_indexed(endpoint_type(target, false)), &endpoint_value, &guest_handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_commit(&endpoint_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&endpoint_value), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        check_equal(original->ref_count, 4u);
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
        check_equal(turbowasm_component_host_transfer_poll(&transfers[1], &event), TURBOWASM_OK);
        check_equal(event.payload, 16u);
        check_equal(turbowasm_component_host_transfer_destroy(&transfers[1]), TURBOWASM_OK);
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
