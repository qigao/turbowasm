#include <turbowasm/cflow.h>
#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct lifecycle_probe {
    uint32_t complete_count;
    uint32_t finalize_count;
    turbowasm_cflow_completion completion;
} lifecycle_probe;

static void on_complete(
    void *user,
    const turbowasm_cflow_completion *completion) {
    lifecycle_probe *probe = (lifecycle_probe *)user;
    assert(probe != NULL);
    assert(completion != NULL);
    ++probe->complete_count;
    probe->completion = *completion;
}

static void on_finalize(void *user) {
    lifecycle_probe *probe = (lifecycle_probe *)user;
    assert(probe != NULL);
    ++probe->finalize_count;
}

static void load_constant_module(
    turbowasm_module *module,
    turbowasm_instance *instance) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type 0: () -> i32 */
        0x01, 0x05,
        0x01,
        0x60, 0x00, 0x01, 0x7f,

        /* function 0 uses type 0 */
        0x03, 0x02,
        0x01, 0x00,

        /* function 0: i32.const 7 */
        0x0a, 0x06,
        0x01,
        0x04,
        0x00,
        0x41, 0x07,
        0x0b
    };

    assert(turbowasm_module_load_borrowed(
               module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               instance, module) == TURBOWASM_OK);
}

static void test_manual_execution(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    cflow_executor executor = {0};
    lifecycle_probe probe = {0};
    turbowasm_value result = {0};
    turbowasm_cflow_call call = {0};

    load_constant_module(&module, &instance);
    assert(cflow_executor_manual_init_with_capacity(&executor, 1u));

    call.instance = &instance;
    call.function_index = 0u;
    call.results = &result;
    call.result_capacity = 1u;
    call.complete = on_complete;
    call.finalize = on_finalize;
    call.user = &probe;

    assert(turbowasm_cflow_try_submit(
               &executor, &call) == CFLOW_ADMISSION_ACCEPTED);
    assert(probe.complete_count == 0u);
    assert(probe.finalize_count == 0u);

    assert(cflow_executor_run_ready(&executor) == 1u);
    assert(probe.complete_count == 1u);
    assert(probe.finalize_count == 1u);
    assert(probe.completion.terminal == TURBOWASM_CFLOW_EXECUTED);
    assert(probe.completion.status == TURBOWASM_OK);
    assert(probe.completion.trap == TURBOWASM_TRAP_NONE);
    assert(probe.completion.result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);

    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_cancel_pending(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    lifecycle_probe probe = {0};
    turbowasm_cflow_call call = {0};

    load_constant_module(&module, &instance);
    assert(cflow_executor_manual_init_with_capacity(&executor, 1u));
    assert(cflow_executor_as_control(&executor, &control));

    call.instance = &instance;
    call.function_index = 0u;
    call.complete = on_complete;
    call.finalize = on_finalize;
    call.user = &probe;

    assert(turbowasm_cflow_try_submit(
               &executor, &call) == CFLOW_ADMISSION_ACCEPTED);

    assert(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    assert(probe.complete_count == 1u);
    assert(probe.finalize_count == 1u);
    assert(probe.completion.terminal == TURBOWASM_CFLOW_CANCELLED);
    assert(probe.completion.status == TURBOWASM_OK);
    assert(probe.completion.trap == TURBOWASM_TRAP_NONE);
    assert(probe.completion.result_count == 0u);
    assert(cflow_executor_run_ready(&executor) == 0u);

    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_rejected_admission_has_no_callbacks(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    cflow_executor executor = {0};
    lifecycle_probe first_probe = {0};
    lifecycle_probe second_probe = {0};
    turbowasm_cflow_call first = {0};
    turbowasm_cflow_call second = {0};

    load_constant_module(&module, &instance);
    assert(cflow_executor_manual_init_with_capacity(&executor, 1u));

    first.instance = &instance;
    first.function_index = 0u;
    first.complete = on_complete;
    first.finalize = on_finalize;
    first.user = &first_probe;

    second.instance = &instance;
    second.function_index = 0u;
    second.complete = on_complete;
    second.finalize = on_finalize;
    second.user = &second_probe;

    assert(turbowasm_cflow_try_submit(
               &executor, &first) == CFLOW_ADMISSION_ACCEPTED);
    assert(turbowasm_cflow_try_submit(
               &executor, &second) == CFLOW_ADMISSION_FULL);
    assert(second_probe.complete_count == 0u);
    assert(second_probe.finalize_count == 0u);

    assert(cflow_executor_run_ready(&executor) == 1u);
    assert(first_probe.complete_count == 1u);
    assert(first_probe.finalize_count == 1u);

    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_virtual_clock_deadline(void) {
    cflow_clock clock = {0};
    turbowasm_cflow_deadline deadline = {0};

    assert(cflow_clock_virtual_init(
        &clock, (cflow_instant){100u}));
    assert(turbowasm_cflow_deadline_init_after(
        &deadline, &clock, cflow_duration_from_ns(25u)));

    assert(!turbowasm_cflow_deadline_should_interrupt(
        &deadline));
    assert(cflow_clock_advance(
        &clock, cflow_duration_from_ns(24u)));
    assert(!turbowasm_cflow_deadline_should_interrupt(
        &deadline));
    assert(cflow_clock_advance(
        &clock, cflow_duration_from_ns(1u)));
    assert(turbowasm_cflow_deadline_should_interrupt(
        &deadline));

    cflow_clock_destroy(&clock);
}

static void test_deadline_execution_policy(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    cflow_clock clock = {0};
    turbowasm_cflow_deadline deadline = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    load_constant_module(&module, &instance);
    assert(cflow_clock_virtual_init(
        &clock, (cflow_instant){100u}));

    /* Zero delay is expired at the first execution checkpoint. */
    assert(turbowasm_cflow_deadline_init_after(
        &deadline, &clock, cflow_duration_from_ns(0u)));
    assert(turbowasm_cflow_execution_options_set_deadline(
        &options, &deadline));
    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap,
               &options) == TURBOWASM_INTERRUPTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Deadline policy leaves fuel accounting untouched. */
    options = (turbowasm_execution_options){0};
    options.has_fuel_limit = true;
    options.fuel = 1u;
    assert(turbowasm_cflow_deadline_init_after(
        &deadline, &clock, cflow_duration_from_s(1u)));
    assert(turbowasm_cflow_execution_options_set_deadline(
        &options, &deadline));
    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap,
               &options) == TURBOWASM_FUEL_EXHAUSTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    cflow_clock_destroy(&clock);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_virtual_clock_deadline();
    test_deadline_execution_policy();
    test_manual_execution();
    test_cancel_pending();
    test_rejected_admission_has_no_callbacks();
    return 0;
}
