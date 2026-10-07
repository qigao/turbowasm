#include "component_async_call.h"
#include "component_endpoint.h"
#include "execution_internal.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_call.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_module module;
static turbowasm_instance instances[2];
static turbowasm_linker linker;
static turbowasm_component_type_graph graphs[2];
static turbowasm_component_resource_table tables[2];
static turbowasm_component_task_domain domains[2];
static bool may_leave[2];
static turbowasm_component_async_call call;
static turbowasm_component_async_call_binding binding;
static turbowasm_component_task parent;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;
static unsigned returns, waits, reallocations;
static bool reject_realloc;
static unsigned resource_drops;
typedef struct resource_context {
    turbowasm_component_resource_table *table;
    uint32_t staged, lender, borrowed;
    bool staged_borrow, reject_commit;
    unsigned commits, rollbacks;
} resource_context;
static resource_context resources[2];
static turbowasm_component_endpoint reader, writer;
static turbowasm_component_endpoint_codec codecs[2];
static bool reject_endpoint_commit;

static turbowasm_status commit_endpoint(void *context, turbowasm_component_task *task,
    turbowasm_component_value *values, uint32_t count) {
    (void)task; (void)values; check_equal(count, 1u);
    if (reject_endpoint_commit) return TURBOWASM_TRAPPED;
    return turbowasm_component_endpoint_codec_commit(context);
}
static turbowasm_status rollback_endpoint(void *context, turbowasm_component_task *task) {
    (void)task; return turbowasm_component_endpoint_codec_rollback(context);
}

static turbowasm_status release_resource(void *context) {
    resource_context *resource = context;
    if (resource->lender != 0) {
        uint32_t handle = resource->lender; resource->lender = 0;
        return turbowasm_component_resource_lend_release(resource->table, handle, 42);
    }
    ++resource_drops; return TURBOWASM_OK;
}
static turbowasm_status drop_resource(void *context, uint64_t identity, turbowasm_value value) {
    (void)context; check_equal(identity, 42u); check_equal(value.as.i32, 77);
    ++resource_drops; return TURBOWASM_OK;
}
static turbowasm_status lift_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type, uint32_t handle, turbowasm_component_value *out) {
    resource_context *resource = context;
    const turbowasm_component_type *definition = turbowasm_component_type_graph_get(graph, type.as.indexed);
    turbowasm_value rep; turbowasm_status status;
    if (definition->kind == TURBOWASM_COMPONENT_TYPE_BORROW) {
        status = turbowasm_component_resource_rep(resource->table, handle, 42, &rep);
        if (status == TURBOWASM_OK) status = turbowasm_component_resource_lend_acquire(resource->table, handle, 42);
        if (status == TURBOWASM_OK) resource->lender = handle;
    } else status = turbowasm_component_resource_take_owned(resource->table, handle, 42, &rep);
    if (status != TURBOWASM_OK) return status;
    out->kind = definition->kind; out->as.resource_rep = rep; out->resource_identity = 42;
    out->release = release_resource; out->release_context = resource; return TURBOWASM_OK;
}
static turbowasm_status lower_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type, const turbowasm_component_value *value, uint32_t *out) {
    resource_context *resource = context; turbowasm_status status;
    (void)graph; (void)type;
    check_equal(resource->staged, 0u);
    resource->staged_borrow = value->kind == TURBOWASM_COMPONENT_TYPE_BORROW;
    status = resource->staged_borrow
        ? turbowasm_component_resource_new_borrowed(resource->table, 42, value->as.resource_rep, out)
        : turbowasm_component_resource_new_owned(resource->table, 42, value->as.resource_rep, out);
    if (status == TURBOWASM_OK) resource->staged = *out;
    return status;
}
static turbowasm_status commit_resource(void *context, turbowasm_component_task *task,
    turbowasm_component_value *values, uint32_t count) {
    resource_context *resource = context; ++resource->commits;
    check_equal(count, 1u); check_not_equal(resource->staged, 0u);
    if (resource->reject_commit) return TURBOWASM_TRAPPED;
    if (resource->staged_borrow) { resource->borrowed = resource->staged; ++task->borrowed_handles; }
    else { values[0].release = NULL; values[0].release_context = NULL; }
    resource->staged = 0; return TURBOWASM_OK;
}
static turbowasm_status rollback_resource(void *context, turbowasm_component_task *task) {
    resource_context *resource = context; turbowasm_status status = TURBOWASM_OK;
    (void)task; ++resource->rollbacks;
    if (resource->staged != 0)
        status = turbowasm_component_resource_drop(resource->table, resource->staged, 42, NULL, NULL);
    resource->staged = 0; return status;
}

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static turbowasm_name name(const char *s) {
    turbowasm_name n = {(const uint8_t *)s, (uint32_t)strlen(s)}; return n;
}
static turbowasm_component_type_ref u32_type(void) {
    return turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32);
}
static uint32_t function_index(const char *s) {
    size_t i;
    for (i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->name.size == strlen(s) && memcmp(e->name.bytes, s, e->name.size) == 0) return e->item_index;
    }
    return UINT32_MAX;
}
static void compiled(unsigned instance, const char *s) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)instances[instance].impl)->jit_functions[function_index(s)].state, TURBOWASM_JIT_COMPILED);
#else
    (void)instance; (void)s;
#endif
}
static turbowasm_status realloc_memory(void *context, uint64_t old_pointer, uint64_t old_size,
    uint64_t alignment, uint64_t new_size, uint64_t *out) {
    (void)context; (void)alignment;
    ++reallocations;
    check_equal(old_pointer, 0u); check_equal(old_size, 0u); check_less_equal(new_size, 128u);
    if (reject_realloc) return TURBOWASM_OUT_OF_MEMORY;
    *out = 256; return TURBOWASM_OK;
}
static turbowasm_status host(void *context, turbowasm_host_call *core_call,
    const turbowasm_value *args, size_t count, turbowasm_value *out, size_t capacity,
    size_t *out_count, turbowasm_trap *trap) {
    uintptr_t operation = (uintptr_t)context;
    turbowasm_component_task_domain *domain = binding.callee_domain;
    turbowasm_status status;
    *out_count = 0; *trap = TURBOWASM_TRAP_NONE;
    if (operation < 4 || operation == 10) {
        uint32_t word;
        turbowasm_component_task *active = domain->active;
        status = turbowasm_component_async_call_create(&call, &binding, args, count);
        if (status == TURBOWASM_OK) status = turbowasm_component_async_call_start(&call, core_call, NULL, &word);
        check_true(domain->active == active);
        if (status != TURBOWASM_OK) return status;
        check_greater_equal(capacity, 1u);
        out[0].kind = TURBOWASM_VALUE_I32; out[0].as.i32 = (int32_t)word; *out_count = 1;
        return TURBOWASM_OK;
    }
    check_not_null(domain->active);
    if (operation == 9) {
        check_equal(count, 1u); check_equal((uint32_t)args[0].as.i32, resources[1].borrowed);
        status = turbowasm_component_resource_drop(&tables[1], resources[1].borrowed, 42, NULL, NULL);
        if (status == TURBOWASM_OK) { resources[1].borrowed = 0; --domain->active->borrowed_handles; }
        return status;
    } else if (operation == 8) {
        turbowasm_host_wait wait = {0}; int completion;
        ++waits;
        return turbowasm_host_call_wait(core_call, 123, &wait, &completion);
    } else {
        const turbowasm_component_task_binding *b = &domain->active->binding;
        const turbowasm_component_type *type = turbowasm_component_type_graph_get(b->graph, b->function_type);
        ++returns;
        return turbowasm_component_task_return_flat(domain, b->graph, type->as.function.has_result,
            type->as.function.result, &b->memory, args, count);
    }
}
static void setup_binding(unsigned type, const char *entry, unsigned caller_wide, unsigned callee_wide) {
    memset(&binding, 0, sizeof(binding));
    binding.caller_graph = &graphs[0]; binding.caller_function_type = type;
    binding.caller_domain = &domains[0]; binding.callee_domain = &domains[1];
    binding.caller_memory.instance = &instances[0]; binding.caller_memory.memory_index = caller_wide;
    binding.caller_memory.pointer_type = caller_wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
    binding.caller_memory.guest_realloc = realloc_memory;
    binding.callee.graph = &graphs[1]; binding.callee.function_type = type;
    binding.callee.instance = &instances[1]; binding.callee.function_index = function_index(entry);
    binding.callee.memory.instance = &instances[1]; binding.callee.memory.memory_index = callee_wide;
    binding.callee.memory.pointer_type = callee_wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
    binding.callee.memory.guest_realloc = realloc_memory;
}
static turbowasm_value integer(bool wide, uint64_t value) {
    turbowasm_value result = {0}; result.kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
    if (wide) result.as.i64 = (int64_t)value; else result.as.i32 = (int32_t)value;
    return result;
}
static turbowasm_status realloc_core(void *context, uint64_t old_pointer, uint64_t old_size,
    uint64_t alignment, uint64_t size, uint64_t *out) {
    unsigned index = (unsigned)(uintptr_t)context;
    const turbowasm_component_canonical_memory *memory = index ? &binding.callee.memory : &binding.caller_memory;
    bool wide = memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64;
    bool was_allowed = may_leave[index];
    turbowasm_value args[] = {integer(wide, old_pointer), integer(wide, old_size), integer(wide, alignment), integer(wide, size)}, result;
    turbowasm_status status; size_t count;
    ++reallocations; may_leave[index] = false;
    status = turbowasm_instance_invoke_internal(instances[index].impl, function_index(wide ? "realloc64" : "realloc32"),
        args, 4, &result, 1, &count, &call.task.trap, turbowasm_execution_control_get(&call.task.core));
    may_leave[index] = was_allowed;
    if (status == TURBOWASM_OK) {
        check_equal(count, 1u); *out = wide ? (uint64_t)result.as.i64 : (uint32_t)result.as.i32;
    }
    return status;
}
static uint32_t invoke(const char *entry, const turbowasm_value *args, size_t count) {
    turbowasm_value output = {0}; size_t actual = 0; turbowasm_trap trap;
    check_equal(turbowasm_instance_invoke(&instances[0], function_index(entry), args, count, &output, 1, &actual, &trap), TURBOWASM_OK);
    check_equal(actual, 1u); compiled(0, entry); return (uint32_t)output.as.i32;
}
static void put_u32(uint64_t address, uint32_t value) {
    turbowasm_component_value v = {0}; v.kind = TURBOWASM_COMPONENT_TYPE_U32; v.as.u32 = value;
    check_equal(turbowasm_component_canonical_lower_value(&graphs[0], u32_type(), &binding.caller_memory, address, &v), TURBOWASM_OK);
}
static uint32_t get_u32(uint64_t address) {
    turbowasm_component_value v = {0}; uint32_t result;
    check_equal(turbowasm_component_canonical_lift_value(&graphs[0], u32_type(), &binding.caller_memory, address, &v), TURBOWASM_OK);
    result = v.as.u32; check_equal(turbowasm_component_value_destroy(&v), TURBOWASM_OK); return result;
}
static void complete_wait(void) {
    turbowasm_host_wait wait;
    check_true(turbowasm_execution_pending_host_wait(&call.task.core, &wait));
    check_equal(turbowasm_execution_complete_host_wait(&call.task.core, wait, 0), TURBOWASM_OK);
}
static uint32_t resource_binding(bool borrow) {
    turbowasm_value rep = integer(false, 77); uint32_t handle;
    setup_binding(borrow ? 10 : 9, borrow ? "borrow" : "wait-scalar", 0, 0);
    binding.caller_memory.resource_lift = lift_resource; binding.caller_memory.resource_lower = lower_resource;
    binding.caller_memory.resource_context = &resources[0];
    binding.callee.memory.resource_lift = lift_resource; binding.callee.memory.resource_lower = lower_resource;
    binding.callee.memory.resource_context = &resources[1];
    binding.parameters.commit = commit_resource; binding.parameters.rollback = rollback_resource;
    binding.parameters.context = &resources[1];
    if (!borrow) {
        binding.result.commit = commit_resource; binding.result.rollback = rollback_resource;
        binding.result.context = &resources[0];
    }
    check_equal(turbowasm_component_resource_new_owned(&tables[0], 42, rep, &handle), TURBOWASM_OK);
    return handle;
}
static void take_event(uint32_t phase) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_waitable_take(&tables[0], call.subtask.waitable.handle, &event), TURBOWASM_OK);
    check_equal(event.payload, phase);
}

spec("async canonical lower to lift calls") {
    before_each() {
        static const char *names[] = {"lower32","lower64","string32","string64","return","returnstr32","returnstr64","unit","wait","drop-borrow","lower4"};
        static const turbowasm_value_kind i32s[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        static const turbowasm_value_kind i64s[] = {TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64};
        const turbowasm_host_function_type types[] = {
            {i32s,2,i32s,1},{i64s,2,i32s,1},{i32s,3,i32s,1},{i64s,3,i32s,1},
            {i32s,1,NULL,0},{i32s,2,NULL,0},{i64s,2,NULL,0},{NULL,0,NULL,0},{NULL,0,NULL,0},{i32s,1,NULL,0},{i32s,5,i32s,1}
        };
        unsigned i, j;
        live = 0; allowance = SIZE_MAX; returns = waits = reallocations = 0; reject_realloc = false;
        resource_drops = 0; memset(resources, 0, sizeof(resources));
        reject_endpoint_commit = false;
        memset(&reader, 0, sizeof(reader)); memset(&writer, 0, sizeof(writer)); memset(codecs, 0, sizeof(codecs));
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        for (i = 0; i < 2; ++i) {
            turbowasm_component_type_ref params[17], string = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING);
            may_leave[i] = true;
            check_true(turbowasm_component_resource_table_init(&tables[i], 32));
            resources[i].table = &tables[i];
            codecs[i].table = &tables[i];
            check_true(turbowasm_component_task_domain_init(&domains[i], &tables[i], &may_leave[i], 4));
            check_true(turbowasm_component_type_graph_allocate(&graphs[i], 14));
            for (j = 0; j < 17; ++j) params[j] = u32_type();
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 0, params, 1, true, u32_type()));
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 1, params, 5, true, u32_type()));
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 2, params, 17, true, u32_type()));
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 3, &string, 1, true, string));
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 4, NULL, 0, false, u32_type()));
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 5, NULL, 0, true, u32_type()));
            for (j = 0; j < 6; ++j) graphs[i].types[j].as.function.is_async = true;
            check_true(turbowasm_component_type_graph_define_resource(&graphs[i], 6, 42));
            check_true(turbowasm_component_type_graph_define_handle(&graphs[i], 7, TURBOWASM_COMPONENT_TYPE_OWN, 6));
            check_true(turbowasm_component_type_graph_define_handle(&graphs[i], 8, TURBOWASM_COMPONENT_TYPE_BORROW, 6));
            params[0] = turbowasm_component_type_ref_indexed(7);
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 9, params, 1, true, params[0]));
            params[0] = turbowasm_component_type_ref_indexed(8);
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 10, params, 1, true, u32_type()));
            graphs[i].types[9].as.function.is_async = graphs[i].types[10].as.function.is_async = true;
            check_true(turbowasm_component_type_graph_define_async_value(&graphs[i], 11, TURBOWASM_COMPONENT_TYPE_FUTURE, true, u32_type()));
            params[0] = turbowasm_component_type_ref_indexed(11);
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 12, params, 1, true, params[0]));
            graphs[i].types[12].as.function.is_async = true;
            params[0] = u32_type();
            check_true(turbowasm_component_type_graph_define_function(&graphs[i], 13, params, 4, true, u32_type()));
            graphs[i].types[13].as.function.is_async = true;
        }
        check_equal(turbowasm_module_load_borrowed_with_config(&module, component_async_call_bytes, sizeof(component_async_call_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_init_with_config(&linker, &config), TURBOWASM_OK);
        for (i = 0; i < 11; ++i)
            check_equal(turbowasm_linker_define_host_function(&linker, name("t"), name(names[i]), &types[i], host, (void *)(uintptr_t)i), TURBOWASM_OK);
        for (i = 0; i < 2; ++i) {
            check_equal(turbowasm_instance_create_linked(&instances[i], &module, &linker), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
            { turbowasm_jit_backend backend = {0};
              check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
              check_equal(turbowasm_jit_instance_attach_backend(instances[i].impl, &backend, 1), TURBOWASM_OK); }
#endif
        }
    }
    after_each() {
        unsigned i; allowance = SIZE_MAX;
        check_equal(turbowasm_component_task_destroy(&parent), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        if (reader.initialized && !reader.closed) check_equal(turbowasm_component_endpoint_close(&reader), TURBOWASM_OK);
        if (writer.initialized && !writer.closed) check_equal(turbowasm_component_endpoint_close(&writer), TURBOWASM_OK);
        for (i = 0; i < 2; ++i) {
            check_equal(turbowasm_component_task_domain_destroy(&domains[i]), TURBOWASM_OK);
            turbowasm_component_resource_table_destroy(&tables[i]);
            turbowasm_component_type_graph_destroy(&graphs[i]); turbowasm_instance_destroy(&instances[i]);
        }
        turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("runs an actual Core lower import and publishes an eager scalar without a handle") {
        turbowasm_value args[] = {integer(false, 42), integer(false, 128)};
        setup_binding(0, "scalar", 0, 0);
        check_equal(invoke("lower32", args, 2), 2u);
        check_equal(get_u32(128), 42u); check_equal(returns, 1u);
        check_equal(tables[0].live_count, 0u); check_null(call.arguments); compiled(1, "scalar");
    }
    it("independently applies the four and sixteen flat parameter limits across memory widths") {
        unsigned source, target, kind, i;
        for (source = 0; source < 2; ++source) for (target = 0; target < 2; ++target) for (kind = 1; kind < 3; ++kind) {
            turbowasm_value args[] = {integer(source != 0, 32), integer(source != 0, 128)};
            const char *entry = kind == 1 ? "sum5" : target ? "tuple64" : "tuple32";
            setup_binding(kind, entry, source, target);
            for (i = 0; i < (kind == 1 ? 5u : 17u); ++i) put_u32(32 + 4u * i, i + 1);
            check_equal(invoke(source ? "lower64" : "lower32", args, 2), 2u);
            check_equal(get_u32(128), kind == 1 ? 15u : 18u);
            check_true(call.caller_signature.params_indirect);
            check_equal(call.task.signature.params_indirect, kind == 2); compiled(1, entry);
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        }
    }
    it("defers source reads under backpressure and snapshots indirect arguments before overlapping results") {
        turbowasm_value args[] = {integer(false, 32), integer(false, 32)};
        uint32_t word; unsigned i;
        setup_binding(1, "sum5", 0, 0);
        check_equal(turbowasm_component_task_backpressure(&domains[1], true), TURBOWASM_OK);
        word = invoke("lower32", args, 2); check_equal(word & 15u, 0u); check_greater(word >> 4, 0u);
        check_null(call.arguments); check_equal(returns, 0u);
        for (i = 0; i < 5; ++i) put_u32(32 + i * 4u, 10);
        check_equal(turbowasm_component_task_backpressure(&domains[1], false), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        check_equal(get_u32(32), 50u); check_not_null(call.arguments);
        check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_TRAPPED);
        take_event(2); check_null(call.arguments);
    }
    it("cancels before start without reading invalid indirect parameter memory") {
        turbowasm_value args[] = {integer(true, UINT64_MAX), integer(true, UINT64_MAX)};
        uint32_t word, phase;
        setup_binding(1, "sum5", 1, 0);
        check_equal(turbowasm_component_task_backpressure(&domains[1], true), TURBOWASM_OK);
        word = invoke("lower64", args, 2);
        check_equal(turbowasm_component_subtask_cancel_begin(&tables[0], word >> 4, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_subtask_cancel_poll(&tables[0], word >> 4, &phase), TURBOWASM_OK);
        check_equal(phase, 3u); check_equal(returns, 0u); check_null(call.arguments);
        check_equal(turbowasm_component_task_backpressure(&domains[1], false), TURBOWASM_OK);
    }
    it("copies strings through both memories and UTF16 encoding before terminal publication") {
        unsigned source, target;
        for (source = 0; source < 2; ++source) for (target = 0; target < 2; ++target) {
            turbowasm_value args[] = {integer(source != 0, 0), integer(source != 0, 5), integer(source != 0, 128)};
            turbowasm_component_value result = {0};
            setup_binding(3, target ? "echo64" : "echo32", source, target);
            binding.callee.memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF16;
            check_equal(invoke(source ? "string64" : "string32", args, 3), 2u);
            check_equal(turbowasm_component_canonical_lift_value(&graphs[0], graphs[0].types[3].as.function.result,
                &binding.caller_memory, 128, &result), TURBOWASM_OK);
            check_equal(result.as.string.size, 5u); check_equal(result.as.string.data, "hello", 5u);
            check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            compiled(1, target ? "echo64" : "echo32");
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        }
        check_equal(reallocations, 8u);
    }
    it("retains lifted arguments until deferred delivery and allows return before Core exit") {
        unsigned early;
        for (early = 0; early < 2; ++early) {
            turbowasm_value args[] = {integer(false, 42), integer(false, 128)};
            uint32_t word;
            setup_binding(0, early ? "scalar-wait" : "wait-scalar", 0, 0);
            put_u32(128, 7); word = invoke("lower32", args, 2);
            check_equal(word & 15u, early ? 2u : 1u);
            check_equal(get_u32(128), early ? 42u : 7u);
            check_equal(call.task.state, TURBOWASM_EXECUTION_YIELDED);
            if (early) check_null(call.arguments); else check_not_null(call.arguments);
            complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
            check_equal(get_u32(128), 42u);
            if (!early) { check_not_null(call.arguments); take_event(2); check_null(call.arguments); }
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        }
        check_equal(waits, 2u); check_equal(returns, 2u);
    }
    it("supports unit calls with no result address or output storage") {
        uint32_t word = 99;
        setup_binding(4, "unit", 1, 0);
        check_equal(turbowasm_component_async_call_create(&call, &binding, NULL, 0), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_OK);
        check_equal(word, 2u); check_equal(returns, 1u);
    }
    it("rejects malformed bindings and raw carriers before task or memory effects") {
        turbowasm_value args[] = {integer(false, 42), integer(false, 128)};
        setup_binding(0, "scalar", 0, 0);
        args[0].kind = TURBOWASM_VALUE_I64;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_TYPE_MISMATCH);
        args[0].kind = TURBOWASM_VALUE_I32;
        binding.caller_memory.pointer_type = TURBOWASM_COMPONENT_POINTER_I64;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_TYPE_MISMATCH);
        binding.caller_memory.pointer_type = TURBOWASM_COMPONENT_POINTER_I32;
        binding.caller_memory.string_encoding = (turbowasm_component_string_encoding)-1;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_INVALID_ARGUMENT);
        binding.caller_memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF8;
        domains[0].may_leave = NULL;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_INVALID_ARGUMENT);
        domains[0].may_leave = &may_leave[0]; may_leave[0] = false;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_TRAPPED);
        may_leave[0] = true; binding.callee.function_type = 1;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_TYPE_MISMATCH);
        check_equal(domains[1].count, 0u); check_null(call.binding.callee_domain); check_equal(returns, 0u);
    }
    it("unwinds a failed result write without publishing success") {
        turbowasm_value args[] = {integer(false, 42), integer(false, 65536)};
        uint32_t word = 99;
        setup_binding(0, "scalar", 0, 0);
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_TRAPPED);
        check_equal(word, 99u); check_equal(returns, 1u);
        check_false(call.subtask.waitable.state.subtask.resolve_delivered);
        check_equal(call.subtask.waitable.failure, TURBOWASM_TRAPPED);
    }
    it("cleans lifted dynamic values when destination realloc fails before entry") {
        turbowasm_value args[] = {integer(false, 0), integer(false, 5), integer(false, 128)};
        uint32_t word = 99;
        setup_binding(3, "echo64", 0, 1); reject_realloc = true;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 3), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_OUT_OF_MEMORY);
        check_equal(returns, 0u); check_equal(word, 99u); check_not_null(call.arguments);
    }
    it("restores the active parent after a nested same-domain initial quantum") {
        turbowasm_component_task_binding b = {0}; turbowasm_component_value result = {0};
        setup_binding(0, "scalar", 0, 0); binding.callee_domain = &domains[0];
        b.graph = &graphs[0]; b.function_type = 5; b.instance = &instances[0]; b.function_index = function_index("nested");
        b.memory = binding.caller_memory;
        check_equal(turbowasm_component_task_create(&parent, &domains[0], &b), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&parent, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&parent, &result), TURBOWASM_OK);
        check_equal(result.as.u32, 42u); check_equal(returns, 2u); check_null(domains[0].active);
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
    }
    it("inherits initial Core fuel and resumes the callee without replaying argument conversion") {
        turbowasm_value args[] = {integer(false, 42), integer(false, 128)};
        turbowasm_execution execution = {0}; turbowasm_execution_options options = {0};
        setup_binding(0, "fuel", 0, 0);
        options.has_fuel_limit = true; options.fuel = 20;
        check_equal(turbowasm_execution_create(&execution, &instances[0], function_index("lower32"), args, 2), TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution, &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_execution_yield_reason_get(&execution), TURBOWASM_YIELD_FUEL);
        check_equal(turbowasm_execution_yield_reason_get(&call.task.core), TURBOWASM_YIELD_FUEL);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        check_equal(get_u32(128), 42u); take_event(2); check_equal(returns, 1u);
        compiled(1, "fuel"); turbowasm_execution_destroy(&execution);
    }
    it("cleans every allocation failure during dynamic argument and result conversion") {
        turbowasm_value args[] = {integer(false, 0), integer(false, 5), integer(false, 128)};
        size_t baseline, limit; bool succeeded = false;
        uint32_t word;
        setup_binding(3, "echo32", 0, 0);
        /* Warm native compilation before measuring per-invocation ownership. */
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 3), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        baseline = live;
        for (limit = 0; limit < 100; ++limit) {
            turbowasm_status status;
            allowance = limit; word = 99;
            status = turbowasm_component_async_call_create(&call, &binding, args, 3);
            if (status == TURBOWASM_OK) status = turbowasm_component_async_call_start(&call, NULL, NULL, &word);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { check_equal(word, 2u); succeeded = true; }
            else { check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(word, 99u); }
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
            check_equal(live, baseline); if (succeeded) break;
        }
        check_true(succeeded); check_greater(limit, 3u);
    }
    it("moves own handles in both directions and destroys the representation exactly once") {
        uint32_t handle = resource_binding(false), output;
        turbowasm_value args[] = {integer(false, handle), integer(false, 128)}, rep;
        check_equal(invoke("lower32", args, 2) & 15u, 1u);
        check_equal(turbowasm_component_resource_rep(&tables[0], handle, 42, &rep), TURBOWASM_TRAPPED);
        check_equal(tables[1].live_count, 1u); check_equal(resources[1].commits, 1u); check_equal(resource_drops, 0u);
        complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        output = get_u32(128);
        check_equal(turbowasm_component_resource_rep(&tables[0], output, 42, &rep), TURBOWASM_OK);
        check_equal(rep.as.i32, 77); check_equal(tables[1].live_count, 0u); check_equal(resources[0].commits, 1u);
        take_event(2); check_equal(resource_drops, 0u);
        check_equal(turbowasm_component_resource_drop(&tables[0], output, 42, drop_resource, NULL), TURBOWASM_OK);
        check_equal(resource_drops, 1u);
    }
    it("keeps a caller lender until terminal delivery after the callee drops its borrow") {
        uint32_t handle = resource_binding(true);
        turbowasm_value args[] = {integer(false, handle), integer(false, 128)};
        check_equal(invoke("lower32", args, 2) & 15u, 1u);
        check_equal(call.task.borrowed_handles, 0u); check_equal(tables[1].live_count, 0u);
        check_equal(turbowasm_component_resource_drop(&tables[0], handle, 42, drop_resource, NULL), TURBOWASM_TRAPPED);
        complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_resource_drop(&tables[0], handle, 42, drop_resource, NULL), TURBOWASM_TRAPPED);
        take_event(2);
        check_equal(turbowasm_component_resource_drop(&tables[0], handle, 42, drop_resource, NULL), TURBOWASM_OK);
        check_equal(resource_drops, 1u);
    }
    it("rolls back staged handles on parameter or result commit failure") {
        unsigned fail_result;
        for (fail_result = 0; fail_result < 2; ++fail_result) {
            uint32_t handle = resource_binding(false), word = 99;
            turbowasm_value args[] = {integer(false, handle), integer(false, 128)};
            resources[0].reject_commit = fail_result != 0; resources[1].reject_commit = fail_result == 0;
            check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_OK);
            if (fail_result) {
                check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_OK);
                complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_TRAPPED);
            } else {
                check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_TRAPPED);
                check_equal(word, 99u);
            }
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
            check_equal(tables[0].live_count, 0u); check_equal(tables[1].live_count, 0u);
            check_equal(resource_drops, fail_result + 1u);
            check_equal(resources[fail_result ? 0 : 1].rollbacks, 1u);
        }
    }
    it("moves a future endpoint through parameter and result codecs without losing its peer") {
        turbowasm_value args[2]; uint32_t original;
        setup_binding(12, "wait-scalar", 0, 0);
        binding.caller_memory.endpoint_lift = binding.callee.memory.endpoint_lift = turbowasm_component_endpoint_codec_lift;
        binding.caller_memory.endpoint_lower = binding.callee.memory.endpoint_lower = turbowasm_component_endpoint_codec_lower;
        binding.caller_memory.endpoint_context = &codecs[0]; binding.callee.memory.endpoint_context = &codecs[1];
        binding.parameters.commit = binding.result.commit = commit_endpoint;
        binding.parameters.rollback = binding.result.rollback = rollback_endpoint;
        binding.parameters.context = &codecs[1]; binding.result.context = &codecs[0];
        check_equal(turbowasm_component_endpoint_pair_open(&graphs[0], 11, &tables[0], NULL, &reader, &writer), TURBOWASM_OK);
        original = reader.waitable.handle; args[0] = integer(false, original); args[1] = integer(false, 128);
        check_equal(invoke("lower32", args, 2) & 15u, 1u);
        check_true(reader.waitable.table == &tables[1]); check_true(reader.peer == &writer);
        check_equal(turbowasm_component_handle_kind_get(&tables[0], original), TURBOWASM_COMPONENT_HANDLE_INVALID);
        complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        check_true(reader.waitable.table == &tables[0]); check_equal(reader.waitable.handle, get_u32(128));
        take_event(2); check_false(reader.closed); check_true(reader.peer == &writer);
        check_null(codecs[0].lower_head); check_null(codecs[1].lower_head);
    }
    it("rolls back a failed endpoint commit and closes the consumed source exactly once") {
        turbowasm_value args[2]; uint32_t word = 99;
        setup_binding(12, "scalar", 0, 0);
        binding.caller_memory.endpoint_lift = binding.callee.memory.endpoint_lift = turbowasm_component_endpoint_codec_lift;
        binding.caller_memory.endpoint_lower = binding.callee.memory.endpoint_lower = turbowasm_component_endpoint_codec_lower;
        binding.caller_memory.endpoint_context = &codecs[0]; binding.callee.memory.endpoint_context = &codecs[1];
        binding.parameters.commit = binding.result.commit = commit_endpoint;
        binding.parameters.rollback = binding.result.rollback = rollback_endpoint;
        binding.parameters.context = &codecs[1]; binding.result.context = &codecs[0];
        check_equal(turbowasm_component_endpoint_pair_open(&graphs[0], 11, &tables[0], NULL, &reader, &writer), TURBOWASM_OK);
        args[0] = integer(false, reader.waitable.handle); args[1] = integer(false, 128); reject_endpoint_commit = true;
        check_equal(turbowasm_component_async_call_create(&call, &binding, args, 2), TURBOWASM_OK);
        check_equal(turbowasm_component_async_call_start(&call, NULL, NULL, &word), TURBOWASM_TRAPPED);
        check_equal(word, 99u); check_equal(returns, 0u); check_null(codecs[1].lower_head);
        check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        check_true(reader.closed); check_equal(tables[0].live_count, 0u); check_equal(tables[1].live_count, 0u);
    }
    it("retains conversion stacks across guest realloc suspension in both directions") {
        unsigned direction;
        for (direction = 0; direction < 2; ++direction) {
            turbowasm_value args[] = {integer(false, 0), integer(false, 5), integer(false, 128)};
            turbowasm_component_canonical_memory *memory;
            setup_binding(3, "echo64", 0, 1);
            memory = direction ? &binding.callee.memory : &binding.caller_memory;
            memory->guest_realloc = realloc_core; memory->realloc_context = (void *)(uintptr_t)direction;
            check_equal(invoke("string32", args, 3) & 15u, 1u);
            check_equal(call.task.phase, TURBOWASM_COMPONENT_TASK_STARTED);
            check_false(may_leave[direction]); check_not_null(call.arguments);
            complete_wait(); check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
            check_true(may_leave[direction]); take_event(2); check_null(call.arguments);
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
        }
        check_equal(reallocations, 4u); check_equal(returns, 2u); check_equal(waits, 2u);
    }
    it("unwinds suspended argument and result realloc before releasing conversion values") {
        unsigned direction;
        for (direction = 0; direction < 2; ++direction) {
            turbowasm_value args[] = {integer(false, 0), integer(false, 5), integer(false, 128)};
            turbowasm_component_canonical_memory *memory;
            setup_binding(3, "echo64", 0, 1);
            memory = direction ? &binding.callee.memory : &binding.caller_memory;
            memory->guest_realloc = realloc_core; memory->realloc_context = (void *)(uintptr_t)direction;
            check_equal(invoke("string32", args, 3) & 15u, 1u); check_false(may_leave[direction]);
            check_equal(turbowasm_component_async_call_destroy(&call), TURBOWASM_OK);
            check_true(may_leave[direction]); check_null(call.arguments);
        }
    }
    it("keeps four direct parameters separate from the fifth result address carrier") {
        turbowasm_value args[] = {integer(false, 1),integer(false, 2),integer(false, 3),integer(false, 4),integer(false, 128)};
        setup_binding(13, "sum4", 0, 0);
        check_equal(invoke("lower4", args, 5), 2u); check_equal(get_u32(128), 10u);
        check_false(call.caller_signature.params_indirect); check_equal(call.caller_signature.param_count, 5u);
        compiled(1, "sum4");
    }
    it("retains an eagerly returned call until its final callback exits") {
        turbowasm_value args[] = {integer(false, 42), integer(false, 128)};
        setup_binding(0, "scalar-yield", 0, 0);
        binding.callee.callback_instance = &instances[1]; binding.callee.callback_index = function_index("callback-exit");
        check_equal(invoke("lower32", args, 2), 2u); check_equal(get_u32(128), 42u);
        check_true(call.task.between_callbacks); check_null(call.arguments);
        check_equal(turbowasm_component_task_resume(&call.task, NULL), TURBOWASM_OK);
        check_equal(returns, 1u); compiled(1, "scalar-yield"); compiled(1, "callback-exit");
    }
}
