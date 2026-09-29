#include <turbowasm/turbowasm.h>

#include "atomic.h"
#include "instance_internal.h"

#include <salts/thread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t shared_grow_module[] = {
    WASM_HEADER,
    /* shared memory min=1 max=2 */
    0x05, 0x04,
    0x01, 0x03, 0x01, 0x02
};

static const uint8_t two_shared_memories_module[] = {
    WASM_HEADER,
    /* two shared memories, each min=1 max=1 */
    0x05, 0x07,
    0x02,
    0x03, 0x01, 0x01,
    0x03, 0x01, 0x01
};

static turbowasm_instance_impl *instance_impl(
    turbowasm_instance *instance) {
    assert(instance != NULL);
    assert(instance->impl != NULL);
    return (turbowasm_instance_impl *)instance->impl;
}

static const turbowasm_atomic_descriptor *atomic_descriptor(
    uint32_t opcode) {
    const turbowasm_atomic_descriptor *descriptor =
        turbowasm_atomic_descriptor_find(opcode);
    assert(descriptor != NULL);
    return descriptor;
}

static void atomic_store_i32(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t value) {
    uint64_t old = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_memory_atomic(
               instance,
               memory_index,
               0u,
               0u,
               atomic_descriptor(0x17u),
               value,
               0u,
               0u,
               &old,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static uint32_t atomic_load_i32(
    turbowasm_instance_impl *instance,
    uint32_t memory_index) {
    uint64_t old = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_memory_atomic(
               instance,
               memory_index,
               0u,
               0u,
               atomic_descriptor(0x10u),
               0u,
               0u,
               0u,
               &old,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    return (uint32_t)old;
}

typedef struct wait_race_context {
    turbowasm_instance_impl *instance;
    turbowasm_status status;
    turbowasm_trap trap;
    uint32_t result;
} wait_race_context;

static void wait_race_thread(void *argument) {
    wait_race_context *context =
        (wait_race_context *)argument;

    context->trap = TURBOWASM_TRAP_NONE;
    context->result = UINT32_MAX;
    context->status = turbowasm_instance_memory_wait(
        context->instance,
        0u,
        0u,
        0u,
        4u,
        0u,
        INT64_C(100000000),
        &context->result,
        &context->trap);
}

static void test_wait_registration_notify_race(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    uint32_t iteration;

    assert(turbowasm_module_load_borrowed(
               &module,
               shared_grow_module,
               sizeof(shared_grow_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = instance_impl(&instance);

    for (iteration = 0u; iteration < 250u; ++iteration) {
        wait_race_context waiter = {0};
        salts_thread_t thread = NULL;
        uint32_t woken = UINT32_MAX;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;

        atomic_store_i32(impl, 0u, 0u);
        waiter.instance = impl;

        assert(salts_thread_create(
                   &thread,
                   wait_race_thread,
                   &waiter) == 0);

        if ((iteration & 1u) != 0u)
            salts_thread_yield();

        /*
         * The wait expected-value read + waiter publication is one SC
         * registration transaction relative to this store. Therefore the
         * waiter must either observe 1 (not-equal) or already be registered
         * when notify runs. A timeout is a lost-wakeup regression.
         */
        atomic_store_i32(impl, 0u, 1u);
        assert(turbowasm_instance_memory_notify(
                   impl,
                   0u,
                   0u,
                   0u,
                   1u,
                   &woken,
                   &trap) == TURBOWASM_OK);
        assert(trap == TURBOWASM_TRAP_NONE);
        assert(woken <= 1u);

        assert(salts_thread_join(&thread) == 0);
        assert(waiter.status == TURBOWASM_OK);
        assert(waiter.trap == TURBOWASM_TRAP_NONE);
        assert(waiter.result == 0u || waiter.result == 1u);
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

typedef struct atomic_increment_context {
    turbowasm_instance_impl *instance;
    uint32_t iterations;
    turbowasm_status status;
} atomic_increment_context;

static void atomic_increment_thread(void *argument) {
    atomic_increment_context *context =
        (atomic_increment_context *)argument;
    const turbowasm_atomic_descriptor *add =
        atomic_descriptor(0x1eu);
    uint32_t iteration;

    context->status = TURBOWASM_OK;
    for (iteration = 0u;
         iteration < context->iterations;
         ++iteration) {
        uint64_t old = 0u;
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;

        context->status = turbowasm_instance_memory_atomic(
            context->instance,
            0u,
            0u,
            0u,
            add,
            1u,
            0u,
            0u,
            &old,
            &trap);
        if (context->status != TURBOWASM_OK ||
            trap != TURBOWASM_TRAP_NONE)
            return;
    }
}

typedef struct size_reader_context {
    turbowasm_instance_impl *instance;
    uint32_t iterations;
    uint32_t minimum_seen;
    uint32_t maximum_seen;
    turbowasm_status status;
} size_reader_context;

static void size_reader_thread(void *argument) {
    size_reader_context *context =
        (size_reader_context *)argument;
    uint32_t iteration;

    context->minimum_seen = UINT32_MAX;
    context->maximum_seen = 0u;
    context->status = TURBOWASM_OK;

    for (iteration = 0u;
         iteration < context->iterations;
         ++iteration) {
        uint32_t pages = 0u;
        context->status = turbowasm_instance_memory_size(
            context->instance, 0u, &pages);
        if (context->status != TURBOWASM_OK)
            return;
        if (pages < context->minimum_seen)
            context->minimum_seen = pages;
        if (pages > context->maximum_seen)
            context->maximum_seen = pages;
        if ((iteration & 63u) == 0u)
            salts_thread_yield();
    }
}

static void test_grow_size_atomic_stress(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    atomic_increment_context atomic = {0};
    size_reader_context size = {0};
    salts_thread_t atomic_thread = NULL;
    salts_thread_t size_thread = NULL;
    uint32_t previous_pages = UINT32_MAX;
    uint32_t final_pages = 0u;
    uint32_t yield_count;

    assert(turbowasm_module_load_borrowed(
               &module,
               shared_grow_module,
               sizeof(shared_grow_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = instance_impl(&instance);

    atomic.instance = impl;
    atomic.iterations = 20000u;
    size.instance = impl;
    size.iterations = 20000u;

    assert(salts_thread_create(
               &atomic_thread,
               atomic_increment_thread,
               &atomic) == 0);
    assert(salts_thread_create(
               &size_thread,
               size_reader_thread,
               &size) == 0);

    for (yield_count = 0u; yield_count < 32u; ++yield_count)
        salts_thread_yield();

    assert(turbowasm_instance_memory_grow(
               impl,
               0u,
               1u,
               &previous_pages) == TURBOWASM_OK);
    assert(previous_pages == 1u);

    assert(salts_thread_join(&atomic_thread) == 0);
    assert(salts_thread_join(&size_thread) == 0);
    assert(atomic.status == TURBOWASM_OK);
    assert(size.status == TURBOWASM_OK);
    assert(size.minimum_seen >= 1u);
    assert(size.maximum_seen <= 2u);

    assert(turbowasm_instance_memory_size(
               impl, 0u, &final_pages) == TURBOWASM_OK);
    assert(final_pages == 2u);
    assert(atomic_load_i32(impl, 0u) == atomic.iterations);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

typedef struct sc_litmus_context {
    turbowasm_instance_impl *instance;
    uint32_t store_memory;
    uint32_t load_memory;
    uint32_t observed;
    turbowasm_status status;
    turbowasm_trap trap;
} sc_litmus_context;

static void sc_litmus_thread(void *argument) {
    sc_litmus_context *context =
        (sc_litmus_context *)argument;
    uint64_t old = 0u;

    context->trap = TURBOWASM_TRAP_NONE;
    context->status = turbowasm_instance_memory_atomic(
        context->instance,
        context->store_memory,
        0u,
        0u,
        atomic_descriptor(0x17u),
        1u,
        0u,
        0u,
        &old,
        &context->trap);
    if (context->status != TURBOWASM_OK ||
        context->trap != TURBOWASM_TRAP_NONE)
        return;

    context->status = turbowasm_threads_sc_fence();
    if (context->status != TURBOWASM_OK)
        return;

    context->status = turbowasm_instance_memory_atomic(
        context->instance,
        context->load_memory,
        0u,
        0u,
        atomic_descriptor(0x10u),
        0u,
        0u,
        0u,
        &old,
        &context->trap);
    if (context->status == TURBOWASM_OK &&
        context->trap == TURBOWASM_TRAP_NONE)
        context->observed = (uint32_t)old;
}

static void test_cross_backing_sc_fence_stress(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    uint32_t iteration;

    assert(turbowasm_module_load_borrowed(
               &module,
               two_shared_memories_module,
               sizeof(two_shared_memories_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = instance_impl(&instance);

    for (iteration = 0u; iteration < 250u; ++iteration) {
        sc_litmus_context first = {0};
        sc_litmus_context second = {0};
        salts_thread_t first_thread = NULL;
        salts_thread_t second_thread = NULL;

        atomic_store_i32(impl, 0u, 0u);
        atomic_store_i32(impl, 1u, 0u);

        first.instance = impl;
        first.store_memory = 0u;
        first.load_memory = 1u;
        second.instance = impl;
        second.store_memory = 1u;
        second.load_memory = 0u;

        assert(salts_thread_create(
                   &first_thread,
                   sc_litmus_thread,
                   &first) == 0);
        assert(salts_thread_create(
                   &second_thread,
                   sc_litmus_thread,
                   &second) == 0);
        assert(salts_thread_join(&first_thread) == 0);
        assert(salts_thread_join(&second_thread) == 0);

        assert(first.status == TURBOWASM_OK);
        assert(second.status == TURBOWASM_OK);
        assert(first.trap == TURBOWASM_TRAP_NONE);
        assert(second.trap == TURBOWASM_TRAP_NONE);

        /*
         * With one Runtime-wide SC order, both loads cannot be ordered before
         * both stores while preserving each thread's store/fence/load order.
         */
        assert(!(first.observed == 0u && second.observed == 0u));
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_wait_registration_notify_race();
    test_grow_size_atomic_stress();
    test_cross_backing_sc_fence_stress();
    return 0;
}
