#include "component_api_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_host_composites.h"
#include "fixtures/component_host_resources.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

typedef turbowasm_component_host_value host_value;
enum { OWNER_COUNT = 3, BYTE_LIMIT = 65536, FAILURE_LIMIT = 64 };
static turbowasm_component components[2];
static turbowasm_component_instance instances[2];
static turbowasm_component_host_arguments owners[OWNER_COUNT];
static turbowasm_component_host_budget budget;
static turbowasm_component_value result;
static host_value resources[3];
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
        binding.graph, binding.function_type, values, count, move, &budget);
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
    }
    after_each() {
        unsigned i;
        allocations.fail_at = 0u;
        for (i = 0u; i < OWNER_COUNT; ++i)
            (void)turbowasm_component_host_arguments_destroy(&owners[i]);
        (void)turbowasm_component_value_destroy(&result);
        for (i = 0u; i < 3u; ++i) (void)turbowasm_component_host_value_destroy(&resources[i]);
        for (i = 0u; i < 2u; ++i) {
            turbowasm_component_instance_destroy(&instances[i]);
            turbowasm_component_destroy(&components[i]);
        }
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
}
