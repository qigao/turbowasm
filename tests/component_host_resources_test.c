#include <turbowasm/component.h>
#include <tinytest.h>
#include "fixtures/component_host_resources.h"
#include <stdlib.h>
#include <string.h>

typedef turbowasm_component_host_value host_value;
enum { MAX_FAILURE_POINTS = 256, REPRESENTATION = 42 };
static turbowasm_component component;
static turbowasm_component_instance instance;
static struct { size_t live, attempts, fail_at; } allocations;

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

static host_value number(int32_t value) {
    return (host_value){.kind = TURBOWASM_COMPONENT_HOST_S32, .as.s32 = value};
}

static turbowasm_status invoke(const char *name, host_value *arguments,
    size_t argument_count, host_value *result, bool move) {
    size_t count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = move
        ? turbowasm_component_instance_invoke_move(&instance, name_span(name),
            arguments, argument_count, result, 1u, &count, &trap)
        : turbowasm_component_instance_invoke(&instance, name_span(name),
            arguments, argument_count, result, 1u, &count, &trap);
    check_equal(count, status == TURBOWASM_OK ? (size_t)1 : (size_t)0);
    return status;
}

static host_value make(const char *name, int32_t rep) {
    host_value argument = number(rep), output = {0};
    check_equal(invoke(name, &argument, 1u, &output, false), TURBOWASM_OK);
    check_equal(output.kind, TURBOWASM_COMPONENT_HOST_OWN);
    return output;
}

static uint32_t drops(void) {
    host_value output = {0};
    check_equal(invoke("drops", NULL, 0u, &output, false), TURBOWASM_OK);
    return output.as.u32;
}

spec("Component public resource ownership") {
    before_each() {
        turbowasm_runtime_config config;
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        check_equal(turbowasm_component_load_borrowed_with_config(&component,
            component_host_resources_bytes, sizeof(component_host_resources_bytes),
            &config), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create(&instance, &component), TURBOWASM_OK);
    }
    after_each() {
        allocations.fail_at = 0u;
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(allocations.live, (size_t)0);
    }
    it("roundtrips own and borrows without consuming the owner") {
        host_value owned = make("make", REPRESENTATION), borrowed = {0}, output = {0};
        check_equal(turbowasm_component_host_value_borrow(&owned, &borrowed), TURBOWASM_OK);
        check_equal(invoke("borrow", &borrowed, 1u, &output, false), TURBOWASM_OK);
        check_equal(output.as.s32, REPRESENTATION);
        check_equal(drops(), 0u);
        check_equal(invoke("echo", &owned, 1u, &output, false), TURBOWASM_INVALID_ARGUMENT);
        check_equal(owned.kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(invoke("echo", &owned, 1u, &output, true), TURBOWASM_OK);
        check_equal((int)owned.kind, 0);
        check_equal(output.kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(drops(), 1u);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("rejects wrong nominal types and provenance without consuming own") {
        host_value wrong = make("make-s", REPRESENTATION), output = {0};
        turbowasm_component_instance other = {0};
        size_t count = 0u;
        turbowasm_trap trap;
        check_equal(invoke("echo", &wrong, 1u, &output, true), TURBOWASM_TYPE_MISMATCH);
        check_equal(wrong.kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(turbowasm_component_instance_create(&other, &component), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_invoke_move(&other, name_span("echo"),
            &wrong, 1u, &output, 1u, &count, &trap), TURBOWASM_INVALID_ARGUMENT);
        turbowasm_component_instance_destroy(&other);
        check_equal(turbowasm_component_host_value_destroy(&wrong), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("rejects duplicated own and overlapping own-borrow admission") {
        host_value args[2] = {make("make", REPRESENTATION), {0}}, output = {0};
        args[1] = args[0];
        check_equal(invoke("two", args, 2u, &output, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(args[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(args[1].kind, TURBOWASM_COMPONENT_HOST_OWN);
        memset(&args[1], 0, sizeof(args[1]));
        check_equal(turbowasm_component_host_value_borrow(&args[0], &args[1]), TURBOWASM_OK);
        check_equal(invoke("own-and-borrow", args, 2u, &output, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_value_destroy(&args[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&args[0]), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("validates the complete value tree before moving or lowering arguments") {
        uint8_t invalid_utf8[] = {0xff};
        host_value args[2] = {make("make", REPRESENTATION),
            {.kind = TURBOWASM_COMPONENT_HOST_STRING, .as.string = {invalid_utf8, 1u}}};
        host_value output = {0};
        check_equal(invoke("text", args, 2u, &output, true), TURBOWASM_INVALID_ARGUMENT);
        check_equal(args[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
        check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_value_destroy(&args[0]), TURBOWASM_OK);
    }
    it("pins borrow loans until completion or cancellation") {
        unsigned mode;
        for (mode = 0u; mode < 2u; ++mode) {
            host_value owned = make("make", REPRESENTATION), borrowed = {0}, output = {0};
            turbowasm_component_call call = {0};
            turbowasm_execution_options budget = {.has_fuel_limit = true, .fuel = 1u};
            check_equal(turbowasm_component_host_value_borrow(&owned, &borrowed), TURBOWASM_OK);
            check_equal(turbowasm_component_call_create(&call, &instance,
                name_span("spin"), &borrowed, 1u), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&owned), TURBOWASM_INVALID_ARGUMENT);
            check_equal(invoke("echo", &owned, 1u, &output, true), TURBOWASM_INVALID_ARGUMENT);
            check_equal(turbowasm_component_call_resume(&call, &budget), TURBOWASM_YIELDED);
            check_equal(turbowasm_component_host_value_destroy(&owned), TURBOWASM_INVALID_ARGUMENT);
            if (mode == 0u) {
                check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
                check_equal(turbowasm_component_call_take_result(&call, &output), TURBOWASM_OK);
                check_equal(output.as.s32, REPRESENTATION);
            }
            turbowasm_component_call_destroy(&call);
            check_equal(turbowasm_component_host_value_destroy(&owned), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
        }
        check_equal(drops(), 2u);
    }
    it("drops transferred own when an unstarted call is destroyed") {
        host_value owned = make("make", REPRESENTATION);
        turbowasm_component_call call = {0};
        check_equal(turbowasm_component_call_create(&call, &instance,
            name_span("consume"), &owned, 1u), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_call_create_move(&call, &instance,
            name_span("consume"), &owned, 1u), TURBOWASM_OK);
        check_equal((int)owned.kind, 0);
        check_equal(drops(), 0u);
        turbowasm_component_call_destroy(&call);
        check_equal(drops(), 1u);
    }
    it("keeps lifted owners alive after call and instance handles close") {
        turbowasm_component_call call = {0};
        host_value argument = number(REPRESENTATION), output = {0};
        check_equal(turbowasm_component_call_create(&call, &instance,
            name_span("make"), &argument, 1u), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&call, &output), TURBOWASM_OK);
        turbowasm_component_call_destroy(&call);
        check_true(allocations.live != 0u);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(allocations.live, (size_t)0);
    }
    it("preflights all loans before destroying a composite and continues after destructor failure") {
        host_value args[2] = {number(-1), number(REPRESENTATION)}, pair = {0}, borrowed = {0};
        turbowasm_component_call call = {0};
        check_equal(invoke("pair", args, 2u, &pair, false), TURBOWASM_OK);
        if (pair.kind == TURBOWASM_COMPONENT_HOST_TUPLE) {
            check_equal(turbowasm_component_host_value_borrow(&pair.as.tuple.items[1], &borrowed), TURBOWASM_OK);
            check_equal(turbowasm_component_call_create(&call, &instance,
                name_span("spin"), &borrowed, 1u), TURBOWASM_OK);
            check_equal(turbowasm_component_host_value_destroy(&pair), TURBOWASM_INVALID_ARGUMENT);
            check_equal(drops(), 0u);
            check_equal(pair.as.tuple.items[0].kind, TURBOWASM_COMPONENT_HOST_OWN);
            turbowasm_component_call_destroy(&call);
            check_equal(turbowasm_component_host_value_destroy(&pair), TURBOWASM_TRAPPED);
            check_equal((int)pair.kind, 0);
            check_equal(drops(), 2u);
            check_equal(turbowasm_component_host_value_destroy(&pair), TURBOWASM_OK);
        }
        check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
    }
    it("cleans a lifted own when a later result field is invalid") {
        host_value argument = number(REPRESENTATION), output = {0};
        check_equal(invoke("bad-pair", &argument, 1u, &output, false), TURBOWASM_TRAPPED);
        check_equal((int)output.kind, 0);
        check_equal(drops(), 1u);
    }
    it("does not return ownership after a committed guest trap") {
        host_value owned = make("make", REPRESENTATION), output = {0};
        check_equal(invoke("trap", &owned, 1u, &output, true), TURBOWASM_TRAPPED);
        check_equal((int)owned.kind, 0);
        check_equal(drops(), 0u);
    }
    it("moves nested own leaves while preserving the caller's composite storage") {
        host_value items[2] = {make("make", 1), make("make", 2)}, output = {0};
        host_value input = {.kind = TURBOWASM_COMPONENT_HOST_LIST, .as.list = {items, 2u}};
        check_equal(invoke("echo-list", &input, 1u, &output, false), TURBOWASM_INVALID_ARGUMENT);
        check_equal(invoke("echo-list", &input, 1u, &output, true), TURBOWASM_OK);
        check_equal(input.kind, TURBOWASM_COMPONENT_HOST_LIST);
        check_true(input.as.list.items == items);
        check_equal((int)items[0].kind, 0);
        check_equal((int)items[1].kind, 0);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        check_equal(drops(), 2u);
    }
    it("rolls back every failed move admission allocation") {
        size_t failure;
        bool completed = false;
        for (failure = 1u; failure < MAX_FAILURE_POINTS; ++failure) {
            host_value owned = make("make", REPRESENTATION);
            turbowasm_component_host_resource *identity = owned.as.own;
            turbowasm_component_call call = {0};
            turbowasm_status status;
            uint32_t before = drops();
            size_t live = allocations.live;
            allocations.attempts = 0u;
            allocations.fail_at = failure;
            status = turbowasm_component_call_create_move(&call, &instance,
                name_span("consume"), &owned, 1u);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                completed = true;
                check_equal((int)owned.kind, 0);
            } else {
                check_equal(status, TURBOWASM_OUT_OF_MEMORY);
                check_equal(owned.kind, TURBOWASM_COMPONENT_HOST_OWN);
                check_true(owned.as.own == identity);
                check_equal(drops(), before);
                check_equal(allocations.live, live);
            }
            turbowasm_component_call_destroy(&call);
            check_equal(turbowasm_component_host_value_destroy(&owned), TURBOWASM_OK);
            check_equal(drops(), before + 1u);
            if (completed)
                break;
        }
        check_true(completed);
    }
    it("cleans partially converted resource results after every allocation failure") {
        size_t failure;
        bool completed = false;
        for (failure = 1u; failure < MAX_FAILURE_POINTS; ++failure) {
            host_value args[2] = {number(1), number(2)}, output = {0};
            turbowasm_component_call call = {0};
            turbowasm_status status;
            uint32_t before = drops();
            check_equal(turbowasm_component_call_create(&call, &instance,
                name_span("pair"), args, 2u), TURBOWASM_OK);
            check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
            allocations.attempts = 0u;
            allocations.fail_at = failure;
            status = turbowasm_component_call_take_result(&call, &output);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK)
                completed = true;
            else {
                check_equal(status, TURBOWASM_OUT_OF_MEMORY);
                check_equal((int)output.kind, 0);
            }
            check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
            turbowasm_component_call_destroy(&call);
            check_equal(drops(), before + 2u);
            if (completed)
                break;
        }
        check_true(completed);
    }
}
