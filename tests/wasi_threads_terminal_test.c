#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_threads.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <string.h>

static const uint8_t shared_provider[] = {
    0,97,115,109,1,0,0,0,5,4,1,3,1,1,7,7,1,3,'m','e','m',2,0
};

/* Compiled from fixtures/wasi_threads_terminals.wat. Keep the binary fixture
 * embedded so this native adapter test does not require a guest toolchain. */
static const uint8_t terminal_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x12,0x04,0x60,
    0x01,0x7f,0x01,0x7f,0x60,0x00,0x00,0x60,0x02,0x7f,0x7f,0x00,
    0x60,0x00,0x01,0x7f,0x02,0x2e,0x03,0x04,0x77,0x61,0x73,0x69,
    0x0c,0x74,0x68,0x72,0x65,0x61,0x64,0x2d,0x73,0x70,0x61,0x77,
    0x6e,0x00,0x00,0x04,0x74,0x65,0x73,0x74,0x07,0x66,0x61,0x69,
    0x6c,0x75,0x72,0x65,0x00,0x01,0x01,0x6d,0x03,0x6d,0x65,0x6d,
    0x02,0x03,0x01,0x01,0x03,0x05,0x04,0x02,0x00,0x03,0x03,0x0d,
    0x03,0x01,0x00,0x01,0x07,0x2e,0x04,0x11,0x77,0x61,0x73,0x69,
    0x5f,0x74,0x68,0x72,0x65,0x61,0x64,0x5f,0x73,0x74,0x61,0x72,
    0x74,0x00,0x02,0x05,0x73,0x70,0x61,0x77,0x6e,0x00,0x03,0x06,
    0x6d,0x61,0x72,0x6b,0x65,0x72,0x00,0x04,0x05,0x61,0x66,0x74,
    0x65,0x72,0x00,0x05,0x0a,0x50,0x04,0x39,0x00,0x20,0x01,0x41,
    0x01,0x46,0x04,0x40,0x00,0x0b,0x20,0x01,0x41,0x02,0x46,0x04,
    0x40,0x08,0x00,0x0b,0x20,0x01,0x41,0x03,0x46,0x04,0x40,0x10,
    0x01,0x0b,0x20,0x01,0x45,0x04,0x40,0x41,0x00,0x41,0x01,0xfe,
    0x1e,0x02,0x00,0x1a,0x41,0x04,0x41,0x00,0x42,0x7f,0xfe,0x01,
    0x02,0x00,0x1a,0x0b,0x0b,0x06,0x00,0x20,0x00,0x10,0x00,0x0b,
    0x08,0x00,0x41,0x00,0xfe,0x10,0x02,0x00,0x0b,0x04,0x00,0x41,
    0x07,0x0b,0x00,0x31,0x04,0x6e,0x61,0x6d,0x65,0x01,0x11,0x02,
    0x00,0x05,0x73,0x70,0x61,0x77,0x6e,0x01,0x07,0x66,0x61,0x69,
    0x6c,0x75,0x72,0x65,0x02,0x0d,0x01,0x02,0x02,0x00,0x03,0x74,
    0x69,0x64,0x01,0x03,0x61,0x72,0x67,0x0b,0x08,0x01,0x00,0x05,
    0x65,0x72,0x72,0x6f,0x72,
};

static turbowasm_module provider_module, module;
static turbowasm_instance provider, root;
static turbowasm_linker linker;
static turbowasm_wasi_threads threads;
static cflow_executor executor;
static cflow_executor_control control;
static turbowasm_status host_status;
static atomic_uint gate_entered, gate_release, unrelated_calls;

static turbowasm_name name(const char *text) {
    return (turbowasm_name){(const uint8_t *)text, (uint32_t)strlen(text)};
}

static turbowasm_status fail_host(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)arguments; (void)argument_count;
    (void)results; (void)capacity;
    *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return host_status;
}

static int32_t invoke(uint32_t index, const turbowasm_value *argument) {
    turbowasm_value result = {0}; size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke(&root, index, argument, argument ? 1 : 0,
        &result, 1, &count, &trap), TURBOWASM_OK);
    check_equal(count, (size_t)1); check_equal(trap, TURBOWASM_TRAP_NONE);
    return result.as.i32;
}
static int32_t spawn(int32_t value) {
    turbowasm_value argument = {.kind=TURBOWASM_VALUE_I32, .as.i32=value};
    return invoke(3, &argument);
}

static void wait_for_markers(int expected) {
    for (unsigned i = 0; i < 200000; ++i) {
        if (invoke(4, NULL) == expected) return;
        cmeta_thread_yield();
    }
    check_equal(invoke(4, NULL), expected);
}
static void wait_for_marker(void) { wait_for_markers(1); }
static void gate(void *context) {
    (void)context;
    atomic_store(&gate_entered, 1);
    while (!atomic_load(&gate_release)) cmeta_thread_yield();
}
static void unrelated(void *context) {
    (void)context; atomic_fetch_add(&unrelated_calls, 1);
}
static void wait_for_gate(void) {
    for (unsigned i = 0; i < 200000 && !atomic_load(&gate_entered); ++i) cmeta_thread_yield();
    check_equal(atomic_load(&gate_entered), 1u);
}

static void check_terminal(turbowasm_status expected, turbowasm_trap expected_trap) {
    turbowasm_status status = TURBOWASM_OK; turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(turbowasm_wasi_threads_active(&threads), (size_t)0);
    check_true(turbowasm_wasi_threads_group_fatal(&threads, &status, &trap));
    check_equal(status, expected); check_equal(trap, expected_trap);
    check_equal(spawn(4), TURBOWASM_WASI_THREADS_SPAWN_GROUP_TERMINATED);
    turbowasm_wasi_threads_execution_policy policy = {0};
    turbowasm_execution_options options = {0};
    check_true(turbowasm_wasi_threads_execution_policy_init(&policy, &threads));
    check_true(turbowasm_wasi_threads_execution_policy_apply(&policy, &options));
    turbowasm_value result = {0}; size_t count = 99;
    check_equal(turbowasm_instance_invoke_with_options(&root, 5, NULL, 0,
        &result, 1, &count, &trap, &options), TURBOWASM_INTERRUPTED);
    check_equal(count, (size_t)0); check_equal(trap, TURBOWASM_TRAP_NONE);
    /* A later process-exit callback cannot overwrite the first abnormal result. */
    turbowasm_wasi_threads_proc_exit(&threads, &root, 23);
    check_true(turbowasm_wasi_threads_group_fatal(&threads, &status, &trap));
    check_equal(status, expected); check_equal(trap, expected_trap);
    uint32_t exit_code = 99;
    check_false(turbowasm_wasi_threads_group_exit_code(&threads, &exit_code));
}

suite("WASI child abnormal terminals") {
    before_all() {
        check_equal(turbowasm_module_load_borrowed(&provider_module,
            shared_provider, sizeof shared_provider), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed(&module,
            terminal_module, sizeof terminal_module), TURBOWASM_OK);
    }
    after_all() {
        turbowasm_module_destroy(&module); turbowasm_module_destroy(&provider_module);
    }
    before_each() {
        host_status = TURBOWASM_OUT_OF_MEMORY;
        atomic_store(&gate_entered, 0); atomic_store(&gate_release, 0); atomic_store(&unrelated_calls, 0);
        check_true(cflow_executor_worker_init_with_capacity(&executor, 2, 4));
        check_true(cflow_executor_as_control(&executor, &control));
        turbowasm_wasi_threads_config config = {&executor, 4};
        check_equal(turbowasm_wasi_threads_init(&threads, &config), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&provider, &provider_module), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("m"), &provider), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_define(&threads, &linker), TURBOWASM_OK);
        turbowasm_host_function_type type = {0};
        check_equal(turbowasm_linker_define_host_function(&linker, name("test"), name("failure"),
            &type, fail_host, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&root, &module, &linker), TURBOWASM_OK);
    }
    after_each() {
        atomic_store(&gate_release, 1);
        turbowasm_wasi_threads_proc_exit(&threads, &root, 0);
        check_true(cflow_executor_wait_idle(&executor));
        turbowasm_instance_destroy(&root);
        check_true(turbowasm_wasi_threads_destroy(&threads));
        cflow_executor_destroy(&executor); control = (cflow_executor_control){0};
        turbowasm_linker_destroy(&linker); turbowasm_instance_destroy(&provider);
    }
    it("wakes an atomic waiter after an uncaught Wasm exception") {
        check_true(spawn(0) > 0); wait_for_marker();
        check_true(spawn(2) > 0);
        check_terminal(TURBOWASM_EXCEPTION, TURBOWASM_TRAP_NONE);
        check_true(cflow_executor_post(&executor, unrelated, NULL));
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(atomic_load(&unrelated_calls), 1u);
    }
    it("propagates host allocation failure without leaving a sibling blocked") {
        check_true(spawn(0) > 0); wait_for_marker();
        check_true(spawn(3) > 0);
        check_terminal(TURBOWASM_OUT_OF_MEMORY, TURBOWASM_TRAP_NONE);
    }
    it("propagates unsupported host execution instead of silently losing a child") {
        host_status = TURBOWASM_UNSUPPORTED;
        check_true(spawn(0) > 0); wait_for_marker(); check_true(spawn(3) > 0);
        check_terminal(TURBOWASM_UNSUPPORTED, TURBOWASM_TRAP_NONE);
    }
    it("propagates a host interruption that has no previous group terminal") {
        host_status = TURBOWASM_INTERRUPTED;
        check_true(spawn(0) > 0); wait_for_marker(); check_true(spawn(3) > 0);
        check_terminal(TURBOWASM_INTERRUPTED, TURBOWASM_TRAP_NONE);
    }
    it("publishes cancellation before finalization and wakes a running sibling") {
        check_true(cflow_executor_post(&executor, gate, NULL)); wait_for_gate();
        check_true(spawn(0) > 0); wait_for_marker();
        check_true(spawn(4) > 0); /* Both workers occupied: this child is queued. */
        check_equal(turbowasm_wasi_threads_active(&threads), (size_t)2);
        check_true(cflow_executor_control_shutdown(&control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&gate_release, 1);
        check_terminal(TURBOWASM_INTERRUPTED, TURBOWASM_TRAP_NONE);
        cflow_executor_protocol_stats stats = {0};
        check_true(cflow_executor_control_get_stats(&control, &stats));
        check_equal(stats.cancelled, (size_t)1);
        check_equal(stats.completed, (size_t)2);
    }
    it("keeps an earlier proc_exit when queued children are cancelled") {
        check_true(cflow_executor_post(&executor, gate, NULL)); wait_for_gate();
        check_true(spawn(0) > 0); wait_for_marker(); check_true(spawn(4) > 0);
        check_true(cflow_executor_control_shutdown(&control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        turbowasm_wasi_threads_proc_exit(&threads, &root, 23);
        atomic_store(&gate_release, 1);
        check_true(cflow_executor_wait_idle(&executor));
        uint32_t code = 0;
        check_true(turbowasm_wasi_threads_group_exit_code(&threads, &code)); check_equal(code, 23u);
        check_false(turbowasm_wasi_threads_group_fatal(&threads, NULL, NULL));
        check_equal(turbowasm_wasi_threads_active(&threads), (size_t)0);
    }
    it("drains a saturated borrowed executor after explicit group exit") {
        check_true(spawn(0) > 0); check_true(spawn(0) > 0); wait_for_markers(2);
        check_true(spawn(4) > 0);
        check_true(cflow_executor_control_shutdown(&control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        /* Cancellation delivery needs a worker. Interrupt running children
         * before waiting for shutdown; CANCEL_PENDING alone cannot do so. */
        turbowasm_wasi_threads_proc_exit(&threads, &root, 31);
        check_true(cflow_executor_wait_idle(&executor));
        uint32_t code = 0;
        check_true(turbowasm_wasi_threads_group_exit_code(&threads, &code)); check_equal(code, 31u);
        check_false(turbowasm_wasi_threads_group_fatal(&threads, NULL, NULL));
        check_equal(turbowasm_wasi_threads_active(&threads), (size_t)0);
        cflow_executor_protocol_stats stats = {0};
        check_true(cflow_executor_control_get_stats(&control, &stats));
        check_equal(stats.cancelled, (size_t)1); check_equal(stats.completed, (size_t)2);
    }
    it("keeps successful child return local and leaves admission open") {
        check_true(spawn(4) > 0); check_true(cflow_executor_wait_idle(&executor));
        check_false(turbowasm_wasi_threads_group_fatal(&threads, NULL, NULL));
        check_false(turbowasm_wasi_threads_group_exit_code(&threads, NULL));
        check_true(spawn(4) > 0); check_true(cflow_executor_wait_idle(&executor));
        check_equal(invoke(5, NULL), 7);
    }
}
