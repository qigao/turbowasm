#include "component_task.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_task.h"
#ifdef TURBOWASM_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif

static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_linker linker;
static turbowasm_component_type_graph graph;
static turbowasm_component_resource_table table;
static turbowasm_component_task_domain domain;
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
static uint32_t entry_word, callback_word, prepared_argument;
static unsigned entered, callbacks, prepared, wait_started, wait_resumed, wait_interrupted;
static turbowasm_component_event last_event;

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
    check_true(domain.active == task); check_greater_equal(capacity, 1u);
    arguments[0].kind = TURBOWASM_VALUE_I32; arguments[0].as.i32 = (int32_t)prepared_argument;
    *out_count = task->binding.function_type == 1u ? 1u : 0u;
    return TURBOWASM_OK;
}
static turbowasm_status return_value(const turbowasm_value *args, size_t count) {
    turbowasm_component_canonical_memory memory = {0};
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
            turbowasm_value values[2] = {0};
            const turbowasm_component_type *type = &graph.types[task->binding.function_type];
            bool wide = task->binding.memory.pointer_type == TURBOWASM_COMPONENT_POINTER_I64;
            values[0].kind = values[1].kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
            if (wide) { values[0].as.i64 = composite_return == 2 ? 16 : 0; values[1].as.i64 = 5; }
            else { values[0].as.i32 = composite_return == 2 ? 16 : 0; values[1].as.i32 = 5; }
            status = turbowasm_component_task_return_flat(&domain, &graph, type->as.function.has_result,
                type->as.function.result, &task->binding.memory, values,
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

spec("private async Component Core task execution") {
    before_each() {
        static const turbowasm_value_kind i32s[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        static const char *names[] = {"control","callback","return","cancel","wait"};
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
        entry_word = 1; callback_word = 0; prepared_argument = 42;
        entered = callbacks = prepared = wait_started = wait_resumed = wait_interrupted = 0;
        memset(&last_event, 0, sizeof(last_event)); memset(&set, 0, sizeof(set)); memset(&item, 0, sizeof(item));
        check_true(turbowasm_component_resource_table_init(&table, 32));
        check_true(turbowasm_component_task_domain_init(&domain, &table, &may_leave, 2));
        check_true(turbowasm_component_type_graph_allocate(&graph, 6));
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
        check_equal(turbowasm_module_load_borrowed_with_config(&module, component_task_bytes, sizeof(component_task_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_init_with_config(&linker, &config), TURBOWASM_OK);
        for (i = 0; i < 5; ++i)
            check_equal(turbowasm_linker_define_host_function(&linker, name("t"), name(names[i]), &types[i], host, (void *)(uintptr_t)i), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
        { turbowasm_jit_backend backend = {0};
          check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
          check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1), TURBOWASM_OK); }
#endif
    }
    after_each() {
        unsigned i; allowance = SIZE_MAX;
        for (i = 0; i < 3; ++i) check_equal(turbowasm_component_task_destroy(&tasks[i]), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_OK);
        turbowasm_component_resource_table_destroy(&table);
        turbowasm_component_type_graph_destroy(&graph);
        turbowasm_instance_destroy(&instance); turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
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
}
