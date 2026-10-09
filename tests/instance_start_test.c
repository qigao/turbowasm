#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include "instance_internal.h"

#define HEADER 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0
static const uint8_t empty[] = {HEADER};
static const uint8_t looping[] = {
    HEADER, 1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 8, 1, 0,
    10, 9, 1, 7, 0, 3, 0x40, 0x0c, 0, 0x0b, 0x0b
};
static const uint8_t trapping[] = {
    HEADER, 1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 8, 1, 0,
    10, 5, 1, 3, 0, 0, 0x0b
};
static const uint8_t calling[] = {
    HEADER, 1, 4, 1, 0x60, 0, 0,
    2, 7, 1, 1, 'h', 1, 'f', 0, 0,
    3, 2, 1, 0, 8, 1, 1,
    /* start: call h.f; nop; nop; end */
    10, 8, 1, 6, 0, 0x10, 0, 1, 1, 0x0b
};

static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_linker linker;
static turbowasm_execution_options options;
static turbowasm_trap trap;
static unsigned callbacks, queries;
static bool interrupted, spawn, interrupt_child;
static turbowasm_status host_status, sibling_status;

static bool should_interrupt(void *context) {
    (void)context;
    ++queries;
    return interrupted;
}

static turbowasm_status host(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *out_trap) {
    (void)context; (void)args; (void)argc; (void)results; (void)capacity;
    ++callbacks;
    *count = 0;
    *out_trap = TURBOWASM_TRAP_NONE;
    if (spawn) {
        turbowasm_instance child = {0};
        spawn = false;
        interrupted = interrupt_child;
        sibling_status = turbowasm_instance_create_sibling_internal(&child, call);
        if (sibling_status != TURBOWASM_OK) check_null(child.impl);
        turbowasm_instance_destroy(&child);
        return sibling_status;
    }
    return host_status;
}

static void load(const uint8_t *bytes, size_t size) {
    check_equal(turbowasm_module_load_borrowed(&module, bytes, size), TURBOWASM_OK);
}

static turbowasm_status create(void) {
    return turbowasm_instance_create_linked_with_options(
        &instance, &module, &linker, &options, &trap);
}

spec("Bounded instance startup") {
    before_each() {
        callbacks = queries = 0;
        interrupted = spawn = interrupt_child = false;
        host_status = sibling_status = TURBOWASM_OK;
        options = (turbowasm_execution_options){.fuel = 100, .has_fuel_limit = true,
            .should_interrupt = should_interrupt};
        trap = TURBOWASM_TRAP_UNREACHABLE;
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        turbowasm_host_function_type type = {0};
        check_equal(turbowasm_linker_define_host_function(&linker,
            (turbowasm_name){(const uint8_t *)"h", 1},
            (turbowasm_name){(const uint8_t *)"f", 1}, &type, host, NULL), TURBOWASM_OK);
    }
    after_each() {
        turbowasm_instance_destroy(&instance);
        turbowasm_linker_destroy(&linker);
        turbowasm_module_destroy(&module);
    }
    it("does not execute guest code or query interruption without a start") {
        load(empty, sizeof(empty));
        options.fuel = 0; interrupted = true;
        check_equal(create(), TURBOWASM_OK);
        check_equal(queries, 0u); check_equal(trap, TURBOWASM_TRAP_NONE);
    }
    it("cleans a start that exhausts fuel and permits retry with an empty handle") {
        load(looping, sizeof(looping));
        check_equal(create(), TURBOWASM_FUEL_EXHAUSTED);
        check_null(instance.impl); check_equal(trap, TURBOWASM_TRAP_NONE);
        interrupted = true;
        check_equal(create(), TURBOWASM_INTERRUPTED);
        check_null(instance.impl); check_equal(trap, TURBOWASM_TRAP_NONE);
    }
    it("reports the start trap while releasing the failed instance") {
        load(trapping, sizeof(trapping));
        check_equal(create(), TURBOWASM_TRAPPED);
        check_null(instance.impl); check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
    }
    it("preserves admitted host effects and propagates host failure") {
        load(calling, sizeof(calling));
        host_status = TURBOWASM_OUT_OF_MEMORY;
        check_equal(create(), TURBOWASM_OUT_OF_MEMORY);
        check_equal(callbacks, 1u); check_null(instance.impl);
        check_equal(trap, TURBOWASM_TRAP_NONE);
        host_status = TURBOWASM_OK;
        check_equal(create(), TURBOWASM_OK);
        check_equal(callbacks, 2u);
    }
    it("requires explicit options and an empty destination without overwriting it") {
        load(empty, sizeof(empty));
        check_equal(turbowasm_instance_create_linked_with_options(
            &instance, &module, &linker, NULL, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(trap, TURBOWASM_TRAP_NONE); check_null(instance.impl);
        check_equal(turbowasm_instance_create_linked_with_options(
            &instance, &module, &linker, &options, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(create(), TURBOWASM_OK);
        void *original = instance.impl;
        check_equal(create(), TURBOWASM_INVALID_ARGUMENT);
        check_equal(instance.impl, original);
    }
    it("shares the parent fuel budget with synchronous sibling startup") {
        load(calling, sizeof(calling));
        spawn = true;
        options.fuel = 1;
        check_equal(create(), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(sibling_status, TURBOWASM_FUEL_EXHAUSTED);
        check_equal(callbacks, 1u); check_null(instance.impl);
    }
    it("shares interruption with sibling startup before further host effects") {
        load(calling, sizeof(calling));
        spawn = interrupt_child = true;
        check_equal(create(), TURBOWASM_INTERRUPTED);
        check_equal(sibling_status, TURBOWASM_INTERRUPTED);
        check_equal(callbacks, 1u); check_null(instance.impl);
    }
    it("retains remaining fuel rather than resetting it after sibling startup") {
        load(calling, sizeof(calling));
        spawn = true;
        options.fuel = 7;
        check_equal(create(), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(sibling_status, TURBOWASM_OK);
        check_equal(callbacks, 2u); check_null(instance.impl);
        spawn = true; options.fuel = 100;
        check_equal(create(), TURBOWASM_OK);
        size_t count = 0;
        options.fuel = 0;
        check_equal(turbowasm_instance_invoke_with_options(&instance, 1,
            NULL, 0, NULL, 0, &count, &trap, &options), TURBOWASM_FUEL_EXHAUSTED);
    }
}
