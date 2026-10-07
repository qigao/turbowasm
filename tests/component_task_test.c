#include "component_task.h"
#include "component_task_builtin.h"
#include "component_subtask.h"
#include "execution_internal.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_task.h"
#include "fixtures/component_task_builtins.h"
#include "fixtures/component_task_alias.h"
#ifdef TURBOWASM_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif

static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_linker linker;
static turbowasm_component_binary builtin_binary;
enum { BUILTIN_COUNT = 19 };
static turbowasm_component_task_builtin builtins[BUILTIN_COUNT];
static turbowasm_module alias_module;
static turbowasm_instance aliases[2], other_instance;
static turbowasm_linker alias_linker;
static turbowasm_component_type_graph graph;
static turbowasm_component_resource_table table;
static turbowasm_component_task_domain domain;
static turbowasm_component_task_domain caller_domain;
static turbowasm_component_task tasks[3];
static turbowasm_component_waitable_set set;
static turbowasm_component_waitable item;
static turbowasm_component_value result;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;
static bool may_leave, ignore_cancel, callback_exit_only, wait_in_entry, request_in_entry;
static bool mismatch_return, mismatched_encoding;
static bool deliver_on_wait;
static unsigned composite_return;
static unsigned return_memory_override;
static uint32_t entry_word, callback_word, prepared_argument;
static unsigned entered, callbacks, prepared, wait_started, wait_resumed, wait_interrupted;
static turbowasm_component_event last_event;
static turbowasm_component_subtask subtask;
static turbowasm_component_canonical_memory lower_memory;
static turbowasm_component_resource_handle lender;
static uint64_t lower_address;
static unsigned loans, lowered, released;
static bool lower_failure, release_failure, check_release_reentry, check_cancel_pin, prepare_failure;
static const char *prepare_realloc_entry;
static unsigned prepare_realloc_calls, prepare_completed;
static bool prepare_interrupt;
static bool prepare_wrong_kind, prepare_wrong_count;

static turbowasm_status subtask_prepare(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count);
static turbowasm_status lower_result(void *context, turbowasm_component_value *value);
static turbowasm_status release_lender(void *context);

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live;
    return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static turbowasm_name name(const char *s) {
    turbowasm_name n = {(const uint8_t *)s, (uint32_t)strlen(s)}; return n;
}
static uint32_t function_index(const char *s) {
    size_t i;
    for (i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->name.size == strlen(s) && memcmp(e->name.bytes, s, e->name.size) == 0) return e->item_index;
    }
    return UINT32_MAX;
}
static void compiled(const char *s) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)instance.impl)->jit_functions[function_index(s)].state, TURBOWASM_JIT_COMPILED);
#else
    (void)s;
#endif
}
static turbowasm_component_type_ref u32_type(void) {
    return turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32);
}
static turbowasm_status prepare(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count) {
    (void)context;
    ++prepared;
    check_true(task->domain->active == task); check_greater_equal(capacity, 1u);
    arguments[0].kind = TURBOWASM_VALUE_I32; arguments[0].as.i32 = (int32_t)prepared_argument;
    *out_count = task->binding.function_type == 1u ? 1u : 0u;
    if (prepare_wrong_kind) arguments[0].kind = TURBOWASM_VALUE_I64;
    if (prepare_wrong_count) *out_count = 0;
    return TURBOWASM_OK;
}
static turbowasm_status return_value(const turbowasm_value *args, size_t count) {
    turbowasm_component_canonical_memory memory = domain.active->binding.memory;
    turbowasm_component_type_ref type = u32_type();
    if (mismatch_return) type = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_S32);
    if (mismatched_encoding) memory.string_encoding = TURBOWASM_COMPONENT_STRING_UTF16;
    return turbowasm_component_task_return_flat(&domain, &graph, true, type, &memory, args, count);
}
static turbowasm_status wait_now(turbowasm_host_call *call) {
    turbowasm_host_wait wait = {0};
    int completion;
    turbowasm_status status;
    ++wait_started;
    status = turbowasm_host_call_wait(call, 123u, &wait, &completion);
    if (status == TURBOWASM_INTERRUPTED) ++wait_interrupted;
    if (status != TURBOWASM_OK) return status;
    ++wait_resumed; check_equal(completion, 7);
    if (deliver_on_wait) check_true(turbowasm_component_task_deliver_cancel(&domain));
    return TURBOWASM_OK;
}
static turbowasm_status host(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t count, turbowasm_value *out, size_t capacity,
    size_t *out_count, turbowasm_trap *trap) {
    uintptr_t operation = (uintptr_t)context;
    turbowasm_status status = TURBOWASM_OK;
    *out_count = 0; *trap = TURBOWASM_TRAP_NONE;
    check_not_null(domain.active);
    if (operation == 0u) {
        ++entered;
        if (request_in_entry) check_equal(turbowasm_component_task_request_cancel(domain.active), TURBOWASM_OK);
        if (composite_return != 0u) {
            turbowasm_component_task *task = domain.active;
            turbowasm_component_canonical_memory memory = task->binding.memory;
            turbowasm_value values[2] = {0};
            const turbowasm_component_type *type = &graph.types[task->binding.function_type];
            bool wide = task->binding.memory.pointer_type == TURBOWASM_COMPONENT_POINTER_I64;
            values[0].kind = values[1].kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
            if (wide) { values[0].as.i64 = composite_return == 2 ? 16 : 0; values[1].as.i64 = 5; }
            else { values[0].as.i32 = composite_return == 2 ? 16 : 0; values[1].as.i32 = 5; }
            if (return_memory_override == 1u) { memory.instance = &aliases[1]; memory.memory_index = wide ? 0u : 1u; }
            if (return_memory_override == 2u) memory.instance = &other_instance;
            status = turbowasm_component_task_return_flat(&domain, &graph, type->as.function.has_result,
                type->as.function.result, &memory, values,
                composite_return == 1 ? 2 : composite_return == 2 ? 1 : 0);
            if (status != TURBOWASM_OK) return status;
        }
        if (wait_in_entry) { status = wait_now(call); if (status != TURBOWASM_OK) return status; }
        check_greater_equal(capacity, 1u);
        out[0].kind = TURBOWASM_VALUE_I32; out[0].as.i32 = (int32_t)entry_word; *out_count = 1;
    } else if (operation == 1u) {
        ++callbacks; check_equal(count, 3u);
        last_event.code = (turbowasm_component_event_code)args[0].as.i32;
        last_event.handle = (uint32_t)args[1].as.i32; last_event.payload = (uint32_t)args[2].as.i32;
        if (check_cancel_pin) {
            turbowasm_component_event event;
            check_true(subtask.waitable.sync_waiter);
            check_equal(turbowasm_component_waitable_take(&table, subtask.waitable.handle, &event), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_waitable_join(&table, subtask.waitable.handle, 0), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_subtask_drop(&table, subtask.waitable.handle), TURBOWASM_TRAPPED);
        }
        if (!callback_exit_only) {
            if (last_event.code == TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED && !ignore_cancel)
                status = turbowasm_component_task_cancel(&domain);
            else if (!ignore_cancel) {
                turbowasm_value value = {.kind = TURBOWASM_VALUE_I32}; value.as.i32 = 42;
                status = return_value(&value, 1u);
            }
        }
        check_greater_equal(capacity, 1u);
        out[0].kind = TURBOWASM_VALUE_I32; out[0].as.i32 = (int32_t)callback_word; *out_count = 1;
    } else if (operation == 2u) {
        status = return_value(args, count);
    } else if (operation == 3u) {
        status = turbowasm_component_task_cancel(&domain);
    } else {
        status = wait_now(call);
    }
    return status;
}
static turbowasm_component_task_binding binding(const char *entry, const char *callback, bool has_arg) {
    turbowasm_component_task_binding b = {0};
    b.graph = &graph; b.function_type = has_arg ? 1u : 0u;
    b.instance = &instance; b.function_index = function_index(entry);
    if (callback) { b.callback_instance = &instance; b.callback_index = function_index(callback); }
    b.prepare = prepare;
    return b;
}
static void create(unsigned i, const char *entry, const char *callback, bool has_arg) {
    turbowasm_component_task_binding b = binding(entry, callback, has_arg);
    check_equal(turbowasm_component_task_create(&tasks[i], &domain, &b), TURBOWASM_OK);
}
static void register_set(void) {
    check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
    check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_SUBTASK, &item), TURBOWASM_OK);
    check_equal(turbowasm_component_waitable_join(&table, item.handle, set.handle), TURBOWASM_OK);
    entry_word = (set.handle << 4u) | 2u;
}
static void complete_wait(unsigned index) {
    turbowasm_host_wait wait;
    check_true(turbowasm_execution_pending_host_wait(&tasks[index].core, &wait));
    check_equal(turbowasm_execution_complete_host_wait(&tasks[index].core, wait, 7), TURBOWASM_OK);
}

static turbowasm_status subtask_prepare(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count) {
    turbowasm_status status;
    (void)context;
    check_equal(subtask.waitable.state.subtask.phase, TURBOWASM_COMPONENT_SUBTASK_STARTED);
    status = turbowasm_component_resource_lend_acquire(&table, lender, 123);
    if (status != TURBOWASM_OK) return status;
    ++loans;
    if (prepare_failure) return TURBOWASM_OUT_OF_MEMORY;
    return prepare(NULL, task, arguments, capacity, out_count);
}

static turbowasm_status lower_result(void *context, turbowasm_component_value *value) {
    turbowasm_status status;
    (void)context;
    ++lowered;
    check_equal(subtask.waitable.state.subtask.phase, TURBOWASM_COMPONENT_SUBTASK_STARTED);
    check_equal(released, 0u);
    check_equal(turbowasm_component_task_return(&domain, value), TURBOWASM_TRAPPED);
    if (subtask.waitable.handle != 0)
        check_equal(turbowasm_component_subtask_cancel_begin(&table, subtask.waitable.handle, NULL), TURBOWASM_TRAPPED);
    if (lower_failure) return TURBOWASM_OUT_OF_MEMORY;
    status = turbowasm_component_canonical_lower_value(&graph, u32_type(), &lower_memory, lower_address, value);
    if (status != TURBOWASM_OK) return status;
    return turbowasm_component_value_destroy(value);
}

static turbowasm_status release_lender(void *context) {
    turbowasm_status status = TURBOWASM_OK;
    (void)context;
    ++released;
    check_true(subtask.waitable.delivering);
    if (check_release_reentry) {
        turbowasm_component_event event;
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_subtask_drop(&table, subtask.waitable.handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_take(&table, subtask.waitable.handle, &event), TURBOWASM_TRAPPED);
    }
    if (loans != 0) {
        status = turbowasm_component_resource_lend_release(&table, lender, 123);
        --loans;
    }
    return release_failure ? TURBOWASM_TRAPPED : status;
}

static void create_subtask(const char *entry, const char *callback, bool has_arg) {
    turbowasm_component_task_binding b = binding(entry, callback, has_arg);
    turbowasm_value rep = {.kind = TURBOWASM_VALUE_I32}; rep.as.i32 = 9;
    check_equal(turbowasm_component_resource_new_owned(&table, 123, rep, &lender), TURBOWASM_OK);
    b.prepare = subtask_prepare;
    check_equal(turbowasm_component_subtask_create(&subtask, &table, &tasks[0], &domain, &b,
        lower_result, release_lender, NULL), TURBOWASM_OK);
}

static uint32_t publish_subtask(void) {
    uint32_t word;
    check_equal(turbowasm_component_subtask_publish(&subtask, &word), TURBOWASM_OK);
    return word;
}

static void check_lowered_result(void) {
    turbowasm_component_value value = {0};
    check_equal(turbowasm_component_canonical_lift_value(&graph, u32_type(), &lower_memory, lower_address, &value), TURBOWASM_OK);
    check_equal(value.as.u32, 42u);
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
}

static bool interrupt_child(void *context) {
    (void)context;
    return domain.active == &tasks[0];
}

static bool interrupt_prepare(void *context) { (void)context; return prepare_interrupt; }

static turbowasm_status prepare_realloc(void *context, uint64_t old_pointer, uint64_t old_size,
    uint64_t alignment, uint64_t new_size, uint64_t *out_pointer) {
    turbowasm_component_task *task = context;
    turbowasm_value args[4] = {0}, value = {0};
    const uint64_t inputs[] = {old_pointer, old_size, alignment, new_size};
    bool wide = task->binding.memory.pointer_type == TURBOWASM_COMPONENT_POINTER_I64;
    bool was_allowed = *task->domain->may_leave;
    turbowasm_status status;
    size_t i, count = 0;
    ++prepare_realloc_calls;
    for (i = 0; i < 4; ++i) {
        args[i].kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
        if (wide) args[i].as.i64 = (int64_t)inputs[i]; else args[i].as.i32 = (int32_t)inputs[i];
    }
    *task->domain->may_leave = false;
    status = turbowasm_instance_invoke_internal(instance.impl, function_index(prepare_realloc_entry),
        args, 4, &value, 1, &count, &task->trap, turbowasm_execution_control_get(&task->core));
    *task->domain->may_leave = was_allowed;
    if (status != TURBOWASM_OK) return status;
    check_equal(count, 1u);
    *out_pointer = wide ? (uint64_t)value.as.i64 : (uint32_t)value.as.i32;
    return TURBOWASM_OK;
}

static turbowasm_status prepare_string(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count) {
    turbowasm_component_canonical_memory memory = task->binding.memory;
    turbowasm_component_value value = {0};
    turbowasm_status status, cleanup;
    uint32_t count = 0;
    (void)context;
    ++prepared;
    check_true(task->domain->active == task);
    check_equal(task->phase, TURBOWASM_COMPONENT_TASK_STARTED);
    value.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    value.as.string.data = turbowasm_rt_malloc(5);
    if (value.as.string.data == NULL) return TURBOWASM_OUT_OF_MEMORY;
    value.as.string.size = 5; memcpy(value.as.string.data, "hello", 5);
    memory.guest_realloc = prepare_realloc; memory.realloc_context = task;
    status = turbowasm_component_canonical_lower_flat_value(&graph,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING), &memory, &value,
        arguments, (uint32_t)capacity, &count);
    cleanup = turbowasm_component_value_destroy(&value);
    if (status == TURBOWASM_OK) status = cleanup;
    if (status == TURBOWASM_OK) { *out_count = count; ++prepare_completed; }
    return status;
}

static void create_prepared_string(unsigned wide, const char *realloc_entry) {
    turbowasm_component_task_binding b = binding(wide ? "prepared-string64" : "prepared-string32", NULL, false);
    prepare_realloc_entry = realloc_entry;
    b.function_type = 6; b.prepare = prepare_string;
    b.memory.instance = &instance; b.memory.memory_index = wide;
    b.memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
    check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
}

spec("private async Component Core task execution") {
    before_each() {
        static const turbowasm_value_kind i32s[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        static const char *names[] = {"control","callback","return","cancel","wait"};
        static const char *builtin_names[] = {"return","cancel","get0","set0","get1","set1","inc","dec",
            "new","drop","join","yield","wait32","poll32","wait64","poll64","subcancel","subcancel-async","subdrop"};
        const turbowasm_host_function_type types[] = {
            {NULL,0,i32s,1},{i32s,3,i32s,1},{i32s,1,NULL,0},{NULL,0,NULL,0},{NULL,0,NULL,0}
        };
        turbowasm_component_type_ref param = u32_type(), fields[17]; unsigned i;
        live = 0; allowance = SIZE_MAX;
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        may_leave = true; ignore_cancel = callback_exit_only = wait_in_entry = request_in_entry = false;
        mismatch_return = mismatched_encoding = false;
        deliver_on_wait = false;
        composite_return = 0;
        return_memory_override = 0;
        memset(&subtask, 0, sizeof(subtask)); memset(&lower_memory, 0, sizeof(lower_memory));
        lower_memory.instance = &instance; lower_address = 128;
        loans = lowered = released = 0; lender = 0;
        lower_failure = release_failure = check_release_reentry = check_cancel_pin = prepare_failure = false;
        prepare_realloc_entry = NULL; prepare_realloc_calls = prepare_completed = 0; prepare_interrupt = false;
        prepare_wrong_kind = prepare_wrong_count = false;
        memset(builtins, 0, sizeof(builtins));
        entry_word = 1; callback_word = 0; prepared_argument = 42;
        entered = callbacks = prepared = wait_started = wait_resumed = wait_interrupted = 0;
        memset(&last_event, 0, sizeof(last_event)); memset(&set, 0, sizeof(set)); memset(&item, 0, sizeof(item));
        check_true(turbowasm_component_resource_table_init(&table, 32));
        check_true(turbowasm_component_task_domain_init(&domain, &table, &may_leave, 2));
        check_true(turbowasm_component_type_graph_allocate(&graph, 7));
        for (i = 0; i < 2; ++i) {
            check_true(turbowasm_component_type_graph_define_function(&graph, i, &param, i, true, u32_type()));
            graph.types[i].as.function.is_async = true;
        }
        for (i = 0; i < 17; ++i) fields[i] = param;
        check_true(turbowasm_component_type_graph_define_function(&graph, 2, NULL, 0, true,
            turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING)));
        check_true(turbowasm_component_type_graph_define_tuple(&graph, 3, fields, 17));
        check_true(turbowasm_component_type_graph_define_function(&graph, 4, NULL, 0, true,
            turbowasm_component_type_ref_indexed(3)));
        check_true(turbowasm_component_type_graph_define_function(&graph, 5, NULL, 0, false, param));
        graph.types[2].as.function.is_async = graph.types[4].as.function.is_async = graph.types[5].as.function.is_async = true;
        { turbowasm_component_type_ref string = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING);
          check_true(turbowasm_component_type_graph_define_function(&graph, 6, &string, 1, true, u32_type()));
          graph.types[6].as.function.is_async = true; }
        check_equal(turbowasm_module_load_borrowed_with_config(&module, component_task_bytes, sizeof(component_task_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_init_with_config(&linker, &config), TURBOWASM_OK);
        for (i = 0; i < 5; ++i)
            check_equal(turbowasm_linker_define_host_function(&linker, name("t"), name(names[i]), &types[i], host, (void *)(uintptr_t)i), TURBOWASM_OK);
        check_equal(turbowasm_component_binary_decode_async_metadata(&builtin_binary,
            component_task_builtins_bytes, sizeof(component_task_builtins_bytes), &config), TURBOWASM_OK);
        check_equal(builtin_binary.async_builtin_count, (uint32_t)BUILTIN_COUNT);
        for (i = 0; i < BUILTIN_COUNT; ++i) {
            static const turbowasm_value_kind kinds[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_F32,TURBOWASM_VALUE_F64};
            turbowasm_component_flat_signature sig;
            turbowasm_value_kind params[17], results[1];
            turbowasm_host_function_type type;
            unsigned j;
            check_equal(turbowasm_component_async_builtin_signature(&builtin_binary.type_graph,
                &builtin_binary.async_builtins[i], i >= 14 ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32, &sig), TURBOWASM_OK);
            for (j = 0; j < sig.param_count; ++j) params[j] = kinds[sig.params[j]];
            for (j = 0; j < sig.result_count; ++j) results[j] = kinds[sig.results[j]];
            type.params = params; type.param_count = sig.param_count; type.results = results; type.result_count = sig.result_count;
            check_equal(turbowasm_linker_define_host_function(&linker, name("a"), name(builtin_names[i]), &type,
                turbowasm_component_task_builtin_invoke, &builtins[i]), TURBOWASM_OK);
        }
        check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
        for (i = 0; i < BUILTIN_COUNT; ++i) {
            turbowasm_component_canonical_memory memory = {0};
            const turbowasm_component_async_builtin *definition = &builtin_binary.async_builtins[i];
            memory.instance = &instance; memory.memory_index = definition->memory_index;
            memory.pointer_type = memory.memory_index ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            check_equal(turbowasm_component_task_builtin_bind(&builtins[i], &domain, &builtin_binary.type_graph,
                definition, definition->has_memory ? &memory : NULL), TURBOWASM_OK);
        }
#ifdef TURBOWASM_TEST_MIR
        { turbowasm_jit_backend backend = {0};
          check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
          check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1), TURBOWASM_OK); }
#endif
    }
    after_each() {
        unsigned i; allowance = SIZE_MAX;
        for (i = 0; i < 3; ++i) check_equal(turbowasm_component_task_destroy(&tasks[i]), TURBOWASM_OK);
        release_failure = false;
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
        check_equal(loans, 0u);
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&caller_domain), TURBOWASM_OK);
        turbowasm_component_resource_table_destroy(&table);
        turbowasm_component_type_graph_destroy(&graph);
        turbowasm_instance_destroy(&aliases[1]); turbowasm_instance_destroy(&aliases[0]);
        turbowasm_instance_destroy(&other_instance); turbowasm_linker_destroy(&alias_linker); turbowasm_module_destroy(&alias_module);
        turbowasm_instance_destroy(&instance); turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        turbowasm_component_binary_destroy(&builtin_binary);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }

    it("runs stackful Core and lifts task.return independently of the Core result signature") {
        create(0, "stackful", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].state, TURBOWASM_EXECUTION_COMPLETED);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_RETURNED); check_equal(prepared, 1u);
        check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_OK);
        check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_U32); check_equal(result.as.u32, 42u);
        compiled("stackful");
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_TRAPPED);
    }

    it("resumes callbacks with waitable events and pins the set while waiting") {
        register_set(); create(0, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(set.wait_count, 1u); check_null(domain.exclusive); check_equal(entered, 1u);
        check_equal(turbowasm_component_waitable_set_drop(&table, set.handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED); check_equal(callbacks, 0u);
        check_true(turbowasm_component_subtask_start(&item.state.subtask));
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(last_event.code, TURBOWASM_COMPONENT_EVENT_SUBTASK); check_equal(last_event.handle, item.handle);
        check_equal(last_event.payload, TURBOWASM_COMPONENT_SUBTASK_STARTED); check_equal(set.wait_count, 0u);
        check_equal(callbacks, 1u); check_equal(entered, 1u);
        compiled("entry"); compiled("callback");
    }

    it("delivers cancellation before ready events and waits for guest acknowledgement") {
        register_set(); create(0, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_true(turbowasm_component_subtask_start(&item.state.subtask));
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_TRAPPED);
        ignore_cancel = true; callback_word = 1;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(last_event.code, TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED); check_true(item.state.subtask.pending_event);
        check_equal(set.wait_count, 0u); check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_STARTED);
        ignore_cancel = false; callback_word = 0;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(last_event.code, TURBOWASM_COMPONENT_EVENT_NONE); /* cancellation delivered only once */
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_RETURNED); /* result wins */
    }

    it("executes guest cancellation and rejects exit without resolution or invalid callback control") {
        create(0, "entry", "guest-callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_INTERRUPTED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        create(0, "no-return", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        create(0, "invalid-code", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED); check_null(domain.exclusive);
    }

    it("does not lower cancelled arguments while backpressured and bounds live task admission") {
        turbowasm_component_task_binding b = binding("argument", NULL, true);
        check_equal(turbowasm_component_task_backpressure(&domain, true), TURBOWASM_OK);
        create(0, "argument", NULL, true); create(1, "argument", NULL, true);
        check_equal(turbowasm_component_task_create(&tasks[2], &domain, &b), TURBOWASM_OUT_OF_MEMORY);
        check_null(tasks[2].domain);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED); check_equal(prepared, 0u);
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED); check_equal(prepared, 0u);
        check_equal(turbowasm_component_task_backpressure(&domain, false), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK); check_equal(prepared, 1u);
        check_equal(turbowasm_component_task_backpressure(&domain, false), TURBOWASM_TRAPPED);
        domain.backpressure = UINT16_MAX;
        check_equal(turbowasm_component_task_backpressure(&domain, true), TURBOWASM_TRAPPED);
        check_equal(domain.backpressure, UINT16_MAX);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_TRAPPED);
    }

    it("keeps callback exclusivity over host suspension while permitting stackful progress") {
        wait_in_entry = true; create(0, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_true(domain.exclusive == &tasks[0]); check_equal(wait_started, 1u);
        create(1, "stackful", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        create(1, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED); check_equal(entered, 1u);
        complete_wait(0);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_null(domain.exclusive); check_equal(wait_started, 1u); check_equal(wait_resumed, 1u);
        wait_in_entry = false;
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED); check_equal(entered, 2u);
    }

    it("publishes a result before Core exits and unwinds outstanding host waits on destruction") {
        create(0, "return-wait", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_OK);
        check_equal(result.as.u32, 42u); check_equal(tasks[0].state, TURBOWASM_EXECUTION_YIELDED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(wait_started, 1u); check_equal(wait_interrupted, 1u); check_equal(wait_resumed, 0u);
        compiled("return-wait");
        check_null(domain.active); check_null(domain.exclusive); check_equal(domain.count, 0u);
    }

    it("keeps task.return from consuming mismatched types options or outstanding loans") {
        unsigned mode;
        for (mode = 0; mode < 4; ++mode) {
            create(0, "stackful", NULL, false);
            mismatch_return = mode == 0; mismatched_encoding = mode == 1;
            tasks[0].borrowed_handles = mode == 2; may_leave = mode != 3;
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
            check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_STARTED);
            check_equal(tasks[0].result.kind, 0);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        }
    }

    it("continues fuel-limited Core execution without replaying argument preparation") {
        turbowasm_execution_options options = {0}; unsigned i;
        options.has_fuel_limit = true; options.fuel = 20; prepared_argument = 40;
        create(0, "fuel", NULL, true);
        for (i = 0; i < 100; ++i) {
            turbowasm_status status = turbowasm_component_task_resume(&tasks[0], &options);
            if (status == TURBOWASM_OK) break;
            check_equal(status, TURBOWASM_YIELDED);
            check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_FUEL);
        }
        check_less(i, 100u); check_greater(i, 0u); check_equal(prepared, 1u);
        check_equal(tasks[0].result.as.u32, 42u);
        compiled("fuel");
    }

    it("validates entry and callback signatures before admission and cleans failed Core allocation") {
        turbowasm_component_task_binding b = binding("stackful", "callback", false);
        check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_TYPE_MISMATCH);
        b = binding("entry", "entry", false);
        check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_TYPE_MISMATCH);
        check_equal(domain.count, 0u); check_null(tasks[0].domain);
        create(0, "stackful", NULL, false);
        allowance = 0;
        { turbowasm_status status = turbowasm_component_task_resume(&tasks[0], NULL);
          allowance = SIZE_MAX; check_equal(status, TURBOWASM_OUT_OF_MEMORY); }
        check_equal(tasks[0].state, TURBOWASM_EXECUTION_FAILED); check_null(domain.active); check_null(domain.exclusive);
    }

    it("retains dynamic parameter preparation across guest realloc host waits for both memory widths") {
        unsigned wide;
        for (wide = 0; wide < 2; ++wide) {
            create_prepared_string(wide, wide ? "prepare-realloc-wait64" : "prepare-realloc-wait32");
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
            check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_HOST_WAIT);
            check_equal(prepared, 1u); check_equal(prepare_realloc_calls, 1u); check_equal(prepare_completed, 0u);
            check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_STARTED); check_equal(tasks[0].result.kind, 0);
            check_false(may_leave);
            /* Requesting cancellation does not free conversion storage or
             * complete realloc's underlying host operation. */
            check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
            check_equal(prepare_realloc_calls, 1u);
            complete_wait(0);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
            check_equal(tasks[0].result.as.u32, 42u); check_equal(prepare_completed, 1u);
            check_equal(prepared, 1u); check_true(may_leave);
            compiled(wide ? "prepare-realloc-wait64" : "prepare-realloc-wait32");
            compiled(wide ? "prepared-string64" : "prepared-string32");
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            prepared = prepare_realloc_calls = prepare_completed = 0;
        }
    }

    it("shares one fuel and interruption continuation across preparation and the guest entry") {
        unsigned i;
        turbowasm_execution_options options = {0};
        options.has_fuel_limit = true; options.fuel = 8;
        create_prepared_string(0, "prepare-realloc32");
        for (i = 0; i < 200; ++i) {
            turbowasm_status status = turbowasm_component_task_resume(&tasks[0], &options);
            if (status == TURBOWASM_OK) break;
            check_equal(status, TURBOWASM_YIELDED);
            check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_FUEL);
            check_equal(prepared, 1u); check_equal(prepare_realloc_calls, 1u);
        }
        check_greater(i, 0u); check_less(i, 200u); check_equal(prepare_completed, 1u);
        check_equal(tasks[0].result.as.u32, 42u);
        compiled("prepare-realloc32"); compiled("prepared-string32");
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        prepared = prepare_realloc_calls = prepare_completed = 0;
        options.has_fuel_limit = false; options.should_interrupt = interrupt_prepare;
        prepare_interrupt = true;
        create_prepared_string(1, "prepare-realloc64");
        check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_INTERRUPTION);
        check_equal(prepared, 1u); check_equal(prepare_realloc_calls, 1u); check_equal(prepare_completed, 0u);
        prepare_interrupt = false;
        check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_OK);
        check_equal(tasks[0].result.as.u32, 42u); check_equal(prepare_completed, 1u);
        check_equal(prepared, 1u); check_equal(prepare_realloc_calls, 1u);
        compiled("prepare-realloc64"); compiled("prepared-string64");
    }

    it("holds callback exclusivity throughout suspended argument preparation") {
        turbowasm_component_task_binding b = binding("prepared-string-callback32", "callback", false);
        b.function_type = 6; b.prepare = prepare_string; b.memory.instance = &instance;
        prepare_realloc_entry = "prepare-realloc-wait32";
        check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_true(domain.exclusive == &tasks[0]); check_equal(prepare_completed, 0u);
        create(1, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(tasks[1].phase, TURBOWASM_COMPONENT_TASK_INITIAL); check_equal(prepared, 1u);
        complete_wait(0);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].result.as.u32, 42u); check_null(domain.exclusive); check_true(may_leave);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(prepared, 2u); check_equal(entered, 1u);
        compiled("prepared-string-callback32");
    }

    it("rejects malformed prepared carriers before invoking the reserved Core entry") {
        unsigned mode;
        for (mode = 0; mode < 2; ++mode) {
            prepare_wrong_kind = mode == 0; prepare_wrong_count = mode == 1;
            create(0, "argument", NULL, true);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TYPE_MISMATCH);
            check_equal(tasks[0].result.kind, 0); check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_STARTED);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        }
    }

    it("unwinds suspended parameter owners before detaching task state") {
        size_t baseline = live;
        create_prepared_string(0, "prepare-realloc-wait32");
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_greater(live, baseline); check_equal(prepare_completed, 0u);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(wait_interrupted, 1u); check_equal(prepare_completed, 0u); check_true(may_leave);
        check_null(domain.active); check_null(domain.exclusive); check_equal(domain.count, 0u);
#ifndef TURBOWASM_TEST_MIR
        check_equal(live, baseline);
#endif
    }

    it("turns preparation Core exceptions into traps and never enters with partial arguments") {
        unsigned i;
        static const char *entries[] = {"prepare-realloc-trap", "prepare-realloc-throw"};
        for (i = 0; i < 2; ++i) {
            create_prepared_string(0, entries[i]);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
            check_equal(tasks[0].trap, TURBOWASM_TRAP_UNREACHABLE);
            check_equal(tasks[0].result.kind, 0); check_equal(prepare_completed, 0u); check_true(may_leave);
            check_null(((turbowasm_instance_impl *)instance.impl)->pending_exception);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        }
        create(0, "stackful", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].result.as.u32, 42u);
    }

    it("does not consume parameters on Core allocation failure and cleans every failed preparation allocation") {
        unsigned n;
        bool succeeded = false;
        size_t baseline;
#ifdef TURBOWASM_TEST_MIR
        create_prepared_string(0, "prepare-realloc32");
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
#endif
        baseline = live;
        for (n = 0; n < 64; ++n) {
            turbowasm_status status;
            prepared = prepare_realloc_calls = prepare_completed = 0;
            create_prepared_string(0, "prepare-realloc32");
            allowance = n;
            status = turbowasm_component_task_resume(&tasks[0], NULL);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                succeeded = true; check_equal(tasks[0].result.as.u32, 42u);
            } else {
                check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_less_equal(prepare_completed, 1u);
                if (n == 0) { check_equal(prepared, 0u); check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_INITIAL); }
            }
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            check_equal(live, baseline); check_true(may_leave);
            if (succeeded) break;
        }
        check_true(succeeded); check_greater(n, 2u);
    }

    it("lifts owned dynamic and indirect task returns through memory32 and memory64") {
        unsigned wide, mode;
        for (wide = 0; wide < 2; ++wide) for (mode = 1; mode <= 3; ++mode) {
            turbowasm_component_task_binding b = binding("entry", "callback", false);
            b.function_type = mode == 1 ? 2 : mode == 2 ? 4 : 5;
            b.memory.instance = &instance; b.memory.memory_index = wide;
            b.memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            composite_return = mode; entry_word = 0;
            check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_OK);
            if (mode == 1) {
                check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_STRING);
                check_equal(result.as.string.size, 5u); check_equal(result.as.string.data, "hello", 5u);
            } else if (mode == 2) {
                unsigned i;
                check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_TUPLE); check_equal(result.as.tuple.count, 17u);
                for (i = 0; i < 17; ++i) check_equal(result.as.tuple.items[i].as.u32, 0u);
            } else check_equal(result.kind, 0);
            check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        }
    }

    it("lets task.return win pending cancellation without delivering a later cancel event") {
        create(0, "return-yield", "callback", false);
        /* Fuel suspension gives the caller a cancellation point before return. */
        { turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 0;
          check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_YIELDED); }
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_RETURNED);
        callback_exit_only = true;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(last_event.code, TURBOWASM_COMPONENT_EVENT_NONE); check_false(tasks[0].cancellation_delivered);
    }

    it("delivers an already pending cancellation before validating the returned WAIT handle") {
        request_in_entry = true; entry_word = 2; /* WAIT with invalid handle zero */
        create(0, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(last_event.code, TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
    }

    it("turns uncaught Core exceptions from entry and callback into canonical traps") {
        create(0, "throw-entry", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_equal(tasks[0].state, TURBOWASM_EXECUTION_TRAPPED); check_equal(tasks[0].trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        create(0, "entry", "throw-callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_null(domain.exclusive);
    }

    it("requires cancellation delivery for stackful task.cancel and preserves an in-flight host wait") {
        create(0, "cancel-early", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        create(0, "stack-cancel", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(wait_resumed, 0u); check_equal(wait_interrupted, 0u);
        deliver_on_wait = true; complete_wait(0);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED); check_equal(wait_resumed, 1u);
    }

    it("releases all execution and untaken result allocations on every allocation failure") {
        turbowasm_component_task_binding b = binding("entry", "callback", false);
        size_t limit, baseline; bool succeeded = false;
        b.function_type = 2; b.memory.instance = &instance;
        composite_return = 1; entry_word = 0;
#ifdef TURBOWASM_TEST_MIR
        /* The instance intentionally retains compiled artifacts. Establish that
         * stable baseline before sweeping task/continuation/value allocations. */
        check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        compiled("entry");
#endif
        baseline = live;
        for (limit = 0; limit < 100; ++limit) {
            turbowasm_status status;
            check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
            allowance = limit; status = turbowasm_component_task_resume(&tasks[0], NULL); allowance = SIZE_MAX;
            check_true(status == TURBOWASM_OK || status == TURBOWASM_OUT_OF_MEMORY);
            check_null(domain.active); check_null(domain.exclusive);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            check_equal(live, baseline); check_equal(domain.count, 0u);
            if (status == TURBOWASM_OK) { succeeded = true; break; }
        }
        check_true(succeeded); check_greater(limit, 0u);
    }

    it("releases a callback wait pin if allocating its next Core execution fails") {
        turbowasm_status status;
        register_set(); create(0, "entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_true(turbowasm_component_subtask_start(&item.state.subtask));
        allowance = 0; status = turbowasm_component_task_resume(&tasks[0], NULL); allowance = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(set.wait_count, 0u);
        check_false(item.state.subtask.pending_event); check_null(domain.exclusive); check_null(domain.active);
    }

    it("binds decoded context and yield builtins to resumable Core imports") {
        create(0, "builtin-yield", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(tasks[0].builtin_wait, TURBOWASM_COMPONENT_TASK_WAIT_YIELD);
        check_equal(tasks[0].context_storage[0], UINT64_C(0x123456780000002a));
        check_equal(tasks[0].context_storage[1], UINT64_C(0x2345678900000007));
        create(1, "builtin-context-zero", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_equal(tasks[1].result.as.u32, 0u);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].result.as.u32, 49u); check_equal(tasks[0].builtin_wait, TURBOWASM_COMPONENT_TASK_WAIT_NONE);
        compiled("builtin-yield");
    }

    it("waits and polls through decoded memory32 and memory64 builtins without consuming readiness early") {
        unsigned wide, polling;
        register_set();
        for (wide = 0; wide < 2; ++wide) for (polling = 0; polling < 2; ++polling) {
            turbowasm_component_canonical_memory memory = {0};
            turbowasm_component_value payload = {0};
            const char *entry = wide ? (polling ? "builtin-poll64" : "builtin-wait64")
                                     : (polling ? "builtin-poll32" : "builtin-wait32");
            prepared_argument = set.handle; create(0, entry, NULL, true);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), polling ? TURBOWASM_OK : TURBOWASM_YIELDED);
            if (!polling) {
                check_equal(set.wait_count, 1u);
                check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
                if (wide == 0) check_true(turbowasm_component_subtask_start(&item.state.subtask));
                else check_true(turbowasm_component_subtask_resolve(&item.state.subtask, false));
                check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
                check_false(item.state.subtask.pending_event); check_equal(set.wait_count, 0u);
            }
            check_equal(tasks[0].result.as.u32, polling ? 0u : (uint32_t)TURBOWASM_COMPONENT_EVENT_SUBTASK);
            memory.instance = &instance; memory.memory_index = wide;
            memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            check_equal(turbowasm_component_canonical_lift_value(&graph, u32_type(), &memory, 32, &payload), TURBOWASM_OK);
            check_equal(payload.as.u32, polling ? 0u : item.handle);
            check_equal(turbowasm_component_value_destroy(&payload), TURBOWASM_OK);
            compiled(entry);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        }
    }

    it("unwinds builtin wait pins before destroying the task and preserves canonical partial stores") {
        turbowasm_component_canonical_memory memory = {0}; turbowasm_component_value payload = {0};
        register_set(); prepared_argument = set.handle; create(0, "builtin-wait32", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK); check_equal(set.wait_count, 0u);
        check_true(turbowasm_component_subtask_start(&item.state.subtask));
        create(0, "builtin-poll-oob", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_false(item.state.subtask.pending_event);
        memory.instance = &instance;
        check_equal(turbowasm_component_canonical_lift_value(&graph, u32_type(), &memory, 65532, &payload), TURBOWASM_OK);
        check_equal(payload.as.u32, item.handle); /* first store committed before second OOB */
        check_equal(turbowasm_component_value_destroy(&payload), TURBOWASM_OK);
    }

    it("owns guest-created sets and applies backpressure through actual builtins") {
        create(0, "builtin-set-drop", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(table.live_count, 0u); check_null(domain.sets);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_SUBTASK, &item), TURBOWASM_OK);
        prepared_argument = item.handle; create(0, "builtin-join", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(item.set_handle, 0u); check_null(domain.sets);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        create(0, "builtin-pressure", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED); check_equal(domain.backpressure, 1u);
        create(1, "builtin-set-leave", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(tasks[1].phase, TURBOWASM_COMPONENT_TASK_INITIAL);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK); check_equal(domain.backpressure, 0u);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_not_null(domain.sets); /* domain teardown releases untaken empty sets */
    }

    it("executes decoded task.cancel and guards may_leave before side effects") {
        create(0, "entry", "builtin-cancel", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        may_leave = false; create(0, "builtin-set-leave", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
        check_null(domain.sets); check_equal(table.live_count, 0u);
    }

    it("compares canonical memory identity through two imported aliases and rejects a distinct memory") {
        unsigned i, wide;
        check_equal(turbowasm_module_load_borrowed(&alias_module, component_task_alias_bytes, sizeof(component_task_alias_bytes)), TURBOWASM_OK);
        for (i = 0; i < 2; ++i) {
            check_equal(turbowasm_linker_init(&alias_linker), TURBOWASM_OK);
            check_equal(turbowasm_linker_define_instance(&alias_linker, name("p"), i ? &aliases[0] : &instance), TURBOWASM_OK);
            check_equal(turbowasm_instance_create_linked(&aliases[i], &alias_module, &alias_linker), TURBOWASM_OK);
            turbowasm_linker_destroy(&alias_linker);
        }
        check_equal(turbowasm_instance_create_linked(&other_instance, &module, &linker), TURBOWASM_OK);
        composite_return = 1; entry_word = 0;
        for (wide = 0; wide < 2; ++wide) {
            turbowasm_component_task_binding b = binding("entry", "callback", false);
            b.function_type = 2; b.memory.instance = &instance; b.memory.memory_index = wide;
            b.memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            for (i = 1; i <= 2; ++i) {
                return_memory_override = i;
                check_equal(turbowasm_component_task_create(&tasks[0], &domain, &b), TURBOWASM_OK);
                check_equal(turbowasm_component_task_resume(&tasks[0], NULL), i == 1 ? TURBOWASM_OK : TURBOWASM_TRAPPED);
                if (i == 1) check_equal(tasks[0].result.as.string.data, "hello", 5u);
                check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            }
        }
    }

    it("rolls back owned-set allocation failures and keeps nonempty sets alive during teardown") {
        turbowasm_component_resource_handle handle = UINT32_MAX;
        turbowasm_status status; size_t baseline = live;
        allowance = 0; status = turbowasm_component_task_set_new(&domain, &handle); allowance = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(handle, UINT32_MAX);
        check_equal(live, baseline); check_null(domain.sets); check_equal(table.live_count, 0u);
        allowance = 1; status = turbowasm_component_task_set_new(&domain, &handle); allowance = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(handle, UINT32_MAX);
        check_equal(live, baseline); check_null(domain.sets); check_equal(table.live_count, 0u);
        check_equal(turbowasm_component_task_set_new(&domain, &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_SUBTASK, &item), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, item.handle, handle), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_TRAPPED);
        check_not_null(domain.sets); check_equal(item.set_handle, handle);
        check_equal(turbowasm_component_waitable_join(&table, item.handle, 0), TURBOWASM_OK);
        check_equal(turbowasm_component_task_set_drop(&domain, handle), TURBOWASM_OK);
        check_equal(turbowasm_component_task_set_drop(&domain, handle), TURBOWASM_TRAPPED);
        check_null(domain.sets);
    }

    it("writes eager subtask results before releasing loans and detaches while Core is still waiting") {
        unsigned wide;
        for (wide = 0; wide < 2; ++wide) {
            lower_memory.memory_index = wide;
            lower_memory.pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            create_subtask("return-wait", NULL, false);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
            check_null(subtask.callee); check_equal(lowered, 1u); check_equal(loans, 1u);
            check_lowered_result();
            check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_TRAPPED);
            check_equal(publish_subtask(), 2u); check_equal(subtask.waitable.handle, 0u);
            check_equal(released, 1u); check_equal(loans, 0u);
            check_equal(turbowasm_component_task_take_result(&tasks[0], &result), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
            complete_wait(0);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_OK);
            lowered = released = 0;
        }
        compiled("return-wait");
    }

    it("publishes STARTING under backpressure and keeps loans until the terminal event is delivered") {
        turbowasm_component_event event = {0};
        uint32_t word;
        check_equal(turbowasm_component_task_backpressure(&domain, true), TURBOWASM_OK);
        create_subtask("wait-return", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        word = publish_subtask();
        check_equal(word & 15u, 0u); check_equal(word >> 4u, subtask.waitable.handle);
        check_equal(prepared, 0u); check_equal(loans, 0u);
        check_equal(turbowasm_component_subtask_drop(&table, subtask.waitable.handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_backpressure(&domain, false), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_waitable_take(&table, subtask.waitable.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 1u); check_equal(released, 0u); check_equal(loans, 1u);
        complete_wait(0);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_lowered_result();
        check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_TRAPPED);
        check_release_reentry = true;
        check_equal(turbowasm_component_waitable_take(&table, subtask.waitable.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_equal(released, 1u); check_equal(loans, 0u);
        check_equal(turbowasm_component_subtask_drop(&table, subtask.waitable.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_OK);
    }

    it("coalesces progress and does not replay STARTED already returned by publication") {
        turbowasm_component_event event = {0};
        uint32_t word;
        create_subtask("entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        word = publish_subtask(); check_equal(word & 15u, 1u);
        check_equal(turbowasm_component_waitable_take(&table, word >> 4, &event), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, word >> 4, set.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_subtask_cancel_begin(&table, word >> 4, NULL), TURBOWASM_TRAPPED);
        check_false(tasks[0].cancellation_requested);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_poll(&table, set.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_equal(event.code, TURBOWASM_COMPONENT_EVENT_SUBTASK);
        check_equal(turbowasm_component_subtask_drop(&table, word >> 4), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, set.handle), TURBOWASM_OK);
    }

    it("cancels before start without preparing arguments and rejects stale drop handles through Wasm") {
        uint32_t handle;
        create_subtask("stackful", NULL, false); handle = publish_subtask() >> 4;
        prepared_argument = handle; create(1, "builtin-subcancel", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_equal(tasks[1].result.as.u32, 3u); check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(prepared, 1u); /* only the caller, never the cancelled callee */
        check_equal(loans, 0u); check_equal(lowered, 0u); check_equal(released, 1u);
        compiled("builtin-subcancel");
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        create(1, "builtin-subdrop", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        compiled("builtin-subdrop");
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        create(1, "builtin-subdrop", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_TRAPPED);
    }

    it("holds the synchronous cancellation pin across a Core suspension until the callee acknowledges") {
        uint32_t handle;
        turbowasm_component_event event = {0};
        create_subtask("entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        handle = publish_subtask() >> 4;
        prepared_argument = handle; create(1, "builtin-subcancel", NULL, true);
        check_cancel_pin = true;
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_true(subtask.waitable.sync_waiter); check_true(tasks[0].cancellation_requested);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_join(&table, handle, 0), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_subtask_drop(&table, handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(released, 0u); check_equal(loans, 1u);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_equal(tasks[1].result.as.u32, 4u); check_false(subtask.waitable.sync_waiter);
        check_equal(released, 1u); check_equal(loans, 0u);
        check_equal(turbowasm_component_subtask_cancel_begin(&table, handle, NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_subtask_drop(&table, handle), TURBOWASM_OK);
        compiled("builtin-subcancel");
    }

    it("returns async BLOCKED once and lets a pending real I/O completion win cancellation") {
        uint32_t handle;
        turbowasm_component_event event = {0};
        create_subtask("wait-return", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        handle = publish_subtask() >> 4;
        prepared_argument = handle; create(1, "builtin-subcancel-async", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OK);
        check_equal(tasks[1].result.as.u32, UINT32_MAX); check_false(subtask.waitable.sync_waiter);
        check_equal(wait_resumed, 0u); check_equal(loans, 1u); check_equal(released, 0u);
        compiled("builtin-subcancel-async");
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        create(1, "builtin-subcancel-async", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_TRAPPED);
        complete_wait(0); check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_equal(released, 1u); check_lowered_result();
    }

    it("runs an available cancellation callback eagerly under the exclusive subtask pin") {
        uint32_t handle, phase = 99;
        create_subtask("entry", "callback", false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        handle = publish_subtask() >> 4; check_cancel_pin = true;
        check_equal(turbowasm_component_subtask_cancel_begin(&table, handle, NULL), TURBOWASM_OK);
        check_equal(callbacks, 1u); check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(released, 0u); check_true(subtask.waitable.sync_waiter);
        check_equal(turbowasm_component_subtask_cancel_poll(&table, handle, &phase), TURBOWASM_OK);
        check_equal(phase, 4u); check_equal(released, 1u); check_false(subtask.waitable.sync_waiter);
    }

    it("wakes a synchronous canceller on callee failure and releases its pin before owner teardown") {
        uint32_t handle;
        create_subtask("wait-return", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        handle = publish_subtask() >> 4;
        prepared_argument = handle; create(1, "builtin-subcancel", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        lower_failure = true; complete_wait(0);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_OUT_OF_MEMORY);
        check_false(subtask.waitable.sync_waiter); check_equal(released, 0u);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
        check_equal(released, 1u); check_equal(loans, 0u);
    }

    it("coalesces an unobserved STARTED into one terminal subtask notification") {
        uint32_t handle;
        turbowasm_component_event event = {0};
        create_subtask("stackful", NULL, false); handle = publish_subtask() >> 4;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_equal(released, 1u);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_YIELDED);
    }

    it("unwinds a synchronous canceller without releasing the callee loans or stealing its event") {
        uint32_t handle;
        turbowasm_component_event event = {0};
        create_subtask("wait-return", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        handle = publish_subtask() >> 4;
        prepared_argument = handle; create(1, "builtin-subcancel", NULL, true);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        check_false(subtask.waitable.sync_waiter); check_equal(loans, 1u); check_equal(released, 0u);
        complete_wait(0); check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 2u); check_equal(released, 1u);
    }

    it("delivers an already returned result during cancellation without issuing a new request") {
        uint32_t handle, phase = 99;
        create_subtask("stackful", NULL, false); handle = publish_subtask() >> 4;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_subtask_cancel_begin(&table, handle, NULL), TURBOWASM_OK);
        check_false(tasks[0].cancellation_requested); check_false(subtask.waitable.state.subtask.cancellation_requested);
        check_equal(turbowasm_component_subtask_cancel_poll(&table, handle, &phase), TURBOWASM_OK);
        check_equal(phase, 2u); check_equal(released, 1u);
    }

    it("propagates callee traps and result conversion failures without publishing a successful terminal event") {
        static const char *entries[] = {"throw-entry", "no-return", "stackful", "stackful", "stackful"};
        unsigned i;
        for (i = 0; i < 5; ++i) {
            turbowasm_component_event event = {.payload = 99};
            turbowasm_status expected = i == 2 || i == 4 ? TURBOWASM_OUT_OF_MEMORY : TURBOWASM_TRAPPED;
            bool ready = false;
            lower_failure = i == 2; lower_address = i == 3 ? 65536 : 128;
            prepare_failure = i == 4;
            create_subtask(entries[i], NULL, false); (void)publish_subtask();
            check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&table, subtask.waitable.handle, set.handle), TURBOWASM_OK);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), expected);
            check_null(subtask.callee); check_equal(released, 0u); check_equal(loans, 1u);
            check_false(subtask.waitable.state.subtask.resolve_delivered);
            check_equal(turbowasm_component_waitable_set_ready(&table, set.handle, &ready), TURBOWASM_OK); check_true(ready);
            check_equal(turbowasm_component_waitable_set_poll(&table, set.handle, &event), expected);
            check_equal(event.payload, 99u);
            check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
            check_equal(released, 1u); check_equal(loans, 0u);
            check_equal(turbowasm_component_waitable_set_drop(&table, set.handle), TURBOWASM_OK);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_OK);
            released = lowered = 0;
        }
    }

    it("releases failed-owner loans only after unwinding the retained callee host frame") {
        create_subtask("wait-return", NULL, false); (void)publish_subtask();
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_TRAPPED);
        check_equal(released, 0u); check_equal(wait_interrupted, 0u);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(wait_interrupted, 1u); check_null(subtask.callee); check_equal(released, 0u);
        check_equal(subtask.waitable.failure, TURBOWASM_INTERRUPTED);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
        check_equal(released, 1u); check_equal(loans, 0u);
    }

    it("consumes a failing terminal loan cleanup once and preserves the event output") {
        turbowasm_component_event event = {.payload = 99};
        uint32_t handle;
        create_subtask("stackful", NULL, false); handle = publish_subtask() >> 4;
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        release_failure = true; check_release_reentry = true;
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_TRAPPED);
        check_equal(event.payload, 99u); check_equal(released, 1u); check_equal(loans, 0u);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_TRAPPED);
        check_equal(released, 1u);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
        check_equal(released, 1u);
    }

    it("preserves task ownership and packed output when bounded handle publication cannot grow") {
        uint32_t word = 99;
        turbowasm_value rep = {.kind = TURBOWASM_VALUE_I32};
        turbowasm_status status;
        create_subtask("stackful", NULL, false);
        while (table.live_count < table.capacity) {
            turbowasm_component_resource_handle handle;
            check_equal(turbowasm_component_resource_new_owned(&table, 456, rep, &handle), TURBOWASM_OK);
        }
        allowance = 0;
        status = turbowasm_component_subtask_publish(&subtask, &word);
        allowance = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(word, 99u);
        check_false(subtask.published); check_true(subtask.callee == &tasks[0]); check_equal(subtask.waitable.handle, 0u);
        check_equal(turbowasm_component_subtask_publish(&subtask, &word), TURBOWASM_OK);
        check_not_equal(word, 99u);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
        check_equal(released, 1u); check_equal(prepared, 0u);
    }

    it("rejects missing preparation or result adapters and resolves unit calls without either") {
        turbowasm_component_task_binding b = binding("argument", NULL, true);
        b.prepare = NULL;
        check_equal(turbowasm_component_subtask_create(&subtask, &table, &tasks[0], &domain, &b,
            lower_result, release_lender, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_null(subtask.table); check_null(tasks[0].domain); check_equal(domain.count, 0u);
        b.prepare = prepare;
        check_equal(turbowasm_component_subtask_create(&subtask, &table, &tasks[0], &domain, &b,
            NULL, release_lender, NULL), TURBOWASM_INVALID_ARGUMENT);
        b = binding("entry", "callback", false); b.function_type = 5; b.prepare = NULL;
        composite_return = 3; entry_word = 0;
        check_equal(turbowasm_component_subtask_create(&subtask, &table, &tasks[0], &domain, &b,
            NULL, release_lender, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(publish_subtask(), 2u); check_equal(released, 1u); check_equal(lowered, 0u);
    }

    it("preserves packed output on eager cleanup failure without replaying loan release") {
        uint32_t word = 99;
        create_subtask("stackful", NULL, false);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        release_failure = true;
        check_equal(turbowasm_component_subtask_publish(&subtask, &word), TURBOWASM_TRAPPED);
        check_equal(word, 99u); check_equal(released, 1u); check_equal(loans, 0u);
        check_equal(turbowasm_component_subtask_publish(&subtask, &word), TURBOWASM_TRAPPED);
        check_equal(released, 1u); check_equal(subtask.waitable.handle, 0u);
        check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
    }

    it("charges eager cancellation callbacks to the caller fuel and honors its interruption check") {
        unsigned mode, i;
        check_true(turbowasm_component_task_domain_init(&caller_domain, &table, &may_leave, 1));
        /* Separate scheduling domains share this fixture's table and Core
         * instance; the child uses t imports, the caller uses decoded a imports. */
        for (i = 0; i < BUILTIN_COUNT; ++i) builtins[i].domain = &caller_domain;
        for (mode = 0; mode < 3; ++mode) {
            turbowasm_component_task_binding b = binding("builtin-subcancel-async", NULL, true);
            turbowasm_execution_options options = {0};
            turbowasm_component_event event = {0};
            turbowasm_status status;
            uint32_t handle;
            create_subtask("entry", "cancel-fuel", false);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
            handle = publish_subtask() >> 4; prepared_argument = handle;
            check_equal(turbowasm_component_task_create(&tasks[1], &caller_domain, &b), TURBOWASM_OK);
            if (mode == 0) { options.has_fuel_limit = true; options.fuel = 20; }
            else if (mode == 1) options.should_interrupt = interrupt_child;
            status = turbowasm_component_task_resume(&tasks[1], &options);
            if (mode < 2) {
                check_equal(tasks[0].phase, TURBOWASM_COMPONENT_TASK_STARTED);
                check_equal(tasks[0].state, TURBOWASM_EXECUTION_YIELDED);
                check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core),
                    mode == 0 ? TURBOWASM_YIELD_FUEL : TURBOWASM_YIELD_INTERRUPTION);
                if (mode == 0) {
                    check_equal(status, TURBOWASM_YIELDED);
                    check_equal(turbowasm_execution_yield_reason_get(&tasks[1].core), TURBOWASM_YIELD_FUEL);
                    status = turbowasm_component_task_resume(&tasks[1], NULL);
                }
                check_equal(status, TURBOWASM_OK); check_equal(tasks[1].result.as.u32, UINT32_MAX);
                check_equal(released, 0u); check_false(subtask.waitable.sync_waiter);
                check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
                check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK);
                check_equal(event.payload, 4u);
            } else {
                check_equal(status, TURBOWASM_OK); check_equal(tasks[1].result.as.u32, 4u);
                check_equal(tasks[0].state, TURBOWASM_EXECUTION_COMPLETED);
            }
            check_equal(released, 1u); check_equal(loans, 0u);
            check_equal(turbowasm_component_subtask_destroy(&subtask), TURBOWASM_OK);
            check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_resource_drop(&table, lender, 123, NULL, NULL), TURBOWASM_OK);
            released = lowered = 0;
        }
        compiled("cancel-fuel"); compiled("builtin-subcancel-async");
    }

    it("rejects invalid endpoint types and resolved bindings before publishing a host signature") {
        turbowasm_component_task_builtin binding = {0};
        turbowasm_component_async_builtin definition = {0};
        turbowasm_component_canonical_memory memory = {0};
        definition.kind = TURBOWASM_COMPONENT_FUTURE_NEW;
        check_equal(turbowasm_component_task_builtin_bind(&binding, &domain, &graph, &definition, NULL), TURBOWASM_TYPE_MISMATCH);
        check_null(binding.domain);
        definition = builtin_binary.async_builtins[14];
        memory.instance = &instance; memory.memory_index = 1;
        check_equal(turbowasm_component_task_builtin_bind(&binding, &domain, &graph, &definition, &memory), TURBOWASM_TYPE_MISMATCH);
        check_null(binding.domain);
        definition = builtin_binary.async_builtins[2]; definition.context_index = 2;
        check_equal(turbowasm_component_task_builtin_bind(&binding, &domain, &graph, &definition, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_null(binding.domain);
    }
}
