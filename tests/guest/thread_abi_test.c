#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_threads.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* env.memory: shared 16 pages, matching the fixture's fixed 1 MiB budget. */
static const uint8_t provider_bytes[] = {
    0,97,115,109,1,0,0,0,
    5,4,1,3,16,16,
    7,10,1,6,'m','e','m','o','r','y',2,0
};
static turbowasm_module module, provider_module;
static uint8_t *module_bytes;
static turbowasm_instance root, provider;
static turbowasm_linker linker;
static turbowasm_wasi_threads threads;
static cmeta_mutex_t mutex;
static cmeta_cond_t condition;
static unsigned entered[2], self_destroy_rejections;
static bool released[2], timed_out;

static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, strlen(s)};
}

static int32_t call(const char *symbol, const int *argument) {
    uint32_t index = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) index = e->item_index;
    }
    check_not_equal(index, UINT32_MAX);
    turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32}, result = {0};
    if (argument) arg.as.i32 = *argument;
    size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_execution_options options = {.has_fuel_limit = true, .fuel = 1000000};
    check_equal(turbowasm_instance_invoke_with_options(&root, index,
        argument ? &arg : NULL, argument ? 1 : 0, &result, 1, &count, &trap, &options), TURBOWASM_OK);
    check_equal(count, (size_t)1);
    check_equal(trap, TURBOWASM_TRAP_NONE);
    return result.as.i32;
}

static turbowasm_status hold(void *context, turbowasm_host_call *host,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)host; (void)results; (void)capacity;
    if (argc != 1 || !args || (uint32_t)args[0].as.i32 > 1u) return TURBOWASM_INVALID_ARGUMENT;
    unsigned phase = (unsigned)args[0].as.i32;
    bool rejected = !turbowasm_wasi_threads_destroy(&threads);
    cmeta_mutex_lock(&mutex);
    self_destroy_rejections += rejected;
    ++entered[phase];
    cmeta_cond_broadcast(&condition);
    while (!released[phase]) {
        if (cmeta_cond_timedwait(&condition, &mutex, UINT64_C(10000000000)) != 0) {
            timed_out = true;
            break;
        }
    }
    cmeta_mutex_unlock(&mutex);
    *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static void await_phase(unsigned phase) {
    cmeta_mutex_lock(&mutex);
    while (entered[phase] < 2 && !timed_out) {
        if (cmeta_cond_timedwait(&condition, &mutex, UINT64_C(10000000000)) != 0) {
            timed_out = true;
            break;
        }
    }
    bool success = !timed_out && entered[phase] == 2;
    cmeta_mutex_unlock(&mutex);
    check_true(success);
}
static void release_phase(unsigned phase) {
    cmeta_mutex_lock(&mutex);
    released[phase] = true;
    cmeta_cond_broadcast(&condition);
    cmeta_mutex_unlock(&mutex);
}

spec("Metallic thread ABI") {
    before_all() {
        FILE *f = fopen(GUEST_THREAD_ABI_PATH, "rb");
        check_not_null(f);
        check_equal(fseek(f, 0, SEEK_END), 0);
        long size = ftell(f);
        check_true(size > 0 && size <= 1024 * 1024);
        check_equal(fseek(f, 0, SEEK_SET), 0);
        module_bytes = malloc((size_t)size);
        check_not_null(module_bytes);
        size_t read = fread(module_bytes, 1, (size_t)size, f);
        fclose(f);
        check_equal(read, (size_t)size);
        check_equal(turbowasm_module_load_borrowed(&module, module_bytes, (size_t)size), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed(&provider_module, provider_bytes, sizeof(provider_bytes)), TURBOWASM_OK);
    }
    after_all() {
        turbowasm_module_destroy(&module); free(module_bytes);
        turbowasm_module_destroy(&provider_module);
    }
    it("rejects invalid pool admission without publishing an owner") {
        check_equal(turbowasm_wasi_threads_init_pool(NULL, 2), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 0), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, SIZE_MAX), TURBOWASM_INVALID_ARGUMENT);
        check_null(threads.impl);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 1), TURBOWASM_OK);
        void *owner = threads.impl;
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 2), TURBOWASM_INVALID_ARGUMENT);
        check_equal(threads.impl, owner);
        check_true(turbowasm_wasi_threads_destroy(&threads));
        check_null(threads.impl);
    }
    it("preserves TLS, stacks, data and SJLJ across saturated sibling instances") {
        cmeta_mutex_init(&mutex); cmeta_cond_init(&condition);
        check_not_null(mutex); check_not_null(condition);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 2), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&provider, &provider_module), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("env"), &provider), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_define(&threads, &linker), TURBOWASM_OK);
        turbowasm_value_kind i32 = TURBOWASM_VALUE_I32;
        turbowasm_host_function_type type = {&i32, 1, NULL, 0};
        check_equal(turbowasm_linker_define_host_function(&linker, name("test"), name("hold"), &type, hold, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&root, &module, &linker), TURBOWASM_OK);
        check_equal(call("setup", NULL), 0);
        for (unsigned round = 0; round < 3; ++round) {
            cmeta_mutex_lock(&mutex);
            entered[0] = entered[1] = self_destroy_rejections = 0;
            released[0] = released[1] = timed_out = false;
            cmeta_mutex_unlock(&mutex);
            int index = 0;
            check_true(call("spawn", &index) > 0);
            index = 1;
            check_true(call("spawn", &index) > 0);
            await_phase(0);
            check_equal(turbowasm_wasi_threads_active(&threads), (size_t)2);
            check_false(turbowasm_wasi_threads_destroy(&threads));
            index = -1;
            check_equal(call("spawn", &index), TURBOWASM_WASI_THREADS_SPAWN_CAPACITY);
            check_equal(call("root_intact", NULL), 1);
            release_phase(0);
            await_phase(1);
            release_phase(1);
            /* Completion is finite; the CTest timeout is the deadlock guard.
             * active includes the child C frame and runtime finalizer. */
            while (turbowasm_wasi_threads_active(&threads)) cmeta_thread_yield();
            check_false(turbowasm_wasi_threads_group_fatal(&threads, NULL, NULL));
            check_equal(call("results", NULL), 0);
            check_equal(self_destroy_rejections, 4u);
            check_false(timed_out);
        }
        turbowasm_instance_destroy(&root);
        check_true(turbowasm_wasi_threads_destroy(&threads));
        turbowasm_linker_destroy(&linker);
        turbowasm_instance_destroy(&provider);
        cmeta_cond_destroy(&condition); cmeta_mutex_destroy(&mutex);
    }
}
