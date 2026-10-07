#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_threads.h>

#include <cflow/executor.h>
#include <salts/thread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t shared_provider[] = {
    WASM_HEADER,
    0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
};

/*
 * Imports:
 *   wasi.thread-spawn : (i32) -> i32
 *   m.mem             : shared memory
 *
 * wasi_thread_start(tid, arg):
 *   arg == 1 -> unreachable trap
 *   otherwise write marker=1, then wait32 forever at memory[4].
 *
 * This lets the test prove that group-fatal wakes an already blocked atomic
 * waiter and returns TURBOWASM_INTERRUPTED rather than a Wasm wait result.
 */
static const uint8_t fatal_group_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0f, 0x03,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,
    0x02, 0x1f, 0x02,
    0x04, 0x77, 0x61, 0x73, 0x69,
    0x0c, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64,
          0x2d, 0x73, 0x70, 0x61, 0x77, 0x6e,
    0x00, 0x00,
    0x01, 0x6d,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x01,
    0x03, 0x05, 0x04, 0x01, 0x00, 0x02, 0x02,
    0x07, 0x2e, 0x04,
    0x11,
    0x77, 0x61, 0x73, 0x69, 0x5f, 0x74, 0x68, 0x72, 0x65,
    0x61, 0x64, 0x5f, 0x73, 0x74, 0x61, 0x72, 0x74,
    0x00, 0x01,
    0x05, 0x73, 0x70, 0x61, 0x77, 0x6e, 0x00, 0x02,
    0x06, 0x6d, 0x61, 0x72, 0x6b, 0x65, 0x72, 0x00, 0x03,
    0x05, 0x61, 0x66, 0x74, 0x65, 0x72, 0x00, 0x04,
    0x0a, 0x33, 0x04,
    0x1d, 0x00,
    0x20, 0x01,
    0x41, 0x01,
    0x46,
    0x04, 0x40,
    0x00,
    0x0b,
    0x41, 0x00,
    0x41, 0x01,
    0x36, 0x02, 0x00,
    0x41, 0x04,
    0x41, 0x00,
    0x42, 0x7f,
    0xfe, 0x01, 0x02, 0x00,
    0x1a,
    0x0b,
    0x06, 0x00,
    0x20, 0x00,
    0x10, 0x00,
    0x0b,
    0x07, 0x00,
    0x41, 0x00,
    0x28, 0x02, 0x00,
    0x0b,
    0x04, 0x00,
    0x41, 0x07,
    0x0b
};

static turbowasm_name one_char_name(const char *text) {
    turbowasm_name name = {0};
    name.bytes = (const uint8_t *)text;
    name.size = 1u;
    return name;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static int32_t invoke_spawn(
    turbowasm_instance *instance,
    int32_t start_arg) {
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = start_arg;

    assert(turbowasm_instance_invoke(
               instance, 2u,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void wait_for_marker(turbowasm_instance *root) {
    uint32_t attempt;

    for (attempt = 0u; attempt < 200000u; ++attempt) {
        if (invoke_i32(root, 3u) == 1)
            return;
        cmeta_thread_yield();
    }
    assert(invoke_i32(root, 3u) == 1);
}

static void test_trap_interrupts_blocked_group_waiter(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    turbowasm_wasi_threads_execution_policy root_policy = {0};
    turbowasm_execution_options options = {0};
    cflow_executor executor = {0};
    turbowasm_status fatal_status = TURBOWASM_OK;
    turbowasm_trap fatal_trap = TURBOWASM_TRAP_NONE;
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    int32_t waiter_tid;
    int32_t trap_tid;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module,
               fatal_group_module,
               sizeof(fatal_group_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(cflow_executor_worker_init_with_capacity(
               &executor, 2u, 4u));
    config.executor = &executor;
    config.capacity = 3u;
    assert(turbowasm_wasi_threads_init(
               &threads, &config) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               one_char_name("m"),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_wasi_threads_define(
               &threads, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &root, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    waiter_tid = invoke_spawn(&root, 2);
    assert(waiter_tid > 0);
    wait_for_marker(&root);

    trap_tid = invoke_spawn(&root, 1);
    assert(trap_tid > 0);
    assert(trap_tid != waiter_tid);

    /*
     * This only becomes idle if the fatal publication wakes the infinite
     * wait32 and the wait returns TURBOWASM_INTERRUPTED to its child invoke.
     */
    assert(cflow_executor_wait_idle(&executor));
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    assert(turbowasm_wasi_threads_group_fatal(
               &threads, &fatal_status, &fatal_trap));
    assert(fatal_status == TURBOWASM_TRAPPED);
    assert(fatal_trap == TURBOWASM_TRAP_UNREACHABLE);

    assert(turbowasm_wasi_threads_execution_policy_init(
               &root_policy, &threads));
    assert(turbowasm_wasi_threads_execution_policy_apply(
               &root_policy, &options));

    assert(turbowasm_instance_invoke_with_options(
               &root, 4u,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap,
               &options) == TURBOWASM_INTERRUPTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    assert(invoke_spawn(&root, 2) ==
           TURBOWASM_WASI_THREADS_SPAWN_GROUP_TERMINATED);

    turbowasm_instance_destroy(&root);
    assert(turbowasm_wasi_threads_destroy(&threads));
    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_trap_interrupts_blocked_group_waiter();
    return 0;
}
