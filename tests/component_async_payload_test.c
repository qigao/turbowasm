#include "component_exec.h"
#include "component_endpoint_builtin.h"
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
static turbowasm_component_task tasks[3];
static turbowasm_component_endpoint ends[2][2], nested[2];
static turbowasm_component_buffer buffers[2];
static turbowasm_component_value inputs[2], outputs[2];
static turbowasm_value arguments[3][3];
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static const turbowasm_component_task_binding *binding(const char *name) {
    const turbowasm_component_task_binding *result = NULL;
    check_equal(turbowasm_component_exec_async_export(&exec, (const uint8_t *)name, (uint32_t)strlen(name), &result), TURBOWASM_OK);
    return result;
}
static turbowasm_status prepare_arguments(void *context, turbowasm_component_task *task,
    turbowasm_value *out, size_t capacity, size_t *count) {
    (void)task;
    if (capacity < 3) return TURBOWASM_TYPE_MISMATCH;
    memcpy(out, context, 3 * sizeof(*out)); *count = 3; return TURBOWASM_OK;
}
static void create(unsigned task, const char *name, uint32_t handle, uint32_t address, uint32_t count) {
    turbowasm_component_task_binding copy = *binding(name);
    arguments[task][0].kind = arguments[task][1].kind = arguments[task][2].kind = TURBOWASM_VALUE_I32;
    arguments[task][0].as.i32 = (int32_t)handle; arguments[task][1].as.i32 = (int32_t)address;
    arguments[task][2].as.i32 = (int32_t)count;
    copy.prepare = prepare_arguments; copy.prepare_context = arguments[task];
    check_equal(turbowasm_component_task_create(&tasks[task], &exec.task_domain, &copy), TURBOWASM_OK);
}
static void result(unsigned index, uint32_t expected) {
    turbowasm_component_value value = {0};
    check_equal(turbowasm_component_task_take_result(&tasks[index], &value), TURBOWASM_OK);
    check_equal(value.as.u32, expected);
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)tasks[index].binding.instance->impl)->jit_functions[tasks[index].binding.function_index].state,
        TURBOWASM_JIT_COMPILED);
#endif
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
}
static turbowasm_component_type_id payload_type(bool future, bool mixed) {
    uint32_t i, expected = TURBOWASM_COMPONENT_VALUE_RESOURCES;
    if (mixed) expected |= TURBOWASM_COMPONENT_VALUE_ENDPOINTS | TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY;
    for (i = 0; i < exec.binary->type_graph.count; ++i) {
        const turbowasm_component_type *type = &exec.binary->type_graph.types[i]; uint32_t features;
        if (type->kind == (future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM) &&
            type->as.async_value.has_payload && turbowasm_component_transfer_type_features(&exec.binary->type_graph,
                type->as.async_value.payload, &features) && features == expected) return i;
    }
    return UINT32_MAX;
}
static void pair(unsigned index, bool destination, bool future, bool mixed) {
    turbowasm_component_type_id type = payload_type(future, mixed); check_not_equal(type, UINT32_MAX);
    check_equal(turbowasm_component_endpoint_pair_open(&exec.binary->type_graph, type,
        destination ? &exec.resource_table : NULL, destination ? NULL : &exec.resource_table,
        &ends[index][0], &ends[index][1]), TURBOWASM_OK);
}
static void buffer(unsigned index, turbowasm_component_value *values, uint32_t count) {
    buffers[index].kind = TURBOWASM_COMPONENT_BUFFER_HOST; buffers[index].values = values; buffers[index].length = count;
}
static void take(turbowasm_component_endpoint *end, turbowasm_status expected) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_endpoint_take(end, &event), expected);
    check_null(end->operation);
}
static void release_end(turbowasm_component_endpoint *end) {
    if (!end->initialized || end->closed) return;
    if (end->operation != NULL) {
        if (end->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
            check_equal(turbowasm_component_endpoint_cancel(end), TURBOWASM_OK);
        take(end, end->failure);
    }
    check_equal(turbowasm_component_endpoint_close(end), TURBOWASM_OK);
}
static void close_pair(unsigned index) {
    release_end(&ends[index][0]); release_end(&ends[index][1]);
    memset(ends[index], 0, sizeof(ends[index])); memset(&buffers[index], 0, sizeof(buffers[index]));
}
static turbowasm_status release_rep(void *context, uint64_t identity, turbowasm_value rep) {
    return turbowasm_component_exec_resource_release(context, identity, rep);
}
static void clear_guest_values(void) {
    uint32_t i;
    for (i = 0; i < exec.resource_table.capacity; ++i) {
        uint32_t handle; turbowasm_component_handle_kind kind; void *object;
        if (!turbowasm_component_handle_at(&exec.resource_table, i, &handle, &kind, &object)) continue;
        check_not_equal(kind, TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION);
        if (kind == TURBOWASM_COMPONENT_HANDLE_RESOURCE)
            check_equal(turbowasm_component_resource_drop(&exec.resource_table, handle,
                exec.resource_table.entries[i].resource_identity, release_rep, &exec), TURBOWASM_OK);
        else {
            turbowasm_component_endpoint *end = turbowasm_component_endpoint_get(&exec.resource_table, handle, kind);
            check_not_null(end); release_end(end);
        }
    }
    turbowasm_component_endpoint_domain_collect(&exec.task_domain);
}
static uint32_t destructions(void) {
    turbowasm_component_value value = {0};
    check_equal(turbowasm_component_canonical_lift_value(&exec.binary->type_graph,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32), &binding("payload-read")->memory, 400, &value), TURBOWASM_OK);
    return value.as.u32;
}
static void own(turbowasm_component_value *out) {
    check_equal(turbowasm_component_task_create(&tasks[2], &exec.task_domain, binding("resource-result")), TURBOWASM_OK);
    check_equal(turbowasm_component_task_resume(&tasks[2], NULL), TURBOWASM_OK);
    check_equal(turbowasm_component_task_take_result(&tasks[2], out), TURBOWASM_OK);
    check_equal(turbowasm_component_task_destroy(&tasks[2]), TURBOWASM_OK);
}
static void mixed_input(void) {
    turbowasm_component_value resource = {0}; turbowasm_component_type_id type = UINT32_MAX; uint32_t i;
    own(&resource);
    for (i = 0; i < exec.binary->type_graph.count; ++i) {
        const turbowasm_component_type *candidate = &exec.binary->type_graph.types[i];
        if (candidate->kind == TURBOWASM_COMPONENT_TYPE_STREAM && candidate->as.async_value.has_payload &&
            candidate->as.async_value.payload.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE &&
            candidate->as.async_value.payload.as.inline_type == TURBOWASM_COMPONENT_TYPE_U32) { type = i; break; }
    }
    check_not_equal(type, UINT32_MAX);
    check_equal(turbowasm_component_endpoint_pair_open(&exec.binary->type_graph, type, NULL, NULL, &nested[0], &nested[1]), TURBOWASM_OK);
    inputs[0].kind = TURBOWASM_COMPONENT_TYPE_TUPLE;
    inputs[0].as.tuple.items = turbowasm_rt_calloc(3, sizeof(*inputs[0].as.tuple.items));
    check_not_null(inputs[0].as.tuple.items); inputs[0].as.tuple.count = 3;
    inputs[0].as.tuple.items[0] = resource;
    check_equal(turbowasm_component_endpoint_into_value(&nested[0], &inputs[0].as.tuple.items[1]), TURBOWASM_OK);
    inputs[0].as.tuple.items[2].kind = TURBOWASM_COMPONENT_TYPE_STRING;
    inputs[0].as.tuple.items[2].as.string.data = turbowasm_rt_malloc(5);
    check_not_null(inputs[0].as.tuple.items[2].as.string.data);
    memcpy(inputs[0].as.tuple.items[2].as.string.data, "hello", 5); inputs[0].as.tuple.items[2].as.string.size = 5;
}
static turbowasm_status drive_host_write(void *context, turbowasm_component_task *task,
    turbowasm_value *out, size_t capacity, size_t *count) {
    turbowasm_component_event event; turbowasm_status status;
    (void)context; (void)task; (void)out; (void)capacity;
    status = turbowasm_component_endpoint_submit(&ends[0][1], &buffers[0]);
    if (status == TURBOWASM_OK) status = turbowasm_component_endpoint_take(&ends[0][1], &event);
    if (status == TURBOWASM_OK) *count = 0;
    return status;
}
static void suspended_mixed_copy(void) {
    turbowasm_component_task_binding driver = *binding("payload-driver");
    turbowasm_execution_options options = {0}; unsigned turns = 0;
    pair(0, true, false, true); mixed_input(); buffer(0, inputs, 1);
    create(0, "payload-mixed-read", ends[0][0].waitable.handle, 512, 1);
    check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
    driver.prepare = drive_host_write;
    check_equal(turbowasm_component_task_create(&tasks[2], &exec.task_domain, &driver), TURBOWASM_OK);
    options.has_fuel_limit = true; options.fuel = 4;
    do {
        check_equal(turbowasm_component_task_resume(&tasks[2], &options), TURBOWASM_YIELDED);
        check_less(++turns, 1000u);
    } while (exec.task_domain.auxiliary == NULL);
    check_equal(exec.async_buffer_owners, 1u); check_true(ends[0][0].waitable.delivering);
    check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_TRAPPED);
}

spec("instantiated ownership-bearing endpoint payloads") {
    before_each() {
        turbowasm_component_exec_async_limits limits = {5,16};
        live = 0; allowance = SIZE_MAX;
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binary, component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        { uint32_t i; for (i = 0; i < exec.core_instance_count; ++i) {
            turbowasm_jit_backend backend = {0}; if (exec.core_instances[i].impl == NULL) continue;
            check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
            check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl, &backend, 1), TURBOWASM_OK);
        } }
#endif
    }
    after_each() {
        int i; allowance = SIZE_MAX;
        for (i = 2; i >= 0; --i) check_equal(turbowasm_component_task_destroy(&tasks[i]), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_async_abort(&exec, TURBOWASM_INTERRUPTED), TURBOWASM_OK);
        close_pair(0); close_pair(1);
        for (i = 0; i < 2; ++i) {
            check_equal(turbowasm_component_value_destroy(&inputs[i]), TURBOWASM_OK);
            check_equal(turbowasm_component_value_destroy(&outputs[i]), TURBOWASM_OK);
        }
        clear_guest_values(); release_end(&nested[0]); release_end(&nested[1]); memset(nested, 0, sizeof(nested));
        check_equal(exec.async_buffer_owners, 0u); check_equal(exec.async_resource_owners, 0u);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binary); turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("lifts a guest resource batch into host ownership") {
        pair(0, false, false, false); buffer(0, outputs, 2);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][0], &buffers[0]), TURBOWASM_OK);
        create(0, "payload-write", ends[0][1].waitable.handle, 512, 2);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 2);
        check_equal(exec.async_buffer_owners, 0u); check_equal(exec.async_resource_owners, 2u);
        take(&ends[0][0], TURBOWASM_OK); check_false(buffers[0].leased);
        check_equal(outputs[0].as.resource_rep.as.i32, 42); check_equal(outputs[1].as.resource_rep.as.i32, 42);
        check_equal(destructions(), 0u);
    }
    it("commits host resource batches into guest handles before guest destruction") {
        pair(0, true, false, false); own(&inputs[0]); own(&inputs[1]); buffer(0, inputs, 2);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][1], &buffers[0]), TURBOWASM_OK);
        create(0, "payload-read", ends[0][0].waitable.handle, 512, 2);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 2);
        take(&ends[0][1], TURBOWASM_OK); check_equal(inputs[0].kind, 0); check_equal(inputs[1].kind, 0);
        check_equal(exec.async_resource_owners, 0u); check_equal(exec.async_buffer_owners, 0u); check_equal(destructions(), 2u);
    }
    it("lifts resource, endpoint and string payloads together from memory64") {
        pair(0, false, false, true); buffer(0, outputs, 1);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][0], &buffers[0]), TURBOWASM_OK);
        create(0, "payload-mixed-write", ends[0][1].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 1);
        take(&ends[0][0], TURBOWASM_OK); check_equal(outputs[0].kind, TURBOWASM_COMPONENT_TYPE_TUPLE);
        check_equal(outputs[0].as.tuple.items[0].as.resource_rep.as.i32, 42);
        check_equal(outputs[0].as.tuple.items[1].kind, TURBOWASM_COMPONENT_TYPE_STREAM);
        check_equal(outputs[0].as.tuple.items[2].as.string.data, "hello", 5);
        check_equal(exec.async_buffer_owners, 0u);
    }
    it("retains mixed read transactions while another task drives suspended realloc") {
        suspended_mixed_copy();
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_resume(&tasks[2], NULL), TURBOWASM_OK); result(2, 52);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 1);
        check_equal(exec.async_buffer_owners, 0u); check_equal(exec.async_resource_owners, 0u);
        check_equal(inputs[0].kind, 0); check_true(nested[0].closed); check_equal(destructions(), 1u);
    }
    it("rolls back suspended mixed payloads before releasing either endpoint buffer") {
        suspended_mixed_copy();
        check_equal(turbowasm_component_task_destroy(&tasks[2]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_INTERRUPTED);
        check_equal(exec.async_buffer_owners, 0u); check_false(buffers[0].leased);
        check_equal(inputs[0].kind, TURBOWASM_COMPONENT_TYPE_TUPLE); check_equal(exec.async_resource_owners, 1u);
        check_null(nested[0].lower_scope); check_null(nested[0].waitable.table); check_equal(destructions(), 0u);
    }
    it("keeps independent contexts for concurrent operations on one async read builtin") {
        void *second;
        pair(0, true, false, false); pair(1, true, false, false);
        create(0, "payload-read-async", ends[0][0].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, UINT32_MAX);
        create(1, "payload-read-async", ends[1][0].waitable.handle, 528, 1);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK); result(1, UINT32_MAX);
        check_equal(exec.async_buffer_owners, 2u); second = ends[1][0].guest_buffer.guest.context;
        check_true(second != ends[0][0].guest_buffer.guest.context);
        own(&inputs[0]); own(&inputs[1]); buffer(0, &inputs[0], 1); buffer(1, &inputs[1], 1);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][1], &buffers[0]), TURBOWASM_OK);
        take(&ends[0][1], TURBOWASM_OK); take(&ends[0][0], TURBOWASM_OK);
        check_equal(exec.async_buffer_owners, 1u); check_true(ends[1][0].guest_buffer.guest.context == second);
        check_equal(turbowasm_component_endpoint_submit(&ends[1][1], &buffers[1]), TURBOWASM_OK);
        take(&ends[1][1], TURBOWASM_OK); take(&ends[1][0], TURBOWASM_OK);
        check_equal(exec.async_buffer_owners, 0u); check_equal(exec.async_resource_owners, 0u);
    }
    it("releases a pending async read context only when cancellation is delivered") {
        pair(0, true, false, false); create(0, "payload-read-async", ends[0][0].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, UINT32_MAX);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK); check_equal(exec.async_buffer_owners, 1u);
        check_equal(turbowasm_component_endpoint_cancel(&ends[0][0]), TURBOWASM_OK);
        check_equal(exec.async_buffer_owners, 1u); take(&ends[0][0], TURBOWASM_OK); check_equal(exec.async_buffer_owners, 0u);
    }
    it("releases prepared contexts when guest memory admission fails") {
        pair(0, true, false, false); create(0, "payload-read", ends[0][0].waitable.handle, 65536, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_equal(exec.async_buffer_owners, 0u); check_null(ends[0][0].operation);
    }
    it("rolls back a resource reservation when a mixed endpoint cannot fit the handle quota") {
        turbowasm_component_resource_table_destroy(&exec.resource_table);
        check_true(turbowasm_component_resource_table_init(&exec.resource_table, 2));
        pair(0, true, false, true); mixed_input(); buffer(0, inputs, 1);
        create(0, "payload-mixed-read", ends[0][0].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][1], &buffers[0]), TURBOWASM_OK);
        take(&ends[0][1], TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(exec.async_buffer_owners, 0u); check_equal(exec.resource_table.live_count, 1u);
        check_equal(inputs[0].kind, TURBOWASM_COMPONENT_TYPE_TUPLE); check_null(nested[0].lower_scope);
        check_equal(destructions(), 0u);
    }
    it("preserves the same-Component nonnumeric copy trap and the other pending context") {
        uint32_t type = payload_type(false, false);
        check_equal(turbowasm_component_endpoint_pair_open(&exec.binary->type_graph, type,
            &exec.resource_table, &exec.resource_table, &ends[0][0], &ends[0][1]), TURBOWASM_OK);
        create(0, "payload-read", ends[0][0].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        create(1, "payload-write", ends[0][1].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_TRAPPED);
        check_equal(exec.async_buffer_owners, 1u); check_null(ends[0][1].operation); check_equal(destructions(), 0u);
    }
    it("transfers owned resources in both directions through futures") {
        pair(0, false, true, false); buffer(0, outputs, 1);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][0], &buffers[0]), TURBOWASM_OK);
        create(0, "payload-future-write", ends[0][1].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 1); take(&ends[0][0], TURBOWASM_OK);
        pair(1, true, true, false); buffer(1, outputs, 1);
        check_equal(turbowasm_component_endpoint_submit(&ends[1][1], &buffers[1]), TURBOWASM_OK);
        create(1, "payload-future-read", ends[1][0].waitable.handle, 512, 1);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK); result(1, 1); take(&ends[1][1], TURBOWASM_OK);
        check_equal(outputs[0].kind, 0); check_equal(exec.async_buffer_owners, 0u); check_equal(destructions(), 1u);
    }
    it("recovers all guest-write allocation failures without leaking contexts or resource owners") {
        size_t baseline, budget; bool succeeded = false;
        pair(0, false, false, false); buffer(0, outputs, 2);
        check_equal(turbowasm_component_endpoint_submit(&ends[0][0], &buffers[0]), TURBOWASM_OK);
        create(0, "payload-write", ends[0][1].waitable.handle, 512, 2);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); result(0, 2);
        take(&ends[0][0], TURBOWASM_OK); check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK); close_pair(0);
        check_equal(turbowasm_component_value_destroy(&outputs[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&outputs[1]), TURBOWASM_OK); baseline = live;
        for (budget = 0; budget < 200; ++budget) {
            turbowasm_status status;
            pair(0, false, false, false); buffer(0, outputs, 2);
            check_equal(turbowasm_component_endpoint_submit(&ends[0][0], &buffers[0]), TURBOWASM_OK);
            create(0, "payload-write", ends[0][1].waitable.handle, 512, 2);
            allowance = budget; status = turbowasm_component_task_resume(&tasks[0], NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { result(0, 2); succeeded = true; }
            else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK); close_pair(0);
            check_equal(turbowasm_component_value_destroy(&outputs[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_value_destroy(&outputs[1]), TURBOWASM_OK); clear_guest_values();
            check_equal(exec.async_buffer_owners, 0u); check_equal(exec.async_resource_owners, 0u); check_equal(live, baseline);
            if (succeeded) break;
        }
        /* Allocation counts differ between interpreted and compiled frames;
         * the sweep must exercise failure and reach a leak-free success. */
        check_true(succeeded); check_greater(budget, 0u);
    }
}
