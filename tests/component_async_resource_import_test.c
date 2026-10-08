#include "component_exec.h"
#include "component_subtask.h"
#include "component_endpoint_builtin.h"
#include "runtime_alloc.h"
#include "instance_internal.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_lower.h"
#include "fixtures/component_async_resource_import.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_component_binary binaries[2];
static turbowasm_component_exec execs[3];
static turbowasm_component_task root;
static turbowasm_component_task payload_tasks[3];
static turbowasm_value payload_arguments[3][3];
static turbowasm_component_endpoint payload_ends[2][2];
static turbowasm_component_value value;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;
static bool other_resource_provider;
static bool failing_resource;
static const char *borrow_override;
static const turbowasm_component_exec_async_limits limits = {8u, 32u};
static turbowasm_component_task *host_task;
static bool host_wait, host_fail;
static unsigned host_calls, host_unwinds;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static bool interrupt_copy(void *context) { ++*(unsigned *)context; return true; }
static bool name_is(turbowasm_component_name name, const char *text) {
    return name.size == strlen(text) && memcmp(name.bytes, text, name.size) == 0;
}
static bool can_bind(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    (void)context; (void)function; (void)graph; (void)type; return name_is(instance, "provider");
}
static turbowasm_status target(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    turbowasm_component_exec **provider, uint32_t *adapter) {
    const turbowasm_component_task_binding *binding = NULL;
    const char *override = failing_resource && name_is(function, "resource-result") ? "resource-result-trap" : NULL;
    if (context == &execs[1] && name_is(function, "resource-borrow-child")) override = borrow_override;
    turbowasm_status status; (void)graph; (void)type;
    if (!name_is(instance, "provider")) return TURBOWASM_TYPE_MISMATCH;
    *provider = context;
    status = turbowasm_component_exec_async_export(*provider,
        override != NULL ? (const uint8_t *)override : function.bytes,
        override != NULL ? (uint32_t)strlen(override) : function.size, &binding);
    if (status == TURBOWASM_OK) *adapter = (uint32_t)(binding - (*provider)->async_functions);
    return status;
}
static turbowasm_status resource_target(void *context, turbowasm_component_name instance,
    turbowasm_component_name resource, turbowasm_component_exec **provider, uint32_t *type) {
    turbowasm_component_exec *exec = other_resource_provider ? &execs[2] : context; uint32_t i;
    if (!name_is(instance, "provider")) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *item = &exec->binary->exports[i];
        if (item->kind == TURBOWASM_COMPONENT_EXTERN_TYPE && item->name.size == resource.size &&
            memcmp(item->name.bytes, resource.bytes, resource.size) == 0) {
            *provider = exec; *type = item->item_index; return TURBOWASM_OK;
        }
    }
    return TURBOWASM_TYPE_MISMATCH;
}
static turbowasm_component_exec_imports imports(unsigned provider) {
    turbowasm_component_exec_imports result = {0};
    result.context = &execs[provider]; result.can_bind = can_bind;
    result.async_target = target; result.resource_target = resource_target; return result;
}
static void attach(unsigned index) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < execs[index].core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0}; if (execs[index].core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(execs[index].core_instances[i].impl, &backend, 1u), TURBOWASM_OK);
    }
#else
    (void)index;
#endif
}
static const turbowasm_component_task_binding *binding(unsigned index, const char *name) {
    const turbowasm_component_task_binding *out = NULL;
    check_equal(turbowasm_component_exec_async_export(&execs[index], (const uint8_t *)name,
        (uint32_t)strlen(name), &out), TURBOWASM_OK); return out;
}
static void compiled(unsigned index, const char *name) {
#ifdef TURBOWASM_TEST_MIR
    const turbowasm_component_task_binding *b = binding(index, name);
    check_equal(((turbowasm_instance_impl *)b->instance->impl)->jit_functions[b->function_index].state, TURBOWASM_JIT_COMPILED);
#else
    (void)index; (void)name;
#endif
}
static void compiled_destructor(void) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < execs[0].binary->type_graph.count; ++i) {
        const turbowasm_component_type *type = &execs[0].binary->type_graph.types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE && !type->as.resource.identity_alias && type->as.resource.has_destructor) {
            const turbowasm_component_exec_core_function *function = &execs[0].core_functions[type->as.resource.destructor_index];
            check_equal(((turbowasm_instance_impl *)execs[0].core_instances[function->instance_index].impl)->jit_functions[function->function_index].state,
                TURBOWASM_JIT_COMPILED);
        }
    }
#endif
}
static void create(unsigned index, const char *name) {
    check_equal(turbowasm_component_task_create(&root, &execs[index].task_domain, binding(index, name)), TURBOWASM_OK);
}
static turbowasm_status drive(const turbowasm_execution_options *options) {
    unsigned turns = 0u; turbowasm_status status;
    do {
        unsigned i;
        for (i = 1u; i < 3u; ++i) if (execs[i].initialized) {
            uint32_t pending;
            status = turbowasm_component_exec_async_poll(&execs[i], 8u, options, &pending);
            if (status != TURBOWASM_OK && status != TURBOWASM_YIELDED) return status;
        }
        status = turbowasm_component_task_resume(&root, options);
        check_less(++turns, 10000u);
    } while (status == TURBOWASM_YIELDED);
    return status;
}
static uint32_t destructions(void) {
    turbowasm_component_value count = {0}; const turbowasm_component_task_binding *b = binding(0u, "payload-read");
    check_equal(turbowasm_component_canonical_lift_value(&execs[0].binary->type_graph,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32), &b->memory, 400u, &count), TURBOWASM_OK);
    return count.as.u32;
}
static turbowasm_status release_rep(void *context, uint64_t identity, turbowasm_value rep) {
    return turbowasm_component_exec_resource_release(context, identity, rep);
}
static void release_end(turbowasm_component_endpoint *end) {
    turbowasm_component_event event;
    if (!end->initialized || end->closed) return;
    if (end->operation != NULL) {
        if (end->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
            check_equal(turbowasm_component_endpoint_cancel(end), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_take(end, &event), end->failure);
    }
    check_equal(turbowasm_component_endpoint_close(end), TURBOWASM_OK);
}
static void clear_handles(unsigned index) {
    uint32_t i; turbowasm_component_exec *exec = &execs[index];
    for (i = 0u; i < exec->resource_table.capacity; ++i) {
        uint32_t handle; turbowasm_component_handle_kind kind; void *object;
        if (!turbowasm_component_handle_at(&exec->resource_table, i, &handle, &kind, &object)) continue;
        if (kind == TURBOWASM_COMPONENT_HANDLE_RESOURCE)
            check_equal(turbowasm_component_resource_drop(&exec->resource_table, handle,
                exec->resource_table.entries[i].resource_identity, release_rep, exec), TURBOWASM_OK);
        else if (kind == TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET)
            check_equal(turbowasm_component_task_set_drop(&exec->task_domain, handle), TURBOWASM_OK);
        else {
            turbowasm_component_endpoint *end = turbowasm_component_endpoint_get(&exec->resource_table, handle, kind);
            check_not_null(end); release_end(end);
        }
    }
    turbowasm_component_endpoint_domain_collect(&exec->task_domain);
}
static void cleanup(void) {
    int i;
    for (i = 2; i >= 0; --i) check_equal(turbowasm_component_task_destroy(&payload_tasks[i]), TURBOWASM_OK);
    check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
    for (i = 0; i < 2; ++i) { release_end(&payload_ends[i][0]); release_end(&payload_ends[i][1]); }
    memset(payload_ends, 0, sizeof(payload_ends));
    for (i = 2; i >= 0; --i) if (execs[i].initialized) {
        check_equal(turbowasm_component_exec_async_abort(&execs[i], TURBOWASM_INTERRUPTED), TURBOWASM_OK);
        clear_handles((unsigned)i);
    }
}
static uint32_t payload_type(bool future, bool mixed) {
    uint32_t i, expected = TURBOWASM_COMPONENT_VALUE_RESOURCES;
    if (mixed) expected |= TURBOWASM_COMPONENT_VALUE_ENDPOINTS | TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY;
    for (i = 0u; i < execs[0].binary->type_graph.count; ++i) {
        const turbowasm_component_type *type = &execs[0].binary->type_graph.types[i]; uint32_t features;
        if (type->kind == (future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM) &&
            type->as.async_value.has_payload && turbowasm_component_transfer_type_features(&execs[0].binary->type_graph,
                type->as.async_value.payload, &features) && features == expected) return i;
    }
    return UINT32_MAX;
}
static void payload_pair(unsigned pair, unsigned reader, unsigned writer, bool future, bool mixed) {
    uint32_t type = payload_type(future, mixed); check_not_equal(type, UINT32_MAX);
    check_equal(turbowasm_component_endpoint_pair_open(&execs[0].binary->type_graph, type,
        &execs[reader].resource_table, &execs[writer].resource_table,
        &payload_ends[pair][0], &payload_ends[pair][1]), TURBOWASM_OK);
}
static turbowasm_status prepare_payload(void *context, turbowasm_component_task *task,
    turbowasm_value *out, size_t capacity, size_t *count) {
    (void)task; if (capacity < 3u) return TURBOWASM_TYPE_MISMATCH;
    memcpy(out, context, 3u * sizeof(*out)); *count = 3u; return TURBOWASM_OK;
}
static void create_payload(unsigned slot, unsigned owner, const char *name, uint32_t handle, uint32_t address, uint32_t count) {
    turbowasm_component_task_binding copy = *binding(owner, name); unsigned i;
    for (i = 0u; i < 3u; ++i) payload_arguments[slot][i].kind = TURBOWASM_VALUE_I32;
    payload_arguments[slot][0].as.i32 = (int32_t)handle; payload_arguments[slot][1].as.i32 = (int32_t)address;
    payload_arguments[slot][2].as.i32 = (int32_t)count;
    copy.prepare = prepare_payload; copy.prepare_context = payload_arguments[slot];
    check_equal(turbowasm_component_task_create(&payload_tasks[slot], &execs[owner].task_domain, &copy), TURBOWASM_OK);
}
static turbowasm_status drive_payload(unsigned slot, const turbowasm_execution_options *options) {
    unsigned turns = 0u; turbowasm_status status;
    do {
        unsigned i;
        for (i = 1u; i < 3u; ++i) if (execs[i].initialized) {
            uint32_t pending;
            status = turbowasm_component_exec_async_poll(&execs[i], 8u, options, &pending);
            if (status != TURBOWASM_OK && status != TURBOWASM_YIELDED) return status;
        }
        status = turbowasm_component_task_resume(&payload_tasks[slot], options);
        check_less(++turns, 10000u);
    } while (status == TURBOWASM_YIELDED);
    return status;
}
static void payload_result(unsigned slot, uint32_t expected) {
    turbowasm_component_value result = {0};
    check_equal(turbowasm_component_task_take_result(&payload_tasks[slot], &result), TURBOWASM_OK);
    check_equal(result.as.u32, expected); check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)payload_tasks[slot].binding.instance->impl)->jit_functions[
        payload_tasks[slot].binding.function_index].state, TURBOWASM_JIT_COMPILED);
#endif
}
static void number(unsigned index, const char *name, uint32_t expected, const turbowasm_execution_options *options) {
    create(index, name); check_equal(drive(options), TURBOWASM_OK);
    check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
    check_equal(value.as.u32, expected); compiled(index, name); cleanup();
}
static void suspend_destructor(void) {
    turbowasm_execution_options options = {0}; unsigned turns = 0u;
    options.has_fuel_limit = true; options.fuel = 4u; create(1u, "roundtrip");
    while (execs[0].task_domain.synchronous_depth == 0u) {
        uint32_t pending; turbowasm_status status;
        status = turbowasm_component_exec_async_poll(&execs[1], 8u, &options, &pending);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&root, &options), TURBOWASM_YIELDED);
        check_less(++turns, 10000u);
    }
    check_not_null(execs[0].task_domain.auxiliary);
}

static bool make_import(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    return can_bind(context, instance, function, graph, type) && name_is(function, "resource-result");
}
static bool resource_host_import(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    return can_bind(context, instance, function, graph, type) && !name_is(function, "resource-result");
}
static turbowasm_status invoke_resource_host(void *context, turbowasm_component_task *task, turbowasm_host_call *call,
    turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    turbowasm_component_value *arguments, size_t count) {
    turbowasm_component_value result = {0};
    turbowasm_status status = TURBOWASM_OK, cleanup_status;
    check_true(context == &execs[0]); check_true(task->domain == &execs[1].task_domain);
    check_true(name_is(instance, "provider")); check_equal(count, 1u);
    check_true(graph == task->binding.graph); check_equal(type, task->binding.function_type);
    host_task = task; ++host_calls;
    if (host_wait) {
        turbowasm_host_wait wait; int completion;
        status = turbowasm_host_call_wait(call, 91u, &wait, &completion);
    }
    if (status == TURBOWASM_OK) {
        if (name_is(function, "resource-borrow-child")) {
            check_equal(arguments[0].kind, TURBOWASM_COMPONENT_TYPE_BORROW);
            result.kind = TURBOWASM_COMPONENT_TYPE_U32; result.as.u32 = (uint32_t)arguments[0].as.resource_rep.as.i32;
        } else { result = arguments[0]; memset(&arguments[0], 0, sizeof(result)); }
        if (host_fail) { task->trap = TURBOWASM_TRAP_UNREACHABLE; status = TURBOWASM_TRAPPED; }
        else status = turbowasm_component_task_return(task->domain, &result);
    }
    if (status == TURBOWASM_INTERRUPTED) ++host_unwinds;
    cleanup_status = turbowasm_component_value_destroy(&result); host_task = NULL;
    return status != TURBOWASM_OK ? status : cleanup_status;
}
static void use_resource_host(void) {
    turbowasm_component_exec_imports sets[2];
    check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK);
    sets[0] = imports(0u); sets[0].can_bind = make_import;
    memset(&sets[1], 0, sizeof(sets[1])); sets[1].context = &execs[0];
    sets[1].can_bind = resource_host_import; sets[1].async_invoke = invoke_resource_host;
    check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, sets, 2), TURBOWASM_OK);
    attach(1u); check_equal(execs[0].async_import_owners, 2u);
}
static void complete_resource_host(void) {
    turbowasm_host_wait wait;
    check_not_null(host_task); check_true(turbowasm_execution_pending_host_wait(&host_task->core, &wait));
    check_equal(turbowasm_execution_complete_host_wait(&host_task->core, wait, 0), TURBOWASM_OK);
}

spec("resource imports between async Component instances") {
    before_each() {
        turbowasm_component_exec_imports set;
        live = 0u; allowance = SIZE_MAX; other_resource_provider = false; failing_resource = false;
        borrow_override = NULL;
        host_task = NULL; host_wait = host_fail = false; host_calls = host_unwinds = 0u;
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[0], component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[1], component_async_resource_import_bytes,
            sizeof(component_async_resource_import_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&execs[0], &binaries[0], &limits), TURBOWASM_OK);
        set = imports(0u);
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(0u); attach(1u);
    }
    after_each() {
        int i; allowance = SIZE_MAX; cleanup();
        for (i = 2; i >= 0; --i) check_equal(turbowasm_component_exec_destroy(&execs[i]), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binaries[1]); turbowasm_component_binary_destroy(&binaries[0]);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("routes own borrow and composite resource values through a host with defining-instance identity") {
        use_resource_host();
        number(1u, "roundtrip", 42u, NULL);
        number(1u, "borrow-roundtrip", 42u, NULL);
        number(1u, "text", 5u, NULL);
        check_equal(host_calls, 3u); check_equal(destructions(), 3u); compiled_destructor();
    }
    it("retains imported resource owners and loans across host I/O until terminal delivery") {
        unsigned i; const char *entries[] = {"roundtrip", "borrow-roundtrip", "text"};
        use_resource_host(); host_wait = true;
        for (i = 0u; i < 3u; ++i) {
            create(1u, entries[i]); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_YIELDED);
            check_not_null(host_task); check_equal(destructions(), i); check_equal(execs[1].async_resource_owners, 1u);
            check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
            complete_resource_host(); check_equal(drive(NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
            check_equal(value.as.u32, i == 2u ? 5u : 42u); cleanup();
            check_equal(destructions(), i + 1u); check_equal(execs[1].async_resource_owners, 0u);
        }
    }
    it("unwinds a suspended host before destroying its own value or releasing its borrow lender") {
        unsigned i; const char *entries[] = {"roundtrip", "borrow-roundtrip", "text"};
        use_resource_host(); host_wait = true;
        for (i = 0u; i < 3u; ++i) {
            create(1u, entries[i]); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_YIELDED);
            cleanup(); check_equal(host_unwinds, i + 1u); check_null(host_task);
            check_equal(destructions(), i + 1u); check_equal(execs[1].async_resource_owners, 0u);
        }
    }
    it("destroys a consumed host resource result once when the callback fails") {
        use_resource_host(); host_fail = true;
        create(1u, "text"); check_equal(drive(NULL), TURBOWASM_TRAPPED);
        check_equal(root.trap, TURBOWASM_TRAP_UNREACHABLE); cleanup();
        check_equal(destructions(), 1u); check_null(host_task); check_equal(execs[1].async_resource_owners, 0u);
    }
    it("round trips owned resources using destination IDs and the defining destructor") {
        uint32_t i; bool checked = false;
        for (i = 0u; i < execs[1].type_view->identity_count; ++i) {
            const turbowasm_component_resource_identity *identity = &execs[1].type_view->identities[i];
            if (identity->provider != NULL) {
                check_not_equal(identity->declaration, identity->provider_declaration); checked = true;
            }
        }
        check_true(checked);
        check_equal(execs[0].async_import_owners, 5u);
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
        number(1u, "roundtrip", 42u, NULL); check_equal(destructions(), 1u);
        compiled(0u, "resource-result"); compiled(0u, "resource-own-child");
        compiled_destructor();
    }
    it("borrows an imported owner back into its defining instance") {
        number(1u, "borrow-roundtrip", 42u, NULL);
        check_equal(destructions(), 1u); compiled(0u, "resource-borrow-child");
    }
    it("copies resource streams futures and mixed payloads between distinct instance tables") {
        unsigned kind, reverse, writer_first;
        for (kind = 0u; kind < 3u; ++kind) for (reverse = 0u; reverse < 2u; ++reverse)
        for (writer_first = 0u; writer_first < 2u; ++writer_first) {
            const char *read = kind == 0u ? "payload-read" : kind == 1u ? "payload-mixed-read" : "payload-future-read";
            const char *write = kind == 0u ? "payload-write" : kind == 1u ? "payload-mixed-write" : "payload-future-write";
            uint32_t count = kind == 0u ? 2u : 1u, before = destructions();
            unsigned reader = reverse, writer = 1u - reverse;
            payload_pair(0u, reader, writer, kind == 2u, kind == 1u);
            create_payload(writer_first ? 1u : 0u, reader, read, payload_ends[0][0].waitable.handle, 512u, count);
            create_payload(writer_first ? 0u : 1u, writer, write, payload_ends[0][1].waitable.handle, 512u, count);
            check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
            check_equal(drive_payload(1u, NULL), TURBOWASM_OK); payload_result(1u, count);
            check_equal(drive_payload(0u, NULL), TURBOWASM_OK); payload_result(0u, count);
            cleanup(); check_equal(destructions() - before, count);
            check_equal(execs[0].async_buffer_owners, 0u); check_equal(execs[1].async_buffer_owners, 0u);
        }
    }
    it("keeps a pending destination buffer valid after its initiating task exits") {
        payload_pair(0u, 1u, 0u, false, true);
        create_payload(0u, 1u, "payload-mixed-read-async", payload_ends[0][0].waitable.handle, 512u, 1u);
        check_equal(drive_payload(0u, NULL), TURBOWASM_OK); payload_result(0u, UINT32_MAX);
        check_equal(turbowasm_component_task_destroy(&payload_tasks[0]), TURBOWASM_OK);
        check_equal(execs[1].async_buffer_owners, 1u);
        create_payload(1u, 0u, "payload-mixed-write", payload_ends[0][1].waitable.handle, 512u, 1u);
        check_equal(drive_payload(1u, NULL), TURBOWASM_OK); payload_result(1u, 1u);
        cleanup(); check_equal(destructions(), 1u); check_equal(execs[1].async_buffer_owners, 0u);
    }
    it("retains both instance guards through foreign realloc fuel yields and forced unwind") {
        unsigned abort;
        for (abort = 0u; abort < 2u; ++abort) {
            turbowasm_execution_options options = {0}; unsigned turns = 0u;
            options.has_fuel_limit = true; options.fuel = 4u;
            payload_pair(0u, 1u, 0u, false, true);
            create_payload(0u, 1u, "payload-mixed-read", payload_ends[0][0].waitable.handle, 512u, 1u);
            check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
            create_payload(1u, 0u, "payload-mixed-write", payload_ends[0][1].waitable.handle, 512u, 1u);
            do {
                check_equal(turbowasm_component_task_resume(&payload_tasks[1], &options), TURBOWASM_YIELDED);
                check_less(++turns, 10000u);
            } while (execs[1].may_leave);
            check_true(execs[0].task_domain.auxiliary == &payload_tasks[1]);
            check_true(execs[1].task_domain.auxiliary == &payload_tasks[1]);
            check_true(payload_ends[0][0].waitable.delivering); check_true(payload_ends[0][1].waitable.delivering);
            check_equal(turbowasm_component_task_destroy(&payload_tasks[0]), TURBOWASM_TRAPPED);
            if (abort == 0u) {
                turbowasm_execution_options interrupt = {0}; unsigned checks = 0u;
                interrupt.should_interrupt = interrupt_copy; interrupt.interrupt_context = &checks;
                check_equal(turbowasm_component_task_resume(&payload_tasks[1], &interrupt), TURBOWASM_YIELDED);
                check_greater(checks, 0u);
                check_equal(turbowasm_execution_yield_reason_get(&payload_tasks[1].core), TURBOWASM_YIELD_INTERRUPTION);
                check_false(execs[1].may_leave); check_true(execs[1].task_domain.auxiliary == &payload_tasks[1]);
                check_equal(drive_payload(1u, &options), TURBOWASM_OK); payload_result(1u, 1u);
                check_equal(drive_payload(0u, &options), TURBOWASM_OK); payload_result(0u, 1u);
            } else check_equal(turbowasm_component_task_destroy(&payload_tasks[1]), TURBOWASM_OK);
            check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
            check_true(execs[0].may_leave); check_true(execs[1].may_leave);
            cleanup(); check_equal(destructions(), abort + 1u);
        }
    }
    it("uses the reader's fuel when failed lowering destroys a foreign source resource") {
        turbowasm_execution_options options = {0}; unsigned turns = 0u;
        options.has_fuel_limit = true; options.fuel = 4u;
        /* Configure before table allocation: one end and one reservation fit. */
        execs[1].resource_table.max_entries = 2u;
        payload_pair(0u, 1u, 0u, false, true);
        create_payload(0u, 0u, "payload-mixed-write", payload_ends[0][1].waitable.handle, 512u, 1u);
        check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
        create_payload(1u, 1u, "payload-mixed-read", payload_ends[0][0].waitable.handle, 512u, 1u);
        do {
            check_equal(turbowasm_component_task_resume(&payload_tasks[1], &options), TURBOWASM_YIELDED);
            check_less(++turns, 10000u);
        } while (execs[0].task_domain.synchronous_depth == 0u);
        check_true(execs[1].task_domain.auxiliary == &payload_tasks[1]);
        check_equal(drive_payload(1u, &options), TURBOWASM_OUT_OF_MEMORY);
        check_equal(drive_payload(0u, NULL), TURBOWASM_OUT_OF_MEMORY);
        cleanup(); check_equal(destructions(), 1u); check_equal(execs[0].task_domain.synchronous_depth, 0u);
        compiled_destructor();
    }
    it("uses a third instance's forwarding task to drive pending mixed payloads") {
        turbowasm_component_exec_imports set = imports(0u);
        turbowasm_execution_options options = {0}; unsigned turns = 0u;
        options.has_fuel_limit = true; options.fuel = 4u;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u);
        payload_pair(0u, 2u, 0u, false, true); payload_pair(1u, 1u, 2u, false, true);
        create_payload(0u, 1u, "payload-mixed-read", payload_ends[1][0].waitable.handle, 512u, 1u);
        create_payload(1u, 0u, "payload-mixed-write", payload_ends[0][1].waitable.handle, 512u, 1u);
        check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&payload_tasks[1], NULL), TURBOWASM_YIELDED);
        create_payload(2u, 2u, "payload-forward", payload_ends[0][0].waitable.handle, payload_ends[1][1].waitable.handle, 0u);
        do {
            check_equal(turbowasm_component_task_resume(&payload_tasks[2], &options), TURBOWASM_YIELDED);
            check_less(++turns, 10000u);
        } while (execs[1].may_leave);
        check_true(execs[0].task_domain.auxiliary == &payload_tasks[2]);
        check_true(execs[1].task_domain.auxiliary == &payload_tasks[2]);
        check_true(execs[2].task_domain.auxiliary == &payload_tasks[2]);
        check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&payload_tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(drive_payload(2u, &options), TURBOWASM_OK); payload_result(2u, 1u);
        check_equal(drive_payload(1u, &options), TURBOWASM_OK); payload_result(1u, 1u);
        check_equal(drive_payload(0u, &options), TURBOWASM_OK); payload_result(0u, 1u);
        cleanup(); check_equal(destructions(), 1u);
    }
    it("drops a foreign task borrow only after its transitive loan is delivered") {
        turbowasm_component_exec_imports set = imports(1u);
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); create(2u, "borrow-roundtrip");
        check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_YIELDED);
        check_not_null(root.children); check_equal(root.children->task_owner->borrowed_handles, 1u);
        check_not_null(root.children->task_owner->children);
        check_equal(drive(NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
        check_equal(value.as.u32, 42u); check_equal(destructions(), 1u);
        compiled(2u, "borrow-roundtrip"); compiled(1u, "resource-borrow-child"); compiled(0u, "resource-borrow-child");
    }
    it("preserves borrow scopes across shared fuel suspension") {
        turbowasm_component_exec_imports set = imports(1u);
        turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 4u;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); number(2u, "borrow-roundtrip", 42u, &options); check_equal(destructions(), 1u);
    }
    it("rejects dropping an owner with an outstanding cross-instance borrow") {
        create(1u, "borrow-drop-live"); check_equal(drive(NULL), TURBOWASM_TRAPPED);
        check_equal(destructions(), 0u); cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[1].async_resource_owners, 0u);
    }
    it("unwinds a trapped borrowing relay before releasing its caller loan") {
        unsigned mode;
        for (mode = 0u; mode < 2u; ++mode) {
            turbowasm_component_exec_imports set = imports(1u);
            borrow_override = mode == 0u ? "borrow-return-live" : "borrow-abort";
            check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
            attach(2u); create(2u, "borrow-roundtrip"); check_equal(drive(NULL), TURBOWASM_TRAPPED);
            cleanup(); check_equal(destructions(), mode + 1u);
            check_equal(execs[0].async_resource_owners, 0u); check_equal(execs[1].async_resource_owners, 0u);
            check_equal(execs[2].async_resource_owners, 0u);
            check_equal(turbowasm_component_exec_destroy(&execs[2]), TURBOWASM_OK);
        }
    }
    it("aborts a suspended transitive borrow without leaving task counters or lenders alive") {
        turbowasm_component_exec_imports set = imports(1u);
        uint32_t pending;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); create(2u, "borrow-roundtrip");
        check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_YIELDED);
        check_not_null(root.children); check_equal(root.children->task_owner->borrowed_handles, 1u);
        check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_async_abort(&execs[2], TURBOWASM_INTERRUPTED), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_async_poll(&execs[1], 8u, NULL, &pending), TURBOWASM_OK);
        check_equal(pending, 0u);
        cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].task_domain.count, 0u); check_equal(execs[1].task_domain.count, 0u);
        check_equal(execs[2].task_domain.count, 0u);
    }
    it("retains both instances while the host owns an imported result") {
        create(1u, "resource-result"); check_equal(drive(NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
        check_equal(value.as.resource_rep.as.i32, 42); check_not_null(value.resource_instance_key);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
        check_equal(destructions(), 0u); check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
        check_equal(destructions(), 1u);
    }
    it("transfers resources with strings across memory32 and memory64 under small fuel") {
        turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 4u;
        number(1u, "text", 5u, &options); check_equal(destructions(), 1u); compiled(0u, "resource-text-child");
    }
    it("retains transitive providers and returns resources through an importing instance") {
        turbowasm_component_exec_imports set = imports(1u);
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); check_equal(execs[1].async_import_owners, 5u);
        number(2u, "roundtrip", 42u, NULL); check_equal(destructions(), 1u);
        compiled(1u, "resource-result"); compiled(1u, "resource-own-child");
    }
    it("rejects ambiguous resource resolvers without retaining a provider") {
        turbowasm_component_exec_imports sets[2] = {imports(0u), imports(0u)};
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, sets, 2u), TURBOWASM_TYPE_MISMATCH);
        check_null(execs[2].binary); check_equal(execs[0].async_import_owners, 5u);
    }
    it("rejects a function provider whose resource belongs to another instance of the same binary") {
        turbowasm_component_exec_imports set = imports(0u);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&execs[2], &binaries[0], &limits), TURBOWASM_OK);
        other_resource_provider = true;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_TYPE_MISMATCH);
        other_resource_provider = false;
        check_null(execs[1].binary); check_equal(execs[0].async_import_owners, 0u); check_equal(execs[2].async_import_owners, 0u);
    }
    it("destroys the uncommitted result when the destination resource table is full") {
        uint32_t occupied;
        execs[1].resource_table.max_entries = 1u;
        check_equal(turbowasm_component_task_set_new(&execs[1].task_domain, &occupied), TURBOWASM_OK);
        create(1u, "roundtrip"); check_equal(drive(NULL), TURBOWASM_OUT_OF_MEMORY);
        cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].async_resource_owners, 0u); check_equal(execs[1].async_resource_owners, 0u);
    }
    it("unwinds a resource reservation suspended in cross-instance result realloc") {
        turbowasm_execution_options options = {0}; unsigned turns = 0u;
        options.has_fuel_limit = true; options.fuel = 4u;
        create(1u, "text");
        while (execs[1].task_domain.auxiliary == NULL) {
            uint32_t pending; turbowasm_status status;
            status = turbowasm_component_task_resume(&root, &options);
            check_equal(status, TURBOWASM_YIELDED);
            status = turbowasm_component_exec_async_poll(&execs[1], 8u, &options, &pending);
            check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
            check_less(++turns, 10000u);
        }
        check_greater(execs[0].async_resource_owners, 0u);
        cleanup(); check_equal(destructions(), 1u);
        check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
    }
    it("shares caller fuel with a foreign resource destructor and resumes without replay") {
        turbowasm_component_task sibling = {0};
        suspend_destructor();
        check_equal(turbowasm_component_task_create(&sibling, &execs[0].task_domain, binding(0u, "resource-result")), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&sibling, NULL), TURBOWASM_YIELDED);
        check_equal(drive(NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&sibling), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
        check_equal(value.as.u32, 42u); cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].task_domain.synchronous_depth, 0u); check_null(execs[0].task_domain.auxiliary);
    }
    it("unwinds a foreign destructor and clears its temporary task when the caller is destroyed") {
        suspend_destructor(); cleanup();
        check_true(destructions() <= 1u);
        check_equal(execs[0].task_domain.synchronous_depth, 0u); check_null(execs[0].task_domain.auxiliary);
        check_null(execs[0].task_domain.active);
    }
    it("propagates the foreign destructor trap after consuming the owned handle") {
        turbowasm_component_exec_imports set = imports(0u);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK); failing_resource = true;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        failing_resource = false; attach(1u);
        create(1u, "roundtrip"); check_equal(drive(NULL), TURBOWASM_TRAPPED);
        check_equal(root.trap, TURBOWASM_TRAP_UNREACHABLE); cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].resource_table.live_count, 0u); check_equal(execs[1].resource_table.live_count, 0u);
    }
    it("rolls back resource and function provider references after every constructor allocation failure") {
        size_t baseline = live, budget; turbowasm_component_exec_imports set = imports(0u);
        for (budget = 0u; budget < 2048u; ++budget) {
            turbowasm_status status; allowance = budget;
            status = turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(execs[0].async_import_owners, 10u);
                check_equal(turbowasm_component_exec_destroy(&execs[2]), TURBOWASM_OK);
                check_equal(execs[0].async_import_owners, 5u); check_equal(live, baseline); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(execs[2].binary);
            check_equal(execs[0].async_import_owners, 5u); check_equal(live, baseline);
        }
        check_greater(budget, 0u); check_less(budget, 2048u);
    }
    it("rolls back partial mixed-value transfers and releases each resource exactly once") {
        size_t baseline, budget;
        number(1u, "text", 5u, NULL); baseline = live;
        for (budget = 0u; budget < 1024u; ++budget) {
            turbowasm_status status; uint32_t before = destructions();
            create(1u, "text"); allowance = budget; status = drive(NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
                check_equal(value.as.u32, 5u);
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup(); check_equal(live, baseline); check_equal(execs[0].async_resource_owners, 0u);
            check_equal(execs[1].async_resource_owners, 0u);
            check_true(destructions() - before <= 1u);
            if (status == TURBOWASM_OK) check_equal(destructions() - before, 1u);
            if (status == TURBOWASM_OK) break;
        }
        check_greater(budget, 0u); check_less(budget, 1024u);
    }
    it("rolls back every allocation failure in a transitive borrow call") {
        turbowasm_component_exec_imports set = imports(1u);
        size_t baseline, budget;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); number(2u, "borrow-roundtrip", 42u, NULL); baseline = live;
        for (budget = 0u; budget < 1024u; ++budget) {
            turbowasm_status status; uint32_t before = destructions(); unsigned i;
            create(2u, "borrow-roundtrip"); allowance = budget; status = drive(NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
                check_equal(value.as.u32, 42u);
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup(); check_equal(live, baseline);
            for (i = 0u; i < 3u; ++i) {
                check_equal(execs[i].async_resource_owners, 0u); check_equal(execs[i].task_domain.count, 0u);
            }
            check_true(destructions() - before <= 1u);
            if (status == TURBOWASM_OK) { check_equal(destructions() - before, 1u); break; }
        }
        check_greater(budget, 0u); check_less(budget, 1024u);
    }
    it("unwinds every allocation failure during a foreign mixed payload conversion") {
        size_t baseline = 0u, budget;
        for (budget = 0u; budget < 1024u; ++budget) {
            turbowasm_status status; uint32_t before = destructions();
            /* The first complete copy warms table capacities and MIR entries. */
            payload_pair(0u, 1u, 0u, false, true);
            create_payload(0u, 1u, "payload-mixed-read", payload_ends[0][0].waitable.handle, 512u, 1u);
            check_equal(turbowasm_component_task_resume(&payload_tasks[0], NULL), TURBOWASM_YIELDED);
            create_payload(1u, 0u, "payload-mixed-write", payload_ends[0][1].waitable.handle, 512u, 1u);
            allowance = budget == 0u ? SIZE_MAX : budget - 1u;
            status = drive_payload(1u, NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                payload_result(1u, 1u); check_equal(drive_payload(0u, NULL), TURBOWASM_OK); payload_result(0u, 1u);
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup();
            if (budget == 0u) baseline = live;
            check_equal(live, baseline); check_true(destructions() - before <= 1u);
            check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
            check_equal(execs[0].async_buffer_owners, 0u); check_equal(execs[1].async_buffer_owners, 0u);
            check_equal(execs[0].async_resource_owners, 0u); check_equal(execs[1].async_resource_owners, 0u);
            if (status == TURBOWASM_OK && budget != 0u) { check_equal(destructions() - before, 1u); break; }
        }
        check_greater(budget, 1u); check_less(budget, 1024u);
    }
}
