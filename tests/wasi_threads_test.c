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

static const uint8_t nested_thread_consumer[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x03, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x60, 0x00, 0x01,
    0x7f, 0x02, 0x1f, 0x02, 0x04, 0x77, 0x61, 0x73, 0x69, 0x0c, 0x74, 0x68,
    0x72, 0x65, 0x61, 0x64, 0x2d, 0x73, 0x70, 0x61, 0x77, 0x6e, 0x00, 0x00,
    0x01, 0x6d, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x03, 0x01, 0x01, 0x03, 0x05,
    0x04, 0x01, 0x00, 0x02, 0x02, 0x07, 0x3c, 0x04, 0x11, 0x77, 0x61, 0x73,
    0x69, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x5f, 0x73, 0x74, 0x61,
    0x72, 0x74, 0x00, 0x01, 0x09, 0x73, 0x70, 0x61, 0x77, 0x6e, 0x5f, 0x61,
    0x72, 0x67, 0x00, 0x02, 0x0b, 0x6c, 0x6f, 0x61, 0x64, 0x5f, 0x6e, 0x65,
    0x73, 0x74, 0x65, 0x64, 0x00, 0x03, 0x0a, 0x6c, 0x6f, 0x61, 0x64, 0x5f,
    0x63, 0x68, 0x69, 0x6c, 0x64, 0x00, 0x04, 0x0a, 0x3a, 0x04, 0x21, 0x01,
    0x01, 0x7f, 0x20, 0x01, 0x41, 0x01, 0x46, 0x04, 0x40, 0x41, 0x00, 0x10,
    0x00, 0x21, 0x02, 0x41, 0x00, 0x20, 0x02, 0x36, 0x02, 0x00, 0x05, 0x41,
    0x04, 0x20, 0x00, 0x36, 0x02, 0x00, 0x0b, 0x0b, 0x06, 0x00, 0x20, 0x00,
    0x10, 0x00, 0x0b, 0x07, 0x00, 0x41, 0x00, 0x28, 0x02, 0x00, 0x0b, 0x07,
    0x00, 0x41, 0x04, 0x28, 0x02, 0x00, 0x0b
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


static int32_t invoke_i32_arg(
    turbowasm_instance *instance,
    uint32_t function_index,
    int32_t argument_value) {
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = argument_value;
    assert(turbowasm_instance_invoke(
               instance, function_index,
               &argument, 1u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

typedef struct executor_blocker {
    salts_mutex_t mutex;
    salts_cond_t condition;
    bool entered;
    bool release;
} executor_blocker;

static void executor_blocker_run(void *user) {
    executor_blocker *blocker = (executor_blocker *)user;

    assert(blocker != NULL);
    salts_mutex_lock(&blocker->mutex);
    blocker->entered = true;
    salts_cond_signal(&blocker->condition);
    while (!blocker->release)
        salts_cond_wait(&blocker->condition, &blocker->mutex);
    salts_mutex_unlock(&blocker->mutex);
}

static void executor_blocker_init(executor_blocker *blocker) {
    assert(blocker != NULL);
    *blocker = (executor_blocker){0};
    salts_mutex_init(&blocker->mutex);
    salts_cond_init(&blocker->condition);
    assert(blocker->mutex != NULL);
    assert(blocker->condition != NULL);
}

static void executor_blocker_wait_entered(executor_blocker *blocker) {
    salts_mutex_lock(&blocker->mutex);
    while (!blocker->entered)
        salts_cond_wait(&blocker->condition, &blocker->mutex);
    salts_mutex_unlock(&blocker->mutex);
}

static void executor_blocker_release(executor_blocker *blocker) {
    salts_mutex_lock(&blocker->mutex);
    blocker->release = true;
    salts_cond_signal(&blocker->condition);
    salts_mutex_unlock(&blocker->mutex);
}

static void executor_blocker_destroy(executor_blocker *blocker) {
    salts_cond_destroy(&blocker->condition);
    salts_mutex_destroy(&blocker->mutex);
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



static void test_slot_capacity_rejects_and_recovers(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    cflow_executor executor = {0};
    executor_blocker blocker;
    int32_t first_tid;
    int32_t recovered_tid;

    executor_blocker_init(&blocker);
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
               &executor, 1u, 4u));
    assert(cflow_executor_try_post(
               &executor,
               executor_blocker_run,
               &blocker) == CFLOW_ADMISSION_ACCEPTED);
    executor_blocker_wait_entered(&blocker);

    config.executor = &executor;
    config.capacity = 1u;
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
    turbowasm_linker_destroy(&linker);

    first_tid = invoke_i32(&root, 2u);
    assert(first_tid > 0);
    assert(turbowasm_wasi_threads_active(&threads) == 1u);
    assert(invoke_i32(&root, 2u) ==
           TURBOWASM_WASI_THREADS_SPAWN_CAPACITY);

    executor_blocker_release(&blocker);
    assert(cflow_executor_wait_idle(&executor));
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    recovered_tid = invoke_i32(&root, 2u);
    assert(recovered_tid > first_tid);
    assert(cflow_executor_wait_idle(&executor));
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    turbowasm_instance_destroy(&root);
    assert(turbowasm_wasi_threads_destroy(&threads));
    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    executor_blocker_destroy(&blocker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_executor_rejection_rolls_back_child(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    cflow_executor executor = {0};

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
               &executor, 1u, 2u));

    config.executor = &executor;
    config.capacity = 1u;
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
    turbowasm_linker_destroy(&linker);

    assert(cflow_executor_shutdown(&executor));
    assert(invoke_i32(&root, 2u) ==
           TURBOWASM_WASI_THREADS_SPAWN_EXECUTOR);
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    turbowasm_instance_destroy(&root);
    assert(turbowasm_wasi_threads_destroy(&threads));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_nested_child_spawn_reuses_group_linkage(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance root = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    cflow_executor executor = {0};
    int32_t parent_tid;
    int32_t nested_tid;
    int32_t nested_observed_tid;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               nested_thread_consumer,
               sizeof(nested_thread_consumer)) == TURBOWASM_OK);
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
    turbowasm_linker_destroy(&linker);

    parent_tid = invoke_i32_arg(&root, 2u, 1);
    assert(parent_tid > 0);
    assert(cflow_executor_wait_idle(&executor));

    nested_tid = invoke_i32(&root, 3u);
    nested_observed_tid = invoke_i32(&root, 4u);
    assert(nested_tid > 0);
    assert(nested_tid != parent_tid);
    assert(nested_observed_tid == nested_tid);
    assert(turbowasm_wasi_threads_active(&threads) == 0u);

    turbowasm_instance_destroy(&root);
    assert(turbowasm_wasi_threads_destroy(&threads));
    assert(cflow_executor_shutdown(&executor));
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_spawn_uses_fresh_sibling_and_shared_import();
    test_missing_threads_capability_fails_link();
    test_slot_capacity_rejects_and_recovers();
    test_executor_rejection_rolls_back_child();
    test_nested_child_spawn_reuses_group_linkage();
    return 0;
}
