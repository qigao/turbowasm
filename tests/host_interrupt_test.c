#include <tinytest.h>
#include <turbowasm/turbowasm.h>

typedef struct probe {
    bool requested, enabled, propagate;
    unsigned queries, calls;
} probe;

static bool interrupted(void *context) {
    probe *p = context;
    ++p->queries;
    return p->requested;
}

static turbowasm_status callback(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)arguments; (void)argc; (void)results; (void)capacity;
    probe *p = context;
    ++p->calls;
    unsigned before = p->queries;
    check_equal(turbowasm_host_call_check_interrupt(call), TURBOWASM_OK);
    /* Repeated queries neither consume the invocation's fuel nor latch a
     * cancellation state independent of its configured callback. */
    for (unsigned i = 0; i < 64; ++i)
        check_equal(turbowasm_host_call_check_interrupt(call), TURBOWASM_OK);
    check_equal(p->queries - before, p->enabled ? 65u : 0u);
    p->requested = true;
    turbowasm_status status = turbowasm_host_call_check_interrupt(call);
    check_equal(status, p->enabled ? TURBOWASM_INTERRUPTED : TURBOWASM_OK);
    p->requested = false;
    check_equal(turbowasm_host_call_check_interrupt(call), TURBOWASM_OK);
    *count = 0;
    *trap = TURBOWASM_TRAP_NONE;
    return p->propagate ? status : TURBOWASM_OK;
}

static void exercise(bool enabled, bool propagate) {
    const uint8_t bytes[] = {
        0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
        1, 4, 1, 0x60, 0, 0,
        2, 7, 1, 1, 'h', 1, 'f', 0, 0
    };
    turbowasm_module module = {0}; turbowasm_instance instance = {0};
    turbowasm_linker linker = {0}; probe p = {.enabled = enabled, .propagate = propagate};
    turbowasm_host_function_type type = {0};
    check_equal(turbowasm_module_load_borrowed(&module, bytes, sizeof(bytes)), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_host_function(&linker,
        (turbowasm_name){(const uint8_t *)"h", 1}, (turbowasm_name){(const uint8_t *)"f", 1},
        &type, callback, &p), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
    turbowasm_execution_options options = {0};
    options.has_fuel_limit = true; options.fuel = 1;
    options.should_interrupt = enabled ? interrupted : NULL; options.interrupt_context = &p;
    size_t count = 0; turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke_with_options(&instance, 0, NULL, 0, NULL, 0,
        &count, &trap, &options), enabled && propagate ? TURBOWASM_INTERRUPTED : TURBOWASM_OK);
    check_equal(trap, TURBOWASM_TRAP_NONE);
    check_equal(p.calls, 1u);
    turbowasm_instance_destroy(&instance); turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}

spec("Host invocation interruption query") {
    it("returns OK without an interruption callback") { exercise(false, false); }
    it("queries without consuming fuel or latching interruption") { exercise(true, false); }
    it("allows the host to propagate interruption after cleanup") { exercise(true, true); }
    it("rejects absent call contexts") {
        turbowasm_host_call empty = {0};
        check_equal(turbowasm_host_call_check_interrupt(NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_host_call_check_interrupt(&empty), TURBOWASM_INVALID_ARGUMENT);
    }
}
