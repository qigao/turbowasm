#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_threads.h>

#include <cflow/executor.h>
#include <salts/thread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t shared_provider[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
};

/*
 * Imports:
 *   "wasi"."thread-spawn"                  : (i32) -> i32
 *   "wasi_snapshot_preview1"."proc_exit"   : (i32) -> ()
 *   "m"."mem"                              : shared memory
 *
 * wasi_thread_start(tid, start_arg):
 *   start_arg == 1 -> proc_exit(23)
 *   otherwise      -> marker=1; wait32 forever on memory[4]
 *
 * The exact bytes are intentionally retained in the test so CI qualifies the
 * installed TurboWasm binary reader/linker contract, not a test-only shortcut.
 */
static const uint8_t proc_exit_group_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x13, 0x04, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x02, 0x7f, 0x7f,
    0x00, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x42, 0x03, 0x04, 0x77, 0x61, 0x73,
    0x69, 0x0c, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x2d, 0x73, 0x70, 0x61,
    0x77, 0x6e, 0x00, 0x00, 0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e,
    0x61, 0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31, 0x09, 0x70, 0x72, 0x6f, 0x63, 0x5f, 0x65, 0x78, 0x69,
    0x74, 0x00, 0x01, 0x01, 0x6d, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x03, 0x01,
    0x01, 0x03, 0x04, 0x03, 0x02, 0x00, 0x03, 0x07, 0x26, 0x03, 0x11, 0x77,
    0x61, 0x73, 0x69, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x5f, 0x73,
    0x74, 0x61, 0x72, 0x74, 0x00, 0x02, 0x05, 0x73, 0x70, 0x61, 0x77, 0x6e,
    0x00, 0x03, 0x06, 0x6d, 0x61, 0x72, 0x6b, 0x65, 0x72, 0x00, 0x04, 0x0a,
    0x32, 0x03, 0x21, 0x00, 0x20, 0x01, 0x41, 0x01, 0x46, 0x04, 0x40, 0x41,
    0x17, 0x10, 0x01, 0x05, 0x41, 0x00, 0x41, 0x01, 0x36, 0x02, 0x00, 0x41,
    0x04, 0x41, 0x00, 0x42, 0x7f, 0xfe, 0x01, 0x02, 0x00, 0x1a, 0x0b, 0x0b,
    0x06, 0x00, 0x20, 0x00, 0x10, 0x00, 0x0b, 0x07, 0x00, 0x41, 0x00, 0x28,
    0x02, 0x00, 0x0b
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
               instance, 3u,
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
        if (invoke_i32(root, 4u) == 1)
            return;
        cmeta_thread_yield();
    }
    assert(invoke_i32(root, 4u) == 1);
}

static void test_proc_exit_terminates_thread_group(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config threads_config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_wasi_preview1_config wasi_config = {0};
    cflow_executor executor = {0};
    uint32_t exit_code = UINT32_MAX;
    int32_t waiter_tid;
    int32_t exit_tid;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module,
               proc_exit_group_module,
               sizeof(proc_exit_group_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(cflow_executor_worker_init_with_capacity(
               &executor, 2u, 4u));
    threads_config.executor = &executor;
    threads_config.capacity = 3u;
    assert(turbowasm_wasi_threads_init(
               &threads, &threads_config) == TURBOWASM_OK);

    wasi_config.allow_proc_exit = true;
    wasi_config.proc_exit = turbowasm_wasi_threads_proc_exit;
    wasi_config.proc_exit_context = &threads;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &wasi_config) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               one_char_name("m"),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_wasi_threads_define(
               &threads, &linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &root, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    waiter_tid = invoke_spawn(&root, 2);
    assert(waiter_tid > 0);
    wait_for_marker(&root);

    exit_tid = invoke_spawn(&root, 1);
    assert(exit_tid > 0);
    assert(exit_tid != waiter_tid);

    /*
     * proc_exit publishes the group terminal before unwinding the exiting
     * child. The waiting sibling must wake as INTERRUPTED so the borrowed
     * executor can become idle without a synthetic Wasm notify result.
     */
    assert(cflow_executor_wait_idle(&executor));
    assert(turbowasm_wasi_threads_active(&threads) == 0u);
    assert(turbowasm_wasi_threads_group_exit_code(
               &threads, &exit_code));
    assert(exit_code == 23u);
    assert(!turbowasm_wasi_threads_group_fatal(
               &threads, NULL, NULL));
    assert(invoke_spawn(&root, 2) ==
           TURBOWASM_WASI_THREADS_SPAWN_GROUP_TERMINATED);

    turbowasm_instance_destroy(&root);
    turbowasm_wasi_preview1_destroy(&wasi);
    assert(turbowasm_wasi_threads_destroy(&threads));
    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_proc_exit_terminates_thread_group();
    return 0;
}
