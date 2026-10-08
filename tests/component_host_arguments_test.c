#include "component_api_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_host_composites.h"
#include "fixtures/component_host_resources.h"
#include "instance_internal.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

typedef turbowasm_component_host_value host_value;
enum { OWNER_COUNT = 3, BYTE_LIMIT = 65536, FAILURE_LIMIT = 64 };
static turbowasm_component components[2];
static turbowasm_component_instance instances[2];
static turbowasm_component_instance alternate_instance;
static turbowasm_component_host_arguments owners[OWNER_COUNT];
static turbowasm_component_host_budget budget;
static turbowasm_component_value result;
static host_value resources[3];
static turbowasm_component_exec_resource_codec resource_codec;
static turbowasm_component_canonical_memory resource_memory;
static turbowasm_component_task retained_task;
static turbowasm_component_type_graph task_graph;
static struct { const char *entry; uint32_t handle; unsigned prepared, entered, exited;
    bool wait; } task_context;
static struct { size_t live, attempts, fail_at; } allocations;

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (++allocations.attempts == allocations.fail_at) return NULL;
    pointer = malloc(size);
    if (pointer != NULL) ++allocations.live;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) { check_true(allocations.live != 0u); --allocations.live; }
    free(pointer);
}
static turbowasm_name name_span(const char *name) {
    return (turbowasm_name){(const uint8_t *)name, (uint32_t)strlen(name)};
}
static turbowasm_component_core_call_adapter adapter(unsigned instance, const char *name) {
    turbowasm_component_instance_public_impl *impl =
        turbowasm_component_instance_public_impl_get(&instances[instance]);
    const turbowasm_component_binary *binary = impl->exec.binary;
    uint32_t i;
    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *item = &binary->exports[i];
        if (item->kind == TURBOWASM_COMPONENT_EXTERN_FUNCTION &&
            item->name.size == strlen(name) && memcmp(item->name.bytes, name, item->name.size) == 0)
            return impl->exec.functions[impl->exec.function_adapter_indices[item->item_index]];
    }
    check(false, "missing fixture export");
    return (turbowasm_component_core_call_adapter){0};
}
static turbowasm_status prepare(unsigned owner, unsigned instance, const char *name,
    const host_value *values, size_t count, bool move) {
    turbowasm_component_core_call_adapter binding = adapter(instance, name);
    return turbowasm_component_host_arguments_prepare(&owners[owner],
        turbowasm_component_instance_public_impl_get(&instances[instance]),
        binding.graph, binding.function_type, values, count, move, false, &budget);
}
static void attach(unsigned instance) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_component_exec *exec = &turbowasm_component_instance_public_impl_get(&instances[instance])->exec;
    uint32_t i;
    for (i = 0u; i < exec->core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0};
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(exec->core_instances[i].impl, &backend, 1u), TURBOWASM_OK);
    }
#else
    (void)instance;
#endif
}
static void compiled(turbowasm_component_core_call_adapter binding) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)binding.instance->impl)->jit_functions[binding.function_index].state,
        TURBOWASM_JIT_COMPILED);
#else
    (void)binding;
#endif
}
static void async_instance(uint32_t handles) {
    turbowasm_component_exec_async_limits limits = {4u, handles};
    turbowasm_component_instance_destroy(&instances[1]);
    check_equal(turbowasm_component_instance_create_async_private(&instances[1], &components[1], &limits), TURBOWASM_OK);
    attach(1u);
    turbowasm_component_exec_resource_codec_bind(&resource_codec,
        &turbowasm_component_instance_public_impl_get(&instances[1])->exec, &resource_memory);
}
static turbowasm_status prepare_async(unsigned owner, const char *name,
    const host_value *values, size_t count, bool move) {
    turbowasm_component_core_call_adapter binding = adapter(1u, name);
    return turbowasm_component_host_arguments_prepare(&owners[owner],
        turbowasm_component_instance_public_impl_get(&instances[1]), binding.graph,
        binding.function_type, values, count, move, true, &budget);
}
static turbowasm_status lower_leaf(const turbowasm_component_value *value,
    const char *name, uint32_t *handle) {
    turbowasm_component_core_call_adapter binding = adapter(1u, name);
    const turbowasm_component_type *function = turbowasm_component_type_graph_get(binding.graph, binding.function_type);
    return resource_memory.resource_lower(resource_memory.resource_context, binding.graph,
        function->as.function.params[0], value, handle);
}
static void invoke_raw(const char *name, const uint32_t *handles, size_t count, int32_t expected) {
    turbowasm_component_core_call_adapter binding = adapter(1u, name);
    turbowasm_value arguments[2], output;
    turbowasm_trap trap;
    size_t i, result_count;
    for (i = 0u; i < count; ++i)
        arguments[i] = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = (int32_t)handles[i]};
    check_equal(turbowasm_instance_invoke(binding.instance, binding.function_index,
        arguments, count, &output, 1u, &result_count, &trap), TURBOWASM_OK);
    check_equal(result_count, (size_t)1); check_equal(output.as.i32, expected);
    compiled(binding);
}
static turbowasm_status prepare_task(void *context, turbowasm_component_task *task,
    turbowasm_value *arguments, size_t capacity, size_t *out_count) {
    const turbowasm_component_value *value = turbowasm_component_host_arguments_values(&owners[0], NULL);
    turbowasm_status status;
    (void)context; (void)arguments; (void)capacity;
    ++task_context.prepared; resource_codec.borrow_scope = task;
    status = lower_leaf(value, task_context.entry, &task_context.handle);
    if (status == TURBOWASM_OK) status = turbowasm_component_exec_resource_codec_commit(&resource_codec);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_component_exec_resource_codec_rollback(&resource_codec);
        return status;
    }
    status = turbowasm_component_host_arguments_published(&owners[0]);
    *out_count = 0u;
    return status;
}
static turbowasm_status task_entry(void *context, turbowasm_component_task *task, turbowasm_host_call *call) {
    turbowasm_component_core_call_adapter binding = adapter(1u, task_context.entry);
    turbowasm_value argument = {.kind = TURBOWASM_VALUE_I32, .as.i32 = (int32_t)task_context.handle}, output = {0};
    turbowasm_component_value value = {0};
    turbowasm_status status;
    size_t count;
    (void)context;
    ++task_context.entered;
    status = turbowasm_instance_invoke_from_host(call, binding.instance, binding.function_index,
        &argument, 1u, &output, 1u, &count, &task->trap);
    if (status == TURBOWASM_OK && task_context.wait) {
        turbowasm_host_wait wait;
        int completion;
        status = turbowasm_host_call_wait(call, 71u, &wait, &completion);
    }
    if (status == TURBOWASM_OK && turbowasm_component_task_deliver_cancel(task->domain))
        status = turbowasm_component_task_cancel(task->domain);
    else if (status == TURBOWASM_OK) {
        value.kind = TURBOWASM_COMPONENT_TYPE_S32; value.as.s32 = output.as.i32;
        status = turbowasm_component_task_return(task->domain, &value);
    }
    ++task_context.exited;
    return status;
}
static void create_task(const char *entry, bool wait) {
    const turbowasm_component_value *value = turbowasm_component_host_arguments_values(&owners[0], NULL);
    turbowasm_component_type_ref parameter = turbowasm_component_type_ref_indexed(1u);
    turbowasm_component_task_binding binding = {0};
    check_true(turbowasm_component_type_graph_allocate(&task_graph, 3u));
    check_true(turbowasm_component_type_graph_define_resource(&task_graph, 0u, value[0].resource_identity));
    task_graph.types[0].as.resource.instance_key = value[0].resource_instance_key;
    check_true(turbowasm_component_type_graph_define_handle(&task_graph, 1u, value[0].kind, 0u));
    check_true(turbowasm_component_type_graph_define_function(&task_graph, 2u, &parameter, 1u, true,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_S32)));
    task_graph.types[2].as.function.is_async = true;
    task_context.entry = entry; task_context.wait = wait;
    binding.graph = &task_graph; binding.function_type = 2u; binding.instance = adapter(1u, entry).instance;
    binding.host_entry = task_entry; binding.prepare = prepare_task;
    check_equal(turbowasm_component_task_create(&retained_task, &resource_codec.exec->task_domain, &binding), TURBOWASM_OK);
}
static host_value number(int32_t value) {
    return (host_value){.kind = TURBOWASM_COMPONENT_HOST_S32, .as.s32 = value};
}
static void make(unsigned slot, int32_t rep) {
    host_value value = number(rep);
    size_t count;
    turbowasm_trap trap;
    check_equal(turbowasm_component_instance_invoke(&instances[1], name_span("make"),
        &value, 1u, &resources[slot], 1u, &count, &trap), TURBOWASM_OK);
}
static uint32_t drops(void) {
    host_value value = {0};
    size_t count;
    turbowasm_trap trap;
    check_equal(turbowasm_component_instance_invoke(&instances[1], name_span("drops"),
        NULL, 0u, &value, 1u, &count, &trap), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    if (value.as.u32 != 0u) {
        turbowasm_component_core_call_adapter binding = adapter(1u, "consume");
        const turbowasm_component_type *function = turbowasm_component_type_graph_get(binding.graph, binding.function_type);
        const turbowasm_component_type *handle = turbowasm_component_type_graph_get(binding.graph,
            function->as.function.params[0].as.indexed);
        const turbowasm_component_type *resource = turbowasm_component_resource_definition(binding.graph,
            handle->as.handle.resource_type);
        turbowasm_component_exec *exec = &turbowasm_component_instance_public_impl_get(&instances[1])->exec;
        turbowasm_component_exec_core_function *dtor = &exec->core_functions[resource->as.resource.destructor_index];
        check_equal(((turbowasm_instance_impl *)exec->core_instances[dtor->instance_index].impl)->jit_functions[
            dtor->function_index].state, TURBOWASM_JIT_COMPILED);
    }
#endif
    return value.as.u32;
}
static void published(void *context) {
    check_equal(turbowasm_component_host_arguments_published(context), TURBOWASM_OK);
}
static void invoke_snapshot(unsigned owner, turbowasm_component_core_call_adapter binding) {
    const turbowasm_component_value *values;
    turbowasm_trap trap;
    size_t count;
    values = turbowasm_component_host_arguments_values(&owners[owner], &count);
    binding.admission_commit = published;
    binding.admission_context = &owners[owner];
    check_equal(turbowasm_component_core_call_invoke(&binding, values, count, &result, &trap), TURBOWASM_OK);
    compiled(binding);
}

spec("Deferred Component host argument ownership") {
    before_each() {
        turbowasm_runtime_config config;
        memset(&allocations, 0, sizeof(allocations));
        budget = (turbowasm_component_host_budget){BYTE_LIMIT, 0u};
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_borrowed_with_config(&components[0],
            component_host_composites_bytes, sizeof(component_host_composites_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_load_borrowed_with_config(&components[1],
            component_host_resources_bytes, sizeof(component_host_resources_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create(&instances[0], &components[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create(&instances[1], &components[1]), TURBOWASM_OK);
        attach(0u); attach(1u);
    }
    after_each() {
        unsigned i;
        allocations.fail_at = 0u;
        (void)turbowasm_component_task_destroy(&retained_task);
        turbowasm_component_type_graph_destroy(&task_graph); memset(&task_context, 0, sizeof(task_context));
        if (resource_codec.exec != NULL)
            (void)turbowasm_component_exec_resource_codec_rollback(&resource_codec);
        for (i = 0u; i < OWNER_COUNT; ++i)
            (void)turbowasm_component_host_arguments_destroy(&owners[i]);
        (void)turbowasm_component_value_destroy(&result);
        for (i = 0u; i < 3u; ++i) (void)turbowasm_component_host_value_destroy(&resources[i]);
        turbowasm_component_instance_destroy(&alternate_instance);
        for (i = 0u; i < 2u; ++i) {
            turbowasm_component_instance_destroy(&instances[i]);
            turbowasm_component_destroy(&components[i]);
        }
        memset(&resource_codec, 0, sizeof(resource_codec)); memset(&resource_memory, 0, sizeof(resource_memory));
        check_equal(budget.used, (size_t)0);
        check_equal(allocations.live, (size_t)0);
    }
    it("copies nested result option variant tuple string and flags before deferral") {
        uint8_t bytes[] = {'h', 0xc3, 0xa9, 0, 'x'};
        uint32_t flags = 5u, wide_flags = UINT32_C(0x80000001);
        host_value tuple_items[2] = {number(-42),
            {.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {bytes, sizeof(bytes)}}};
        host_value tuple = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE, .as.tuple = {tuple_items, 2u}};
        host_value variant = {.kind = TURBOWASM_COMPONENT_HOST_VARIANT, .as.variant = {2u, &tuple}};
        host_value option = {.kind = TURBOWASM_COMPONENT_HOST_OPTION, .as.option = {1u, &variant}};
        host_value fields[4] = {
            {.kind = TURBOWASM_COMPONENT_HOST_RESULT, .as.result = {1u, &option}},
            {.kind = TURBOWASM_COMPONENT_HOST_ENUM, .as.enum_index = 2u},
            {.kind = TURBOWASM_COMPONENT_HOST_FLAGS, .as.flags = {&flags, 1u}},
            {.kind = TURBOWASM_COMPONENT_HOST_FLAGS, .as.flags = {&wide_flags, 1u}}};
        host_value nested = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE, .as.tuple = {fields, 4u}};
        host_value list = {.kind = TURBOWASM_COMPONENT_HOST_LIST, .as.list = {&nested, 1u}};
        turbowasm_component_value *out;
        check_equal(prepare(0u, 0u, "echo-nested", &list, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        memset(bytes, 0xff, sizeof(bytes)); flags = wide_flags = 0u;
        memset(tuple_items, 0, sizeof(tuple_items)); memset(fields, 0, sizeof(fields));
        memset(&tuple, 0, sizeof(tuple)); memset(&variant, 0, sizeof(variant));
        memset(&option, 0, sizeof(option)); memset(&nested, 0, sizeof(nested)); memset(&list, 0, sizeof(list));
        invoke_snapshot(0u, adapter(0u, "echo-nested"));
        out = result.as.list.items[0].as.tuple.items;
        check_equal(out[1].as.enum_index, 2u); check_equal(out[2].as.flags, 5u);
        check_equal(out[3].as.flags, UINT32_C(0x80000001));
        out = out[0].as.result.payload->as.option.payload->as.variant.payload->as.tuple.items;
        check_equal(out[0].as.s32, -42);
        check_equal(out[1].as.string.size, (size_t)5);
        check_equal(memcmp(out[1].as.string.data, "h\xc3\xa9\0x", 5u), 0);
    }
    it("copies records and empty strings without retaining caller storage") {
        uint8_t byte = 0xff;
        host_value fields[2] = {{.kind = TURBOWASM_COMPONENT_HOST_U64, .as.u64 = UINT64_MAX},
            {.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {&byte, 0u}}};
        host_value record = {.kind = TURBOWASM_COMPONENT_HOST_RECORD, .as.record = {fields, 2u}};
        check_equal(prepare(0u, 0u, "flat-record", &record, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        memset(fields, 0, sizeof(fields));
        invoke_snapshot(0u, adapter(0u, "flat-record"));
        check_equal(result.as.record.items[0].as.u64, UINT64_MAX);
        check_equal(result.as.record.items[1].as.string.size, (size_t)0);
    }
    it("admits exactly the shared byte limit and makes released capacity reusable") {
        host_value value = {.kind = TURBOWASM_COMPONENT_HOST_ENUM, .as.enum_index = 1u};
        size_t charge, attempts;
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OK);
        charge = budget.used;
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        budget.limit = 2u * charge;
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OK);
        check_equal(prepare(1u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OK);
        check_equal(budget.used, budget.limit);
        attempts = allocations.attempts;
        check_equal(prepare(2u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OUT_OF_MEMORY);
        check_equal(allocations.attempts, attempts); check_null(owners[2].impl);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[1]), TURBOWASM_OK);
        check_equal(prepare(2u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OK);
        check_equal(budget.used, budget.limit);
    }
    it("rejects invalid budgets and overflowing storage before allocation or dereference") {
        host_value value = {.kind = TURBOWASM_COMPONENT_HOST_ENUM, .as.enum_index = 1u};
        host_value list = {.kind = TURBOWASM_COMPONENT_HOST_LIST, .as.list = {&value, SIZE_MAX}};
        size_t attempts = allocations.attempts;
        budget.limit = 0u;
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_INVALID_ARGUMENT);
        budget.limit = SIZE_MAX;
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_INVALID_ARGUMENT);
        budget.limit = SIZE_MAX - 1u;
        check_equal(prepare(0u, 0u, "echo-enum", &list, 1u, false), TURBOWASM_OUT_OF_MEMORY);
        value = (host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {(uint8_t *)&value, SIZE_MAX}};
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OUT_OF_MEMORY);
        budget.used = budget.limit + 1u;
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_INVALID_ARGUMENT);
        budget.used = 0u;
        check_equal(allocations.attempts, attempts);
    }
    it("rejects cyclic input at the nesting boundary without allocating") {
        host_value value = {.kind = TURBOWASM_COMPONENT_HOST_OPTION};
        size_t attempts = allocations.attempts;
        value.as.option.payload = &value;
        check_equal(prepare(0u, 0u, "flat-option", &value, 1u, false), TURBOWASM_TRAPPED);
        check_equal(allocations.attempts, attempts);
    }
    it("bounds empty admissions and rejects invalid state without replacing a live owner") {
        size_t charge, count = 99u, attempts;
        void *saved;
        check_equal(prepare(0u, 1u, "drops", NULL, 0u, false), TURBOWASM_OK);
        charge = budget.used; saved = owners[0].impl;
        check_greater(charge, (size_t)0);
        attempts = allocations.attempts;
        check_equal(prepare(0u, 1u, "drops", NULL, 0u, false), TURBOWASM_INVALID_ARGUMENT);
        check_true(owners[0].impl == saved); check_equal(budget.used, charge);
        check_equal(allocations.attempts, attempts);
        check_null(turbowasm_component_host_arguments_values(&owners[0], &count));
        check_equal(count, (size_t)0);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        invoke_snapshot(0u, adapter(1u, "drops"));
        check_equal(result.as.u32, 0u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        budget.limit = charge - 1u;
        check_equal(prepare(0u, 1u, "drops", NULL, 0u, false), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
    }
    it("keeps the defining instance alive after its public handles are closed") {
        host_value value = {.kind = TURBOWASM_COMPONENT_HOST_ENUM, .as.enum_index = 2u};
        turbowasm_component_core_call_adapter binding = adapter(0u, "flat-enum");
        check_equal(prepare(0u, 0u, "flat-enum", &value, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instances[0]); turbowasm_component_destroy(&components[0]);
        invoke_snapshot(0u, binding);
        check_equal(result.as.enum_index, 2u);
    }
    it("unreserves own inputs on abort and consumes them only at commit") {
        make(0u, 42);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, false), TURBOWASM_INVALID_ARGUMENT);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(resources[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 0u);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        check_equal((int)resources[0].kind, 0);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("preserves own on byte exhaustion and on later invalid argument validation") {
        uint8_t invalid = 0xff;
        host_value arguments[2];
        size_t live;
        make(0u, 42); live = allocations.live;
        budget.limit = 1u;
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OUT_OF_MEMORY);
        budget.limit = BYTE_LIMIT;
        arguments[0] = resources[0];
        arguments[1] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {&invalid, 1u}};
        check_equal(prepare(0u, 1u, "text", arguments, 2u, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(arguments[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(budget.used, (size_t)0); check_equal(allocations.live, live);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("rejects repeated own and own-borrow aliases without leaving reservations") {
        host_value arguments[2];
        make(0u, 42);
        arguments[0] = arguments[1] = resources[0];
        check_equal(prepare(0u, 1u, "two", arguments, 2u, true), TURBOWASM_INVALID_ARGUMENT);
        memset(&arguments[1], 0, sizeof(arguments[1]));
        check_equal(turbowasm_component_host_value_borrow(&resources[0], &arguments[1]), TURBOWASM_OK);
        check_equal(prepare(0u, 1u, "own-and-borrow", arguments, 2u, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("holds borrow loans after canonical publication until owner delivery") {
        host_value borrowed = {0};
        make(0u, 42);
        check_equal(turbowasm_component_host_value_borrow(&resources[0], &borrowed), TURBOWASM_OK);
        check_equal(prepare(0u, 1u, "borrow", &borrowed, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        memset(&borrowed, 0, sizeof(borrowed));
        invoke_snapshot(0u, adapter(1u, "borrow"));
        check_equal(result.as.s32, 42);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("hands own to actual guest execution without a second destructor") {
        make(0u, 42);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        invoke_snapshot(0u, adapter(1u, "consume"));
        check_equal(result.as.s32, 42); check_equal(drops(), 1u);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("reports the first destructor failure and cleans all committed resources") {
        make(0u, -1); make(1u, 42);
        check_equal(prepare(0u, 1u, "two", resources, 2u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_null(owners[0].impl); check_equal(budget.used, (size_t)0);
        check_equal(drops(), 2u);
    }
    it("retains resource destruction authority after both public handles close") {
        size_t live;
        make(0u, 42);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instances[1]); turbowasm_component_destroy(&components[1]);
        live = allocations.live;
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_less(allocations.live, live);
        check_equal(budget.used, (size_t)0);
    }
    it("accounts actual copied string bytes at the shared reservation boundary") {
        uint8_t bytes[] = "retained";
        host_value fields[2] = {number(42), {.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {bytes, sizeof(bytes)}}};
        host_value tuple = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE, .as.tuple = {fields, 2u}};
        size_t charge, attempts;
        check_equal(prepare(0u, 0u, "flat-tuple", &tuple, 1u, false), TURBOWASM_OK);
        charge = budget.used;
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        fields[1].as.string.size = 0u;
        check_equal(prepare(0u, 0u, "flat-tuple", &tuple, 1u, false), TURBOWASM_OK);
        check_equal(budget.used + sizeof(bytes), charge);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        fields[1].as.string.size = sizeof(bytes); budget.limit = charge - 1u;
        attempts = allocations.attempts;
        check_equal(prepare(0u, 0u, "flat-tuple", &tuple, 1u, false), TURBOWASM_OUT_OF_MEMORY);
        check_equal(allocations.attempts, attempts);
        budget.limit = charge;
        check_equal(prepare(0u, 0u, "flat-tuple", &tuple, 1u, false), TURBOWASM_OK);
        check_equal(budget.used, charge);
    }
    it("rolls back every snapshot allocation failure before consuming resources") {
        size_t failure;
        bool completed = false;
        uint8_t bytes[] = "retained";
        host_value arguments[2];
        make(0u, 42);
        arguments[0] = resources[0];
        arguments[1] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {bytes, sizeof(bytes)}};
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            size_t live = allocations.live;
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = prepare(0u, 1u, "text", arguments, 2u, true);
            allocations.fail_at = 0u;
            check_equal(arguments[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
                completed = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(budget.used, (size_t)0); check_equal(allocations.live, live);
            if (completed) break;
        }
        check_true(completed); check_greater(failure, (size_t)4);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
    }
    it("rolls back every nested copy allocation and leaves source bytes untouched") {
        uint8_t bytes[] = "nested";
        host_value fields[2] = {number(42), {.kind = TURBOWASM_COMPONENT_HOST_STRING,
            .as.string = {bytes, sizeof(bytes)}}};
        host_value tuple = {.kind = TURBOWASM_COMPONENT_HOST_TUPLE, .as.tuple = {fields, 2u}};
        host_value variant = {.kind = TURBOWASM_COMPONENT_HOST_VARIANT, .as.variant = {2u, &tuple}};
        host_value option = {.kind = TURBOWASM_COMPONENT_HOST_OPTION, .as.option = {1u, &variant}};
        size_t failure;
        bool completed = false;
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            size_t live = allocations.live;
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = prepare(0u, 0u, "flat-option", &option, 1u, false);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
                completed = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(memcmp(bytes, "nested", sizeof(bytes)), 0);
            check_equal(budget.used, (size_t)0); check_equal(allocations.live, live);
            if (completed) break;
        }
        check_true(completed); check_greater(failure, (size_t)5);
    }
    it("rejects async resource admission without an initialized async domain") {
        make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(resources[0].kind, TURBOWASM_COMPONENT_HOST_OWN); check_equal(budget.used, (size_t)0);
    }
    it("keeps adopted own reserved before commit and releases it on cancellation") {
        const turbowasm_component_value *value;
        uint32_t handle = 99u;
        size_t count;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], &count);
        check_equal(count, (size_t)1); check_not_null(value[0].resource_instance_key);
        check_equal(resource_codec.exec->async_resource_owners, 1u);
        check_equal(lower_leaf(value, "consume", &handle), TURBOWASM_INVALID_ARGUMENT);
        check_equal(handle, 99u); check_null(resource_codec.lower_head);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 0u);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        check_equal((int)resources[0].kind, 0);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 1u);
    }
    it("publishes adopted own through the existing codec and consumes it in the real guest") {
        const turbowasm_component_value *value;
        uint32_t handle;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], NULL);
        check_equal(lower_leaf(value, "consume", &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_public_impl_get(&instances[1])->resource_count, 1u);
        check_equal(turbowasm_component_exec_resource_codec_commit(&resource_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_public_impl_get(&instances[1])->resource_count, 0u);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_OK);
        invoke_raw("consume", &handle, 1u, 42);
        check_equal(drops(), 1u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u); check_equal(resource_codec.exec->async_resource_owners, 0u);
    }
    it("returns the published resource as a fresh canonical result without a second host owner") {
        turbowasm_component_core_call_adapter binding;
        const turbowasm_component_type *function;
        const turbowasm_component_value *value;
        uint32_t handle;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "echo", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], NULL);
        check_equal(lower_leaf(value, "echo", &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_resource_codec_commit(&resource_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_public_impl_get(&instances[1])->resource_count, 0u);
        invoke_raw("echo", &handle, 1u, (int32_t)handle);
        binding = adapter(1u, "echo");
        function = turbowasm_component_type_graph_get(binding.graph, binding.function_type);
        check_equal(resource_memory.resource_lift(resource_memory.resource_context,
            binding.graph, function->as.function.result, handle, &result), TURBOWASM_OK);
        check_equal(result.as.resource_rep.as.i32, 42);
        check_equal(resource_codec.exec->async_resource_owners, 2u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 1u); check_equal(drops(), 0u);
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 1u);
    }
    it("rejects destruction without mutation while an adopted lower reservation is live") {
        const turbowasm_component_value *value;
        size_t charge;
        void *saved;
        uint32_t handle;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], NULL);
        check_equal(lower_leaf(value, "consume", &handle), TURBOWASM_OK);
        charge = budget.used; saved = owners[0].impl;
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_INVALID_ARGUMENT);
        check_true(owners[0].impl == saved); check_equal(budget.used, charge);
        check_equal(resource_codec.exec->async_resource_owners, 1u);
        check_equal(resource_codec.exec->resource_table.live_count, 1u); check_equal(drops(), 0u);
        check_equal(turbowasm_component_exec_resource_codec_rollback(&resource_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 1u);
    }
    it("retries rolled-back adopted publication and retains borrow loans through delivery") {
        const turbowasm_component_value *value;
        host_value borrowed = {0};
        uint32_t handle;
        async_instance(8u); make(0u, 42);
        check_equal(turbowasm_component_host_value_borrow(&resources[0], &borrowed), TURBOWASM_OK);
        check_equal(prepare_async(0u, "borrow", &borrowed, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], NULL);
        check_equal(lower_leaf(value, "borrow", &handle), TURBOWASM_OK);
        check_equal(handle, 42u); check_equal(resource_codec.exec->resource_table.live_count, 0u);
        check_equal(turbowasm_component_exec_resource_codec_rollback(&resource_codec), TURBOWASM_OK);
        check_equal(lower_leaf(value, "borrow", &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_resource_codec_commit(&resource_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_published(&owners[0]), TURBOWASM_OK);
        invoke_raw("borrow", &handle, 1u, 42);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("bounds adopted owners across admissions and reuses quota after delivery") {
        host_value borrowed = {0};
        size_t charge;
        async_instance(1u); make(0u, 42);
        check_equal(turbowasm_component_host_value_borrow(&resources[0], &borrowed), TURBOWASM_OK);
        check_equal(prepare_async(0u, "borrow", &borrowed, 1u, false), TURBOWASM_OK);
        charge = budget.used;
        check_equal(prepare_async(1u, "borrow", &borrowed, 1u, false), TURBOWASM_OUT_OF_MEMORY);
        check_equal(budget.used, charge); check_equal(resource_codec.exec->async_resource_owners, 1u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(prepare_async(1u, "borrow", &borrowed, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
    }
    it("preflights adoption bytes before moving own and returns capacity after abort") {
        size_t charge, ordinary, attempts;
        async_instance(8u); make(0u, 42);
        check_equal(prepare(0u, 1u, "consume", resources, 1u, true), TURBOWASM_OK);
        ordinary = budget.used;
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        charge = budget.used;
        check_equal(charge - ordinary, turbowasm_component_exec_resource_adopt_size());
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        budget.limit = charge - 1u; attempts = allocations.attempts;
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OUT_OF_MEMORY);
        check_equal(allocations.attempts, attempts); check_equal(resources[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
        budget.limit = charge;
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(budget.used, charge);
    }
    it("cleans all adopted composite leaves after a destructor failure") {
        host_value list;
        async_instance(8u); make(0u, -1); make(1u, 42);
        list = (host_value){.kind = TURBOWASM_COMPONENT_HOST_LIST, .as.list = {resources, 2u}};
        check_equal(prepare_async(0u, "echo-list", &list, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        check_equal((int)resources[0].kind, 0); check_equal((int)resources[1].kind, 0);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_TRAPPED);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 2u);
    }
    it("rolls back every adoption allocation including a partially adopted own pair") {
        size_t failure;
        bool completed = false;
        async_instance(8u); make(0u, 42); make(1u, 43);
        for (failure = 1u; failure < FAILURE_LIMIT; ++failure) {
            size_t live = allocations.live;
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = prepare_async(0u, "two", resources, 2u, true);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
                completed = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(resources[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
            check_equal(resources[1].kind, TURBOWASM_COMPONENT_HOST_OWN);
            check_equal(resource_codec.exec->async_resource_owners, 0u);
            check_equal(allocations.live, live); check_equal(budget.used, (size_t)0);
            if (completed) break;
        }
        check_true(completed); check_greater(failure, (size_t)5); check_equal(drops(), 0u);
    }
    it("cancels an adopted own task behind backpressure before preparing or entering the guest") {
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        create_task("consume", false);
        check_equal(turbowasm_component_task_backpressure(retained_task.domain, true), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_YIELDED);
        check_equal(task_context.prepared, 0u); check_equal(task_context.entered, 0u);
        check_equal(turbowasm_component_task_request_cancel(&retained_task), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_OK);
        check_equal(retained_task.phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(turbowasm_component_task_backpressure(retained_task.domain, false), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&retained_task), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(task_context.prepared, 0u); check_equal(task_context.entered, 0u); check_equal(drops(), 1u);
    }
    it("holds adopted publication across a real host wait and acknowledges cancellation after completion") {
        turbowasm_host_wait wait;
        size_t charge;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        create_task("consume", true);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_YIELDED);
        check_equal(task_context.prepared, 1u); check_equal(task_context.entered, 1u); check_equal(task_context.exited, 0u);
        check_equal(drops(), 1u); check_equal(resource_codec.exec->async_resource_owners, 1u);
        charge = budget.used;
        check_equal(turbowasm_component_task_request_cancel(&retained_task), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_YIELDED);
        check_equal(budget.used, charge); check_equal(task_context.exited, 0u);
        check_true(turbowasm_execution_pending_host_wait(&retained_task.core, &wait));
        check_equal(turbowasm_execution_complete_host_wait(&retained_task.core, wait, 0), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_OK);
        check_equal(retained_task.phase, TURBOWASM_COMPONENT_TASK_CANCELLED);
        check_equal(task_context.exited, 1u); compiled(adapter(1u, "consume"));
        check_equal(turbowasm_component_task_destroy(&retained_task), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 1u);
    }
    it("unwinds a retained task before releasing adopted host borrow loans") {
        host_value borrowed = {0};
        async_instance(8u); make(0u, 42);
        check_equal(turbowasm_component_host_value_borrow(&resources[0], &borrowed), TURBOWASM_OK);
        check_equal(prepare_async(0u, "borrow", &borrowed, 1u, false), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        create_task("borrow", true);
        check_equal(turbowasm_component_task_resume(&retained_task, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_task_destroy(&retained_task), TURBOWASM_OK);
        check_equal(task_context.exited, 1u); compiled(adapter(1u, "borrow"));
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(drops(), 1u);
    }
    it("rejects lowering a host resource into another instance of the same Component") {
        turbowasm_component_exec_async_limits limits = {4u, 8u};
        turbowasm_component_exec_resource_codec other_codec = {0};
        turbowasm_component_canonical_memory memory = {0};
        turbowasm_component_instance_public_impl *other;
        turbowasm_component_core_call_adapter binding;
        const turbowasm_component_type *function;
        const turbowasm_component_value *value;
        uint32_t handle = 99u;
        async_instance(8u); make(0u, 42);
        check_equal(prepare_async(0u, "consume", resources, 1u, true), TURBOWASM_OK);
        check_equal(turbowasm_component_host_arguments_commit(&owners[0]), TURBOWASM_OK);
        value = turbowasm_component_host_arguments_values(&owners[0], NULL);
        check_equal(turbowasm_component_instance_create_async_private(&alternate_instance, &components[1], &limits),
            TURBOWASM_OK);
        other = turbowasm_component_instance_public_impl_get(&alternate_instance);
        turbowasm_component_exec_resource_codec_bind(&other_codec, &other->exec, &memory);
        binding = adapter(1u, "consume");
        function = turbowasm_component_type_graph_get(binding.graph, binding.function_type);
        check_equal(memory.resource_lower(memory.resource_context, &other->exec.binary->type_graph,
            function->as.function.params[0], value, &handle), TURBOWASM_TYPE_MISMATCH);
        check_equal(handle, 99u); check_null(other_codec.lower_head);
        check_equal(other->exec.async_resource_owners, 0u);
        check_equal(other->exec.resource_table.live_count, 0u);
        check_equal(resource_codec.exec->async_resource_owners, 1u); check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_arguments_destroy(&owners[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("unreserves all host owners when aggregate adoption exceeds the owner quota") {
        async_instance(1u); make(0u, 42); make(1u, 43);
        check_equal(prepare_async(0u, "two", resources, 2u, true), TURBOWASM_OUT_OF_MEMORY);
        check_equal(resource_codec.exec->async_resource_owners, 0u); check_equal(budget.used, (size_t)0);
        check_equal(turbowasm_component_host_value_destroy(&resources[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&resources[1]), TURBOWASM_OK);
        check_equal(drops(), 2u);
    }
    it("rolls back the retained instance constructor after every failed allocation") {
        turbowasm_component_exec_async_limits limits = {4u, 8u};
        turbowasm_component_public_impl *component = turbowasm_component_public_impl_get(&components[1]);
        size_t failure;
        bool completed = false;
        turbowasm_component_instance_destroy(&instances[1]);
        for (failure = 1u; failure < 256u; ++failure) {
            size_t live = allocations.live;
            uint32_t references = component->ref_count;
            turbowasm_status status;
            allocations.attempts = 0u; allocations.fail_at = failure;
            status = turbowasm_component_instance_create_async_private(&instances[1], &components[1], &limits);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                turbowasm_component_instance_destroy(&instances[1]);
                completed = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(instances[1].impl); check_equal(allocations.live, live);
            check_equal(component->ref_count, references);
            if (completed) break;
        }
        check_true(completed); check_greater(failure, (size_t)10);
    }
}
