#include <turbowasm/turbowasm.h>
#include "store_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <salts/thread.h>

typedef struct owner_probe {
    turbowasm_store *store;
    turbowasm_execution *execution;
    turbowasm_status collect_status;
    turbowasm_status resume_status;
} owner_probe;
static void check_other_thread(void *context) {
    owner_probe *probe = context;
    probe->collect_status = turbowasm_store_collect(probe->store);
    probe->resume_status = turbowasm_execution_resume(probe->execution, NULL);
}

typedef struct allocation_probe {
    size_t live;
    bool fail;
} allocation_probe;
static void *probe_allocate(void *context, size_t bytes) {
    allocation_probe *probe = context;
    void *pointer = probe->fail ? NULL : malloc(bytes);
    if (pointer != NULL)
        ++probe->live;
    return pointer;
}
static void probe_free(void *context, void *pointer) {
    allocation_probe *probe = context;
    if (pointer != NULL) {
        assert(probe->live != 0u);
        --probe->live;
        free(pointer);
    }
}

/* make(i32) builds a self-cycle; read(node) reads its scalar field;
 * churn(node,i32) allocates and discards that many nodes, then reads node. */
static const uint8_t lifecycle_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1b, 0x04, 0x5f, 0x02, 0x63, 0x00,
    0x01, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x64, 0x00, 0x60, 0x01, 0x64, 0x00, 0x01, 0x7f,
    0x60, 0x02, 0x64, 0x00, 0x7f, 0x01, 0x7f, 0x03, 0x04, 0x03, 0x01, 0x02, 0x03, 0x07, 0x17,
    0x03, 0x04, 0x6d, 0x61, 0x6b, 0x65, 0x00, 0x00, 0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x01,
    0x05, 0x63, 0x68, 0x75, 0x72, 0x6e, 0x00, 0x02, 0x0a, 0x3f, 0x03, 0x17, 0x01, 0x01, 0x63,
    0x00, 0xd0, 0x00, 0x20, 0x00, 0xfb, 0x00, 0x00, 0x22, 0x01, 0x20, 0x01, 0xfb, 0x05, 0x00,
    0x00, 0x20, 0x01, 0xd4, 0x0b, 0x08, 0x00, 0x20, 0x00, 0xfb, 0x02, 0x00, 0x01, 0x0b, 0x1c,
    0x00, 0x03, 0x40, 0xd0, 0x00, 0x20, 0x01, 0xfb, 0x00, 0x00, 0x1a, 0x20, 0x01, 0x41, 0x01,
    0x6b, 0x22, 0x01, 0x0d, 0x00, 0x0b, 0x20, 0x00, 0xfb, 0x02, 0x00, 0x01, 0x0b};

/* An externalized struct initializes a global and table; run keeps another
 * struct in a local across a host callback; read casts the table entry back. */
static const uint8_t host_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x03, 0x5f, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x08, 0x01, 0x01, 0x68, 0x02, 0x67, 0x63,
    0x00, 0x01, 0x03, 0x03, 0x02, 0x02, 0x02, 0x04, 0x0e, 0x01, 0x40, 0x00, 0x6f, 0x00, 0x01,
    0x41, 0x2a, 0xfb, 0x00, 0x00, 0xfb, 0x1b, 0x0b, 0x06, 0x0b, 0x01, 0x6f, 0x00, 0x41, 0x2a,
    0xfb, 0x00, 0x00, 0xfb, 0x1b, 0x0b, 0x07, 0x0e, 0x02, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x01,
    0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x02, 0x0a, 0x26, 0x02, 0x14, 0x01, 0x01, 0x63, 0x00,
    0x41, 0x2a, 0xfb, 0x00, 0x00, 0x21, 0x00, 0x10, 0x00, 0x20, 0x00, 0xfb, 0x02, 0x00, 0x00,
    0x0b, 0x0f, 0x00, 0x41, 0x00, 0x25, 0x00, 0xfb, 0x1a, 0xfb, 0x16, 0x00, 0xfb, 0x02, 0x00,
    0x00, 0x0b, 0x00, 0x26, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x01, 0x05, 0x01, 0x00, 0x02, 0x67,
    0x63, 0x02, 0x06, 0x01, 0x01, 0x01, 0x00, 0x01, 0x6e, 0x04, 0x04, 0x01, 0x00, 0x01, 0x6e,
    0x05, 0x04, 0x01, 0x00, 0x01, 0x74, 0x07, 0x04, 0x01, 0x00, 0x01, 0x65,
};

typedef struct host_gc_probe {
    turbowasm_store *store;
    turbowasm_instance *producer;
    unsigned calls;
} host_gc_probe;

static turbowasm_status collect_in_host(void *context, turbowasm_host_call *call,
                                        const turbowasm_value *arguments, size_t argument_count,
                                        turbowasm_value *results, size_t result_capacity,
                                        size_t *result_count, turbowasm_trap *trap) {
    enum { PAYLOAD = 42, CHURN_COUNT = 100 };
    host_gc_probe *probe = context;
    turbowasm_value scalar = {0}, node = {0}, args[2], result = {0};
    size_t count = 0u;
    (void)call;
    (void)arguments;
    (void)results;
    (void)result_capacity;
    assert(argument_count == 0u);
    ++probe->calls;
    assert(turbowasm_store_collect(probe->store) == TURBOWASM_OK);
    scalar.kind = TURBOWASM_VALUE_I32;
    scalar.as.i32 = PAYLOAD;
    assert(turbowasm_instance_invoke(probe->producer, 0u, &scalar, 1u, &node, 1u, &count, trap) ==
           TURBOWASM_OK);
    args[0] = node;
    args[1] = scalar;
    args[1].as.i32 = CHURN_COUNT;
    assert(turbowasm_instance_invoke(probe->producer, 2u, args, 2u, &result, 1u, &count, trap) ==
           TURBOWASM_OK);
    assert(result.as.i32 == PAYLOAD);
    *result_count = 0u;
    return TURBOWASM_OK;
}

static void test_nested_host_collection_and_managed_external_roots(void) {
    enum { OBJECT_LIMIT = 5, PAYLOAD = 42 };
    turbowasm_store store = {0};
    turbowasm_store_config config;
    turbowasm_module source = {0}, module = {0};
    turbowasm_instance producer = {0}, instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_host_function_type type = {0};
    turbowasm_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_store_stats stats;
    host_gc_probe probe = {&store, &producer, 0u};
    size_t count = 0u;
    turbowasm_name host = {(const uint8_t *)"h", 1u}, name = {(const uint8_t *)"gc", 2u};
    turbowasm_store_config_init(&config);
    config.max_objects = OBJECT_LIMIT;
    assert(turbowasm_store_create(&store, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(&source, lifecycle_module, sizeof(lifecycle_module)) ==
           TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(&module, host_module, sizeof(host_module)) ==
           TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(&linker, host, name, &type, collect_in_host,
                                                 &probe) == TURBOWASM_OK);
    assert(turbowasm_instance_create_in_store(&producer, &source, NULL, &store) == TURBOWASM_OK);
    assert(turbowasm_instance_create_in_store(&instance, &module, &linker, &store) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(&instance, 1u, NULL, 0u, &result, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(probe.calls == 1u && result.as.i32 == PAYLOAD);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK && stats.objects == 2u);
    assert(turbowasm_instance_invoke(&instance, 2u, NULL, 0u, &result, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(result.as.i32 == PAYLOAD);
    turbowasm_instance_destroy(&instance);
    turbowasm_instance_destroy(&producer);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_module_destroy(&source);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_OK);
}

static void test_execution_roots_and_owned_types(void) {
    enum { OBJECT_LIMIT = 3, CHURN_COUNT = 1000, PAYLOAD = 42 };
    turbowasm_store_config config;
    turbowasm_store store = {0};
    turbowasm_module producer_module = {0}, consumer_module = {0};
    turbowasm_instance producer = {0}, consumer = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    turbowasm_root root = {0};
    turbowasm_value input = {0}, node = {0}, args[2], result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_store_stats stats;
    size_t count = 0u;
    turbowasm_store_config_init(&config);
    config.max_objects = OBJECT_LIMIT;
    assert(turbowasm_store_create(&store, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(&producer_module, lifecycle_module,
                                          sizeof(lifecycle_module)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(&consumer_module, lifecycle_module,
                                          sizeof(lifecycle_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&producer, &producer_module) == TURBOWASM_INVALID_ARGUMENT);
    assert(producer.impl == NULL);
    assert(turbowasm_instance_create_in_store(&producer, &producer_module, NULL, &store) ==
           TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK);
    {
        size_t type_bytes = stats.bytes;
        assert(turbowasm_instance_create_in_store(&consumer, &consumer_module, NULL, &store) ==
               TURBOWASM_OK);
        assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK &&
               stats.bytes == type_bytes);
    }
    input.kind = TURBOWASM_VALUE_I32;
    input.as.i32 = PAYLOAD;
    assert(turbowasm_instance_invoke(&producer, 0u, &input, 1u, &node, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(turbowasm_root_retain(&store, &node, &root) == TURBOWASM_OK);
    turbowasm_instance_destroy(&producer);
    turbowasm_module_destroy(&producer_module);
    assert(turbowasm_instance_invoke(&consumer, 1u, &node, 1u, &result, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(result.as.i32 == PAYLOAD);
    args[0] = node;
    args[1] = input;
    args[1].as.i32 = CHURN_COUNT;
    assert(turbowasm_execution_create(&execution, &consumer, 2u, args, 2u) == TURBOWASM_OK);
    {
        cmeta_thread_t thread = NULL;
        owner_probe probe = {&store, &execution, TURBOWASM_OK, TURBOWASM_OK};
        assert(cmeta_thread_create(&thread, check_other_thread, &probe) == 0);
        assert(cmeta_thread_join(&thread) == 0);
        assert(probe.collect_status == TURBOWASM_INVALID_ARGUMENT);
        assert(probe.resume_status == TURBOWASM_INVALID_ARGUMENT);
        assert(turbowasm_execution_state_get(&execution) == TURBOWASM_EXECUTION_READY);
    }
    assert(turbowasm_root_release(&root) == TURBOWASM_OK);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    options.has_fuel_limit = true;
    options.fuel = 3u;
    assert(turbowasm_execution_resume(&execution, &options) == TURBOWASM_YIELDED);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(&execution, NULL) == TURBOWASM_OK);
    assert(turbowasm_execution_result_at(&execution, 0u)->as.i32 == PAYLOAD);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK);
    assert(stats.collections > 1u && stats.objects <= OBJECT_LIMIT);
    turbowasm_execution_destroy(&execution);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK && stats.objects == 0u);
    turbowasm_instance_destroy(&consumer);
    turbowasm_module_destroy(&consumer_module);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_OK);
}

static void test_cycles_roots_and_stale_handles(void) {
    enum { OBJECT_LIMIT = 2, ROOT_LIMIT = 1 };
    turbowasm_store store = {0}, other = {0};
    turbowasm_store_config config;
    turbowasm_root root = {0}, excess = {0};
    turbowasm_value first, second, replacement, retained;
    turbowasm_store_stats stats;
    turbowasm_store_config_init(&config);
    config.max_objects = OBJECT_LIMIT;
    config.max_roots = ROOT_LIMIT;
    assert(turbowasm_store_create(&store, &config) == TURBOWASM_OK);
    assert(turbowasm_store_create(&other, &config) == TURBOWASM_OK);
    assert(turbowasm_gc_allocate(store.impl, NULL, 1u, &first) == TURBOWASM_OK);
    assert(turbowasm_root_retain(&store, &first, &root) == TURBOWASM_OK);
    assert(turbowasm_gc_allocate(store.impl, NULL, 1u, &second) == TURBOWASM_OK);
    turbowasm_gc_resolve(store.impl, first.as.gcref)->values[0] = second;
    turbowasm_gc_resolve(store.impl, second.as.gcref)->values[0] = first;
    assert(turbowasm_root_retain(&other, &first, &excess) == TURBOWASM_TYPE_MISMATCH);
    assert(turbowasm_root_retain(&store, &second, &excess) == TURBOWASM_OUT_OF_MEMORY);
    assert(excess.impl == NULL);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK);
    assert(stats.objects == OBJECT_LIMIT && stats.roots == ROOT_LIMIT);
    assert(turbowasm_gc_allocate(store.impl, NULL, 0u, &replacement) == TURBOWASM_OUT_OF_MEMORY);
    assert(turbowasm_root_get(&root, &retained) == TURBOWASM_OK);
    assert(retained.as.gcref.handle == first.as.gcref.handle);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_root_release(&root) == TURBOWASM_OK);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &stats) == TURBOWASM_OK);
    assert(stats.objects == 0u && stats.roots == 0u);
    assert(turbowasm_gc_allocate(store.impl, NULL, 0u, &replacement) == TURBOWASM_OK);
    assert(turbowasm_root_retain(&store, &first, &root) == TURBOWASM_TYPE_MISMATCH);
    assert(turbowasm_root_retain(&store, &second, &root) == TURBOWASM_TYPE_MISMATCH);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_OK);
    assert(turbowasm_store_destroy(&other) == TURBOWASM_OK);
}

static void test_instance_lifetime_and_budget(void) {
    static const uint8_t empty[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    turbowasm_store store = {0};
    turbowasm_store_config config;
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_store_config_init(&config);
    config.max_bytes = 1u;
    assert(turbowasm_store_create(&store, &config) == TURBOWASM_OUT_OF_MEMORY);
    assert(store.impl == NULL);
    assert(turbowasm_store_create(&store, NULL) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(&module, empty, sizeof(empty)) == TURBOWASM_OK);
    assert(turbowasm_instance_create_in_store(&instance, &module, NULL, &store) == TURBOWASM_OK);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_INVALID_ARGUMENT);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_OK);
}

static void test_suspended_destruction_and_allocation_failure(void) {
    enum { PAYLOAD = 42, CHURN_COUNT = 100 };
    allocation_probe probe = {0};
    turbowasm_store_config config;
    turbowasm_store store = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value args[2] = {{0}}, node = {0}, result = {0};
    turbowasm_root root = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_store_stats before, after;
    size_t count = 0u;
    turbowasm_store_config_init(&config);
    config.runtime.allocator.context = &probe;
    config.runtime.allocator.allocate = probe_allocate;
    config.runtime.allocator.deallocate = probe_free;
    assert(turbowasm_store_create(&store, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed_with_config(&module, lifecycle_module,
                                                      sizeof(lifecycle_module),
                                                      &config.runtime) == TURBOWASM_OK);
    assert(turbowasm_instance_create_in_store(&instance, &module, NULL, &store) == TURBOWASM_OK);
    args[0].kind = TURBOWASM_VALUE_I32;
    args[0].as.i32 = PAYLOAD;
    assert(turbowasm_instance_invoke(&instance, 0u, args, 1u, &node, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &before) == TURBOWASM_OK);
    probe.fail = true;
    assert(turbowasm_root_retain(&store, &node, &root) == TURBOWASM_OUT_OF_MEMORY);
    assert(root.impl == NULL);
    assert(turbowasm_store_get_stats(&store, &after) == TURBOWASM_OK);
    assert(after.bytes == before.bytes && after.objects == before.objects &&
           after.roots == before.roots);
    probe.fail = false;
    assert(turbowasm_instance_invoke(&instance, 1u, &node, 1u, &result, 1u, &count, &trap) ==
           TURBOWASM_OK);
    assert(result.as.i32 == PAYLOAD);
    args[0] = node;
    args[1].kind = TURBOWASM_VALUE_I32;
    args[1].as.i32 = CHURN_COUNT;
    assert(turbowasm_execution_create(&execution, &instance, 2u, args, 2u) == TURBOWASM_OK);
    options.has_fuel_limit = true;
    options.fuel = 3u;
    assert(turbowasm_execution_resume(&execution, &options) == TURBOWASM_YIELDED);
    turbowasm_execution_destroy(&execution);
    assert(turbowasm_store_collect(&store) == TURBOWASM_OK);
    assert(turbowasm_store_get_stats(&store, &after) == TURBOWASM_OK && after.objects == 0u);
    assert(turbowasm_execution_create(&execution, &instance, 2u, args, 2u) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(execution.impl == NULL);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    assert(turbowasm_store_destroy(&store) == TURBOWASM_OK);
    assert(probe.live == 0u);
}

int main(void) {
    test_nested_host_collection_and_managed_external_roots();
    test_execution_roots_and_owned_types();
    test_cycles_roots_and_stale_handles();
    test_instance_lifetime_and_budget();
    test_suspended_destruction_and_allocation_failure();
    return 0;
}
