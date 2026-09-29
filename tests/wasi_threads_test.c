#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_threads.h>

#include <cflow/executor.h>

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
    /* shared memory min=1 max=1 */
    0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
    /* export "mem" */
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
};

static const uint8_t threaded_consumer[] = {
    WASM_HEADER,

    /* types:
     * 0: (i32) -> i32       thread-spawn
     * 1: (i32, i32) -> ()  wasi_thread_start
     * 2: () -> i32         test helpers
     */
    0x01, 0x0f, 0x03,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,

    /* imports:
     * "wasi"."thread-spawn" function type 0
     * "m"."mem" shared memory min=1 max=1
     */
    0x02, 0x1f, 0x02,
    0x04, 0x77, 0x61, 0x73, 0x69,
    0x0c, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64,
          0x2d, 0x73, 0x70, 0x61, 0x77, 0x6e,
    0x00, 0x00,
    0x01, 0x6d,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x01,

    /* four defined functions: start, spawn, load_tid, load_arg */
    0x03, 0x05, 0x04, 0x01, 0x02, 0x02, 0x02,

    /* exports */
    0x07, 0x33, 0x04,
    0x11,
    0x77, 0x61, 0x73, 0x69, 0x5f, 0x74, 0x68, 0x72, 0x65,
    0x61, 0x64, 0x5f, 0x73, 0x74, 0x61, 0x72, 0x74,
    0x00, 0x01,
    0x05, 0x73, 0x70, 0x61, 0x77, 0x6e, 0x00, 0x02,
    0x08, 0x6c, 0x6f, 0x61, 0x64, 0x5f, 0x74, 0x69, 0x64,
    0x00, 0x03,
    0x08, 0x6c, 0x6f, 0x61, 0x64, 0x5f, 0x61, 0x72, 0x67,
    0x00, 0x04,

    /* bodies */
    0x0a, 0x29, 0x04,

    /* wasi_thread_start(tid, arg): mem[0]=tid; mem[4]=arg */
    0x10, 0x00,
    0x41, 0x00, 0x20, 0x00, 0x36, 0x02, 0x00,
    0x41, 0x04, 0x20, 0x01, 0x36, 0x02, 0x00,
    0x0b,

    /* spawn(): return thread-spawn(42) */
    0x06, 0x00,
    0x41, 0x2a, 0x10, 0x00, 0x0b,

    /* load_tid(): return mem[0] */
    0x07, 0x00,
    0x41, 0x00, 0x28, 0x02, 0x00, 0x0b,

    /* load_arg(): return mem[4] */
    0x07, 0x00,
    0x41, 0x04, 0x28, 0x02, 0x00, 0x0b
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

static void test_spawn_uses_fresh_sibling_and_shared_import(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    cflow_executor executor = {0};
    turbowasm_wasi_threads_config config = {0};
    int32_t tid;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               threaded_consumer,
               sizeof(threaded_consumer)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(cflow_executor_worker_init_with_capacity(
               &executor, 2u, 4u));
    config.executor = &executor;
    config.capacity = 2u;
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
               &root, &consumer_module, &linker) == TURBOWASM_OK);

    /* Linker lifetime is not needed by sibling spawn. */
    turbowasm_linker_destroy(&linker);

    tid = invoke_i32(&root, 2u);
    assert(tid > 0);
    assert((uint32_t)tid < (UINT32_C(1) << 29));

    assert(cflow_executor_wait_idle(&executor));
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    /* Child wrote through the exact imported shared-memory backing. */
    assert(invoke_i32(&root, 3u) == tid);
    assert(invoke_i32(&root, 4u) == 42);

    turbowasm_instance_destroy(&root);
    assert(turbowasm_wasi_threads_destroy(&threads));
    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_missing_threads_capability_fails_link(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               threaded_consumer,
               sizeof(threaded_consumer)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               one_char_name("m"),
               &provider) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &root, &consumer_module, &linker) ==
           TURBOWASM_LINK_ERROR);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_spawn_uses_fresh_sibling_and_shared_import();
    test_missing_threads_capability_fails_link();
    return 0;
}
