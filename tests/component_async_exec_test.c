#include "component_exec.h"
#include "component_endpoint.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_exec.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_task tasks[3];
static turbowasm_component_exec_async_limits limits;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;
static unsigned prepared, completed;
static uint32_t argument;
static turbowasm_component_value value;
static turbowasm_component_endpoint *reader, *writer;
static turbowasm_component_resource_table external;
static turbowasm_component_endpoint host_reader, host_writer;
static turbowasm_component_value input;
static turbowasm_component_buffer input_buffer;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context; if (p != NULL) --live; free(p);
}
static uint32_t function_index(turbowasm_instance *instance, const char *name) {
    const turbowasm_module *module = turbowasm_instance_module(instance);
    uint32_t i;
    for (i = 0; i < turbowasm_module_export_count(module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(module, i);
        if (e->name.size == strlen(name) && memcmp(e->name.bytes, name, e->name.size) == 0) return e->item_index;
    }
    return UINT32_MAX;
}
static void compiled(turbowasm_instance *instance, uint32_t index) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)instance->impl)->jit_functions[index].state, TURBOWASM_JIT_COMPILED);
#else
    (void)instance; (void)index;
#endif
}
static turbowasm_status prepare_number(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *count) {
    (void)context; (void)task; check_greater_equal(capacity, 1u);
    ++prepared; arguments[0].kind = TURBOWASM_VALUE_I32; arguments[0].as.i32 = (int32_t)argument;
    *count = 1; return TURBOWASM_OK;
}
static turbowasm_status prepare_text(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *count) {
    turbowasm_component_value source = {0};
    uint32_t flat_count = 0; turbowasm_status status;
    (void)context; ++prepared;
    /* This immutable test input is borrowed by the lower codec. */
    source.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    source.as.string.data = (uint8_t *)"hello"; source.as.string.size = 5;
    task->context_storage[0] = 77;
    status = turbowasm_component_canonical_lower_flat_value(task->binding.graph,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING), &task->binding.memory,
        &source, arguments, (uint32_t)capacity, &flat_count);
    check_equal(task->context_storage[0], 77u);
    check_null(task->domain->auxiliary); check_true(*task->domain->may_leave);
    if (status == TURBOWASM_OK) { *count = flat_count; ++completed; }
    return status;
}
static turbowasm_status prepare_host_write(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *count) {
    turbowasm_component_event event; turbowasm_status status;
    (void)context; (void)task; (void)arguments; (void)capacity; ++prepared;
    status = turbowasm_component_endpoint_submit(writer, &input_buffer);
    if (status == TURBOWASM_OK) status = turbowasm_component_endpoint_take(writer, &event);
    if (status == TURBOWASM_OK) { *count = 0; ++completed; }
    return status;
}
static bool interrupt_auxiliary(void *context) {
    (void)context; return exec.task_domain.auxiliary != NULL;
}
static void attach_backends(void) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0; i < exec.core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0};
        if (exec.core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl, &backend, 1), TURBOWASM_OK);
    }
#endif
}
static void initialize(void) {
    check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_OK);
    check_true(exec.initialized); check_true(exec.task_domain.table == &exec.resource_table);
    attach_backends();
}
static const turbowasm_component_task_binding *binding(const char *name) {
    const turbowasm_component_task_binding *result = NULL;
    check_equal(turbowasm_component_exec_async_export(&exec, (const uint8_t *)name,
        (uint32_t)strlen(name), &result), TURBOWASM_OK);
    check_not_null(result); return result;
}
static void create(unsigned index, const char *name, turbowasm_component_task_prepare_fn prepare) {
    turbowasm_component_task_binding copy = *binding(name); copy.prepare = prepare;
    check_equal(turbowasm_component_task_create(&tasks[index], &exec.task_domain, &copy), TURBOWASM_OK);
}
static void finish(unsigned index) {
    check_equal(turbowasm_component_task_resume(&tasks[index], NULL), TURBOWASM_OK);
    compiled(tasks[index].binding.instance, tasks[index].binding.function_index);
    check_equal(turbowasm_component_task_take_result(&tasks[index], &value), TURBOWASM_OK);
}
static void clear(unsigned index) {
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
    check_equal(turbowasm_component_task_destroy(&tasks[index]), TURBOWASM_OK);
}
static void make_typed_pair(const char *name) {
    uint64_t pair;
    create(2, name, NULL); finish(2); pair = value.as.u64; clear(2);
    reader = turbowasm_component_endpoint_get(&exec.resource_table, (uint32_t)pair, TURBOWASM_COMPONENT_HANDLE_STREAM_READ);
    writer = turbowasm_component_endpoint_get(&exec.resource_table, (uint32_t)(pair >> 32), TURBOWASM_COMPONENT_HANDLE_STREAM_WRITE);
    check_not_null(reader); check_not_null(writer);
}
static void make_pair(void) { make_typed_pair("new"); }
static void close_pair(void) {
    if (reader != NULL) { check_equal(turbowasm_component_endpoint_close(reader), TURBOWASM_OK); reader = NULL; }
    if (writer != NULL) { check_equal(turbowasm_component_endpoint_close(writer), TURBOWASM_OK); writer = NULL; }
}

spec("Component async binary instantiation") {
    before_each() {
        live = 0; allowance = SIZE_MAX; prepared = completed = 0; argument = 0;
        limits.tasks = 3; limits.handles = 16;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binary, component_async_exec_bytes,
            sizeof(component_async_exec_bytes), &config), TURBOWASM_OK);
    }
    after_each() {
        unsigned i; allowance = SIZE_MAX;
        for (i = 0; i < 3; ++i)
            if (tasks[i].domain != NULL) check_equal(turbowasm_component_task_destroy(&tasks[i]), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
        close_pair();
        check_equal(turbowasm_component_value_destroy(&input), TURBOWASM_OK);
        memset(&input_buffer, 0, sizeof(input_buffer));
        memset(&host_reader, 0, sizeof(host_reader)); memset(&host_writer, 0, sizeof(host_writer));
        turbowasm_component_resource_table_destroy(&external);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binary);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("links actual canonical imports and resolves async exports without opening synchronous admission") {
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        turbowasm_component_exec_call call = {0};
        const turbowasm_component_task_binding *out = NULL;
        check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_UNSUPPORTED);
        initialize();
        check_equal(turbowasm_component_exec_invoke_export(&exec, (const uint8_t *)"answer", 6,
            NULL, 0, &value, &trap), TURBOWASM_UNSUPPORTED);
        check_equal(turbowasm_component_exec_call_create(&call, &exec, (const uint8_t *)"answer", 6,
            NULL, 0), TURBOWASM_UNSUPPORTED);
        check_equal(turbowasm_component_exec_async_export(&exec, (const uint8_t *)"absent", 6, &out), TURBOWASM_INVALID_ARGUMENT);
        check_null(out);
        create(0, "answer", NULL); finish(0); check_equal(value.as.u32, 42u);
        check_equal(exec.task_domain.count, 1u);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_true(exec.initialized); clear(0);
    }
    it("retains linked stackful thread.yield and rejects destruction while suspended") {
        initialize(); create(0, "yield", NULL);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_HOST_WAIT);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        finish(0); check_equal(value.as.u32, 43u); clear(0);
    }
    it("resolves callback options and delivers return or cooperative cancellation") {
        unsigned cancelled; initialize();
        for (cancelled = 0; cancelled < 2; ++cancelled) {
            create(0, "callback", NULL);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
            if (cancelled) check_equal(turbowasm_component_task_request_cancel(&tasks[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
            compiled(tasks[0].binding.callback_instance, tasks[0].binding.callback_index);
            check_equal(tasks[0].phase, cancelled ? TURBOWASM_COMPONENT_TASK_CANCELLED : TURBOWASM_COMPONENT_TASK_RETURNED);
            if (!cancelled) {
                check_equal(turbowasm_component_task_take_result(&tasks[0], &value), TURBOWASM_OK);
                check_equal(value.as.u32, 44u);
            }
            clear(0);
        }
    }
    it("resolves memory32 and memory64 realloc and restores fresh auxiliary context slots") {
        unsigned wide; initialize();
        for (wide = 0; wide < 2; ++wide) {
            const char *name = wide ? "echo64" : "echo32";
            turbowasm_component_exec_realloc_context *realloc_context;
            create(0, name, prepare_text); finish(0);
            check_equal(value.kind, TURBOWASM_COMPONENT_TYPE_STRING);
            check_equal(value.as.string.size, 5u); check_equal(value.as.string.data, "hello", 5);
            realloc_context = tasks[0].binding.memory.realloc_context;
            compiled(realloc_context->instance, realloc_context->function_index);
            clear(0);
        }
        check_equal(prepared, 2u); check_equal(completed, 2u);
    }
    it("retains realloc fuel suspension once and excludes other tasks until context restoration") {
        turbowasm_execution_options options = {0}; unsigned attempts = 0;
        initialize(); create(0, "echo64", prepare_text); create(1, "answer", NULL);
        options.has_fuel_limit = true; options.fuel = 8;
        do {
            check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_YIELDED);
            check_less(++attempts, 1000u);
        } while (exec.task_domain.auxiliary == NULL);
        check_false(exec.may_leave); check_equal(prepared, 1u); check_equal(completed, 0u);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_YIELDED);
        check_equal(tasks[1].state, TURBOWASM_EXECUTION_YIELDED);
        check_equal(tasks[1].phase, TURBOWASM_COMPONENT_TASK_INITIAL);
        finish(0); check_equal(prepared, 1u); check_equal(completed, 1u); clear(0);
        finish(1); check_equal(value.as.u32, 42u); clear(1);
    }
    it("unwinds suspended auxiliary realloc before freeing the exec") {
        turbowasm_execution_options options = {0}; unsigned attempts = 0;
        initialize(); create(0, "echo32", prepare_text); options.has_fuel_limit = true; options.fuel = 8;
        do {
            check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_YIELDED);
            check_less(++attempts, 1000u);
        } while (exec.task_domain.auxiliary == NULL);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_null(exec.task_domain.auxiliary); check_true(exec.may_leave);
        check_equal(prepared, 1u); check_equal(completed, 0u);
    }
    it("inherits interruption in resolved guest realloc and resumes without repeating preparation") {
        turbowasm_execution_options options = {0};
        initialize(); create(0, "echo64", prepare_text); options.should_interrupt = interrupt_auxiliary;
        check_equal(turbowasm_component_task_resume(&tasks[0], &options), TURBOWASM_YIELDED);
        check_equal(turbowasm_execution_yield_reason_get(&tasks[0].core), TURBOWASM_YIELD_INTERRUPTION);
        check_true(exec.task_domain.auxiliary == &tasks[0]); check_equal(prepared, 1u);
        finish(0); check_equal(prepared, 1u); check_equal(completed, 1u);
        check_equal(value.as.string.size, 5u); check_equal(value.as.string.data, "hello", 5); clear(0);
    }
    it("rejects a mismatched builtin import type and missing argument realloc options") {
        uint32_t i; size_t baseline = live;
        for (i = 0; i < binary.async_builtin_count; ++i) {
            turbowasm_component_async_builtin *definition = &binary.async_builtins[i];
            if (definition->kind == TURBOWASM_COMPONENT_TASK_RETURN && !definition->has_memory &&
                definition->result.as.inline_type == TURBOWASM_COMPONENT_TYPE_U32) {
                definition->result = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U64);
                break;
            }
        }
        check_less(i, binary.async_builtin_count);
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_TYPE_MISMATCH);
        check_equal(live, baseline); check_null(exec.binary);
        binary.async_builtins[i].result = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32);
        binary.canon_lifts[3].has_realloc = false;
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_UNSUPPORTED);
        check_equal(live, baseline); check_null(exec.binary);
    }
    it("propagates realloc traps and restores the domain for the next task") {
        unsigned wide; initialize();
        for (wide = 0; wide < 2; ++wide) {
            turbowasm_component_exec_realloc_context *context;
            create(0, wide ? "echo64" : "echo32", prepare_text);
            context = tasks[0].binding.memory.realloc_context;
            context->function_index = function_index(context->instance, wide ? "bad64" : "bad32");
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_TRAPPED);
            check_equal(tasks[0].trap, TURBOWASM_TRAP_UNREACHABLE);
            check_true(exec.may_leave); check_null(exec.task_domain.auxiliary); clear(0);
        }
        create(0, "answer", NULL); finish(0); check_equal(value.as.u32, 42u); clear(0);
    }
    it("connects a suspended stream read to a memory64 writer in the same instantiated Component") {
        initialize(); make_pair(); argument = reader->waitable.handle;
        create(0, "read", prepare_number);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        argument = writer->waitable.handle; create(1, "write", prepare_number); finish(1);
        check_equal(value.as.u32, 46u); clear(1);
        finish(0); check_equal(value.as.u32, 45u); clear(0); close_pair();
    }
    it("bounds live tasks and pairs and preserves instances until endpoint owners close") {
        turbowasm_component_task_binding copy;
        limits.tasks = 1; limits.handles = 2; initialize();
        create(0, "answer", NULL); copy = *binding("answer");
        check_equal(turbowasm_component_task_create(&tasks[1], &exec.task_domain, &copy), TURBOWASM_OUT_OF_MEMORY);
        clear(0); make_pair();
        check_equal(exec.resource_table.live_count, 2u);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        create(0, "new", NULL);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OUT_OF_MEMORY);
        clear(0); close_pair();
    }
    it("retains the same-Component nonnumeric stream rejection at the binary boundary") {
        initialize(); make_typed_pair("text-new"); argument = reader->waitable.handle;
        create(0, "text-read", prepare_number);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        argument = writer->waitable.handle; create(1, "text-write", prepare_number);
        check_equal(turbowasm_component_task_resume(&tasks[1], NULL), TURBOWASM_TRAPPED);
        check_null(exec.task_domain.auxiliary); check_true(exec.may_leave);
        clear(1); clear(0); close_pair();
    }
    it("retains resolved builtin realloc options during a host string rendezvous") {
        turbowasm_execution_options options = {0}; unsigned attempts = 0;
        initialize();
        check_equal(turbowasm_component_endpoint_pair_open(&binary.type_graph, 5,
            &exec.resource_table, NULL, &host_reader, &host_writer), TURBOWASM_OK);
        reader = &host_reader; writer = &host_writer; argument = reader->waitable.handle;
        input.kind = TURBOWASM_COMPONENT_TYPE_STRING; input.as.string.data = turbowasm_rt_malloc(5);
        check_not_null(input.as.string.data); input.as.string.size = 5; memcpy(input.as.string.data, "hello", 5);
        input_buffer.kind = TURBOWASM_COMPONENT_BUFFER_HOST; input_buffer.values = &input; input_buffer.length = 1;
        create(0, "text-read", prepare_number);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        create(1, "answer", prepare_host_write);
        options.has_fuel_limit = true; options.fuel = 8;
        do {
            check_equal(turbowasm_component_task_resume(&tasks[1], &options), TURBOWASM_YIELDED);
            check_less(++attempts, 1000u);
        } while (exec.task_domain.auxiliary == NULL);
        check_true(exec.task_domain.auxiliary == &tasks[1]); check_false(exec.may_leave);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        finish(1); check_equal(value.as.u32, 42u); clear(1);
        finish(0); check_equal(value.kind, TURBOWASM_COMPONENT_TYPE_STRING);
        check_equal(value.as.string.size, 5u); check_equal(value.as.string.data, "hello", 5);
        clear(0); close_pair();
    }
    it("rejects invalid quotas and malformed callback ABI without retaining partial instances") {
        turbowasm_component_exec_async_limits bad = {0,16}; size_t baseline = live;
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &bad), TURBOWASM_INVALID_ARGUMENT);
        bad.tasks = 1; bad.handles = TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS + 1u;
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &bad), TURBOWASM_INVALID_ARGUMENT);
        binary.canon_lifts[2].callback_function_index = binary.canon_lifts[0].core_function_index;
        check_equal(turbowasm_component_exec_init_async(&exec, &binary, &limits), TURBOWASM_TYPE_MISMATCH);
        check_null(exec.binary); check_false(exec.initialized); check_equal(live, baseline);
    }
    it("retains pair storage after a readable endpoint moves to another table") {
        initialize(); make_pair();
        check_true(turbowasm_component_resource_table_init(&external, 2));
        check_equal(turbowasm_component_endpoint_detach_readable(reader), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_attach_readable(reader, &external), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_close(writer), TURBOWASM_OK); writer = NULL;
        check_equal(exec.resource_table.live_count, 0u);
        check_equal(turbowasm_component_exec_destroy(&exec), TURBOWASM_TRAPPED);
        check_true(exec.initialized); check_equal(exec.task_domain.pair_count, 1u);
        close_pair();
    }
    it("rolls back every constructor allocation failure and remains retryable") {
        size_t budget, baseline = live; bool succeeded = false;
        for (budget = 0; budget < 3000; ++budget) {
            turbowasm_status status;
            allowance = budget; status = turbowasm_component_exec_init_async(&exec, &binary, &limits);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { succeeded = true; break; }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(exec.binary); check_null(exec.async_builtins); check_null(exec.async_functions);
            check_false(exec.initialized); check_equal(live, baseline);
        }
        check_true(succeeded); attach_backends();
        create(0, "answer", NULL); finish(0); check_equal(value.as.u32, 42u); clear(0);
    }
}
