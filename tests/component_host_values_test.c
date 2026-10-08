#include <turbowasm/component.h>
#include <tinytest.h>
#include "fixtures/component_host_composites.h"
#include <stdlib.h>
#include <string.h>

typedef turbowasm_component_host_value host_value;
enum { MAX_FAILURE_POINTS = 256, NESTED_FIELD_COUNT = 4, INDIRECT_FIELD_COUNT = 17 };
static turbowasm_component component;
static turbowasm_component_instance instance;
static struct { size_t live, attempts, fail_at; } allocations;
static uint8_t text_bytes[] = {'h', 0xc3, 0xa9, 0, 'x'};
static uint32_t flags_word = 5u, wide_word = UINT32_C(0x80000001);
static host_value text_value, record_fields[2], tuple_fields[2];
static host_value record, tuple, variants[3], options[2], results[2];
static host_value enumeration, flags, wide_flags, nested_fields[NESTED_FIELD_COUNT];
static host_value nested[2];

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (++allocations.attempts == allocations.fail_at)
        return NULL;
    pointer = malloc(size);
    if (pointer != NULL)
        ++allocations.live;
    return pointer;
}

static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) {
        check_true(allocations.live != 0u);
        --allocations.live;
    }
    free(pointer);
}

static turbowasm_name name_span(const char *name) {
    return (turbowasm_name){(const uint8_t *)name, (uint32_t)strlen(name)};
}

static host_value list_of(host_value *items, size_t count) {
    host_value value = {0};
    value.kind = TURBOWASM_COMPONENT_HOST_LIST;
    value.as.list.items = items;
    value.as.list.count = count;
    return value;
}

static void initialize_values(void) {
    text_value = (host_value){.kind = TURBOWASM_COMPONENT_HOST_STRING,
        .as.string = {text_bytes, sizeof(text_bytes)}};
    record_fields[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_U64,
        .as.u64 = UINT64_MAX};
    record_fields[1] = text_value;
    tuple_fields[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_S32,
        .as.s32 = INT32_MIN};
    tuple_fields[1] = text_value;
    record = (host_value){.kind = TURBOWASM_COMPONENT_HOST_RECORD,
        .as.record = {record_fields, 2u}};
    tuple = (host_value){.kind = TURBOWASM_COMPONENT_HOST_TUPLE,
        .as.tuple = {tuple_fields, 2u}};
    variants[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_VARIANT,
        .as.variant = {0u, NULL}};
    variants[1] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_VARIANT,
        .as.variant = {1u, &text_value}};
    variants[2] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_VARIANT,
        .as.variant = {2u, &tuple}};
    options[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_OPTION,
        .as.option = {0u, NULL}};
    options[1] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_OPTION,
        .as.option = {1u, &variants[2]}};
    results[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_RESULT,
        .as.result = {0u, &record}};
    results[1] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_RESULT,
        .as.result = {1u, &options[1]}};
    enumeration = (host_value){.kind = TURBOWASM_COMPONENT_HOST_ENUM,
        .as.enum_index = 2u};
    flags = (host_value){.kind = TURBOWASM_COMPONENT_HOST_FLAGS,
        .as.flags = {&flags_word, 1u}};
    wide_flags = (host_value){.kind = TURBOWASM_COMPONENT_HOST_FLAGS,
        .as.flags = {&wide_word, 1u}};
    nested_fields[0] = results[1];
    nested_fields[1] = enumeration;
    nested_fields[2] = flags;
    nested_fields[3] = wide_flags;
    nested[0] = (host_value){.kind = TURBOWASM_COMPONENT_HOST_TUPLE,
        .as.tuple = {nested_fields, NESTED_FIELD_COUNT}};
    nested[1] = nested[0];
}

static void equal_value(const host_value *actual, const host_value *expected);

static void equal_sequence(turbowasm_component_host_sequence actual,
    turbowasm_component_host_sequence expected) {
    size_t i;
    check_equal(actual.count, expected.count);
    if (actual.count != expected.count)
        return;
    if (actual.count != 0u)
        check_true(actual.items != expected.items);
    for (i = 0u; i < actual.count; ++i)
        equal_value(&actual.items[i], &expected.items[i]);
}

static void equal_variant(turbowasm_component_host_variant actual,
    turbowasm_component_host_variant expected) {
    check_equal(actual.case_index, expected.case_index);
    check_equal(actual.payload == NULL, expected.payload == NULL);
    if (actual.payload != NULL && expected.payload != NULL) {
        check_true(actual.payload != expected.payload);
        equal_value(actual.payload, expected.payload);
    }
}

static void equal_value(const host_value *actual, const host_value *expected) {
    check_equal(actual->kind, expected->kind);
    if (actual->kind != expected->kind)
        return;
    switch (expected->kind) {
        case TURBOWASM_COMPONENT_HOST_U32:
            check_equal(actual->as.u32, expected->as.u32); break;
        case TURBOWASM_COMPONENT_HOST_U64:
            check_equal(actual->as.u64, expected->as.u64); break;
        case TURBOWASM_COMPONENT_HOST_S32:
            check_equal(actual->as.s32, expected->as.s32); break;
        case TURBOWASM_COMPONENT_HOST_STRING:
            check_equal(actual->as.string.size, expected->as.string.size);
            check_true(actual->as.string.data != expected->as.string.data);
            if (actual->as.string.size == expected->as.string.size)
                check_equal(memcmp(actual->as.string.data, expected->as.string.data,
                    expected->as.string.size), 0);
            break;
        case TURBOWASM_COMPONENT_HOST_LIST:
            equal_sequence(actual->as.list, expected->as.list); break;
        case TURBOWASM_COMPONENT_HOST_RECORD:
            equal_sequence(actual->as.record, expected->as.record); break;
        case TURBOWASM_COMPONENT_HOST_TUPLE:
            equal_sequence(actual->as.tuple, expected->as.tuple); break;
        case TURBOWASM_COMPONENT_HOST_VARIANT:
            equal_variant(actual->as.variant, expected->as.variant); break;
        case TURBOWASM_COMPONENT_HOST_OPTION:
            equal_variant(actual->as.option, expected->as.option); break;
        case TURBOWASM_COMPONENT_HOST_RESULT:
            equal_variant(actual->as.result, expected->as.result); break;
        case TURBOWASM_COMPONENT_HOST_ENUM:
            check_equal(actual->as.enum_index, expected->as.enum_index); break;
        case TURBOWASM_COMPONENT_HOST_FLAGS:
            check_equal(actual->as.flags.word_count, (size_t)1);
            check_true(actual->as.flags.words != expected->as.flags.words);
            check_equal(actual->as.flags.words[0], expected->as.flags.words[0]);
            break;
        default: check(false, "unexpected fixture value kind"); break;
    }
}

static turbowasm_status invoke(const char *name, const host_value *argument,
    host_value *result) {
    size_t count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = turbowasm_component_instance_invoke(&instance,
        name_span(name), argument, argument != NULL ? 1u : 0u,
        result, 1u, &count, &trap);
    check_equal(count, status == TURBOWASM_OK ? (size_t)1 : (size_t)0);
    return status;
}

static void roundtrip(const char *name, const host_value *argument) {
    host_value output = {0};
    check_equal(invoke(name, argument, &output), TURBOWASM_OK);
    equal_value(&output, argument);
    check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
    check_equal((int)output.kind, 0);
    check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
}

spec("Component public composites and retained calls") {
    before_each() {
        turbowasm_runtime_config config;
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_borrowed_with_config(&component,
            component_host_composites_bytes, sizeof(component_host_composites_bytes),
            &config), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create(&instance, &component), TURBOWASM_OK);
        initialize_values();
    }
    after_each() {
        allocations.fail_at = 0u;
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(allocations.live, (size_t)0);
    }
    it("roundtrips each synchronous composite through canonical memory") {
        struct { const char *name; host_value *items; size_t count; } cases[] = {
            {"echo-record", &record, 1u}, {"echo-tuple", &tuple, 1u},
            {"echo-variant", variants, 3u}, {"echo-option", options, 2u},
            {"echo-result", results, 2u}, {"echo-enum", &enumeration, 1u},
            {"echo-flags", &flags, 1u}, {"echo-wide-flags", &wide_flags, 1u},
            {"echo-nested", nested, 2u}
        };
        size_t i;
        for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            host_value input = list_of(cases[i].items, cases[i].count);
            roundtrip(cases[i].name, &input);
            input.as.list.count = 0u;
            roundtrip(cases[i].name, &input);
        }
        roundtrip("flat-enum", &enumeration);
        roundtrip("flat-flags", &flags);
        roundtrip("flat-wide-flags", &wide_flags);
    }
    it("roundtrips direct composites, indirect tuples and re-exported functions") {
        size_t i;
        host_value scalar = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = UINT32_MAX};
        host_value sum = {.kind = TURBOWASM_COMPONENT_HOST_RESULT,
            .as.result = {0u, &scalar}};
        host_value fields[INDIRECT_FIELD_COUNT], wide_tuple;
        host_value enum_list = list_of(&enumeration, 1u);
        roundtrip("flat-record", &record);
        roundtrip("flat-tuple", &tuple);
        for (i = 0u; i < 3u; ++i)
            roundtrip("flat-variant", &variants[i]);
        for (i = 0u; i < 2u; ++i)
            roundtrip("flat-option", &options[i]);
        roundtrip("flat-result", &sum);
        sum.as.result.case_index = 1u;
        sum.as.result.payload = &text_value;
        roundtrip("flat-result", &sum);
        for (i = 0u; i < INDIRECT_FIELD_COUNT; ++i) {
            fields[i] = scalar;
            fields[i].as.u32 -= (uint32_t)i;
        }
        wide_tuple = (host_value){.kind = TURBOWASM_COMPONENT_HOST_TUPLE,
            .as.tuple = {fields, INDIRECT_FIELD_COUNT}};
        roundtrip("indirect-tuple", &wide_tuple);
        roundtrip("again-again", &enum_list);
    }
    it("rejects missing or extra fields, tags, payloads and undeclared bits") {
        host_value output = {0}, input;
        record.as.record.count = 1u;
        input = list_of(&record, 1u);
        check_true(invoke("echo-record", &input, &output) != TURBOWASM_OK);
        tuple.as.tuple.count = 1u;
        input = list_of(&tuple, 1u);
        check_true(invoke("echo-tuple", &input, &output) != TURBOWASM_OK);
        variants[0].as.variant.payload = &text_value;
        input = list_of(variants, 1u);
        check_true(invoke("echo-variant", &input, &output) != TURBOWASM_OK);
        variants[0].as.variant.payload = NULL;
        variants[0].as.variant.case_index = 3u;
        check_true(invoke("echo-variant", &input, &output) != TURBOWASM_OK);
        options[1].as.option.payload = NULL;
        input = list_of(&options[1], 1u);
        check_true(invoke("echo-option", &input, &output) != TURBOWASM_OK);
        results[0].as.result.case_index = 2u;
        input = list_of(results, 1u);
        check_true(invoke("echo-result", &input, &output) != TURBOWASM_OK);
        enumeration.as.enum_index = 3u;
        check_true(invoke("flat-enum", &enumeration, &output) != TURBOWASM_OK);
        flags.as.flags.word_count = 0u;
        check_equal(invoke("flat-flags", &flags, &output), TURBOWASM_INVALID_ARGUMENT);
        flags.as.flags.word_count = 1u;
        flags.as.flags.words = &wide_word;
        check_true(invoke("flat-flags", &flags, &output) != TURBOWASM_OK);
        check_true(invoke("bad-enum", NULL, &output) != TURBOWASM_OK);
        check_true(invoke("bad-flags", NULL, &output) != TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
    }
    it("unwinds every allocation failure and leaves borrowed inputs reusable") {
        host_value input = list_of(nested, 2u);
        size_t failure;
        bool completed = false;
        for (failure = 1u; failure < MAX_FAILURE_POINTS; ++failure) {
            host_value output = {0};
            size_t live = allocations.live;
            turbowasm_status status;
            allocations.attempts = 0u;
            allocations.fail_at = failure;
            status = invoke("echo-nested", &input, &output);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                equal_value(&output, &input);
                completed = true;
            } else {
                check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            }
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            check_equal(allocations.live, live);
            if (completed)
                break;
        }
        check_true(completed);
        roundtrip("echo-nested", &input);
    }
    it("unwinds type export cloning and failed call admission") {
        turbowasm_runtime_config config;
        host_value input = list_of(nested, 2u);
        unsigned operation;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        for (operation = 0u; operation < 2u; ++operation) {
            size_t failure;
            bool completed = false;
            for (failure = 1u; failure < MAX_FAILURE_POINTS; ++failure) {
                turbowasm_component loaded = {0};
                turbowasm_component_call call = {0};
                size_t live = allocations.live;
                turbowasm_status status;
                allocations.attempts = 0u;
                allocations.fail_at = failure;
                status = operation == 0u
                    ? turbowasm_component_load_borrowed_with_config(&loaded,
                        component_host_composites_bytes,
                        sizeof(component_host_composites_bytes), &config)
                    : turbowasm_component_call_create(&call, &instance,
                        name_span("echo-nested"), &input, 1u);
                allocations.fail_at = 0u;
                if (status == TURBOWASM_OK)
                    completed = true;
                else
                    check(status == TURBOWASM_OUT_OF_MEMORY,
                        "operation %u allocation %zu returned %d", operation, failure, (int)status);
                turbowasm_component_call_destroy(&call);
                turbowasm_component_destroy(&loaded);
                check_equal(allocations.live, live);
                if (completed)
                    break;
            }
            check_true(completed);
        }
    }
    it("retains instances across fuel suspension and moves results once") {
        turbowasm_component_call call = {0};
        turbowasm_execution_options budget = {0};
        host_value input = list_of(nested, 2u), output = {0};
        budget.has_fuel_limit = true;
        check_equal(turbowasm_component_call_create(&call, &instance,
            name_span("echo-nested"), &input, 1u), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(turbowasm_component_call_resume(&call, &budget), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&call, &output), TURBOWASM_OK);
        equal_value(&output, &input);
        check_equal(turbowasm_component_call_take_result(&call, &output),
            TURBOWASM_INVALID_ARGUMENT);
        turbowasm_component_call_destroy(&call);
        /* Returned allocations retain their allocator independently of the call. */
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(allocations.live, (size_t)0);
    }
    it("uses the instance allocator when taking results and unwinds partial moves") {
        host_value input = list_of(nested, 2u);
        size_t failure;
        bool completed = false;
        for (failure = 1u; failure < MAX_FAILURE_POINTS; ++failure) {
            turbowasm_component_call call = {0};
            host_value output = {0};
            size_t live = allocations.live;
            turbowasm_status status;
            check_equal(turbowasm_component_call_create(&call, &instance,
                name_span("echo-nested"), &input, 1u), TURBOWASM_OK);
            check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
            allocations.attempts = 0u;
            allocations.fail_at = failure;
            status = turbowasm_component_call_take_result(&call, &output);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                equal_value(&output, &input);
                completed = true;
            } else {
                check_equal(status, TURBOWASM_OUT_OF_MEMORY);
                check_equal((int)output.kind, 0);
            }
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            turbowasm_component_call_destroy(&call);
            check_equal(allocations.live, live);
            if (completed)
                break;
        }
        check_true(completed);
    }
    it("releases unstarted and cancelled calls after their public instance is gone") {
        unsigned mode;
        for (mode = 0u; mode < 2u; ++mode) {
            turbowasm_component_instance local = {0};
            turbowasm_component_call call = {0};
            turbowasm_execution_options budget = {0};
            host_value input = list_of(nested, 2u);
            size_t live = allocations.live;
            budget.has_fuel_limit = true;
            check_equal(turbowasm_component_instance_create(&local, &component), TURBOWASM_OK);
            check_equal(turbowasm_component_call_create(&call, &local,
                name_span("echo-nested"), &input, 1u), TURBOWASM_OK);
            turbowasm_component_instance_destroy(&local);
            if (mode != 0u)
                check_equal(turbowasm_component_call_resume(&call, &budget), TURBOWASM_YIELDED);
            turbowasm_component_call_destroy(&call);
            check_equal(allocations.live, live);
        }
    }
}
