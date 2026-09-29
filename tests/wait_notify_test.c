#include <turbowasm/turbowasm.h>

#include "instance_internal.h"

#include <salts/thread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t shared_memory_module[] = {
    WASM_HEADER,
    0x05, 0x04,
    0x01, 0x03, 0x01, 0x01
};

static const uint8_t unshared_memory_module[] = {
    WASM_HEADER,
    0x05, 0x03,
    0x01, 0x00, 0x01
};

static turbowasm_instance_impl *instance_impl(
    turbowasm_instance *instance) {
    assert(instance != NULL);
    assert(instance->impl != NULL);
    return (turbowasm_instance_impl *)instance->impl;
}

static turbowasm_instance_memory *memory0(
    turbowasm_instance *instance) {
    turbowasm_instance_impl *impl =
        instance_impl(instance);
    assert(impl->memory_count == 1u);
    return &impl->memories[0];
}

static void test_direct_wait_results_and_traps(void) {
    turbowasm_module shared_module = {0};
    turbowasm_module unshared_module = {0};
    turbowasm_instance shared = {0};
    turbowasm_instance unshared = {0};
    uint32_t result = UINT32_MAX;
    uint32_t woken = UINT32_MAX;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t one = 1u;

    assert(turbowasm_module_load_borrowed(
               &shared_module,
               shared_memory_module,
               sizeof(shared_memory_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &shared, &shared_module) == TURBOWASM_OK);

    /* Loaded zero != expected one => not-equal without sleeping. */
    assert(turbowasm_instance_memory_wait(
               instance_impl(&shared),
               0u, 0u, 0u, 4u,
               one, -1,
               &result, &trap) == TURBOWASM_OK);
    assert(result == 1u);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Equal value + zero timeout => timed-out. */
    result = UINT32_MAX;
    assert(turbowasm_instance_memory_wait(
               instance_impl(&shared),
               0u, 0u, 0u, 4u,
               0u, 0,
               &result, &trap) == TURBOWASM_OK);
    assert(result == 2u);

    /* Positive timeout also expires without notify. */
    result = UINT32_MAX;
    assert(turbowasm_instance_memory_wait(
               instance_impl(&shared),
               0u, 0u, 0u, 8u,
               0u, INT64_C(1000000),
               &result, &trap) == TURBOWASM_OK);
    assert(result == 2u);

    /* Misalignment precedes all other wait/notify checks. */
    assert(turbowasm_instance_memory_wait(
               instance_impl(&shared),
               0u, 1u, 0u, 4u,
               0u, 0,
               &result, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_UNALIGNED_ATOMIC);

    assert(turbowasm_instance_memory_notify(
               instance_impl(&shared),
               0u, 1u, 0u, 1u,
               &woken, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_UNALIGNED_ATOMIC);

    /* Aligned end-of-memory address is OOB for 4-byte wait/notify. */
    assert(turbowasm_instance_memory_wait(
               instance_impl(&shared),
               0u, 65536u, 0u, 4u,
               0u, 0,
               &result, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    assert(turbowasm_instance_memory_notify(
               instance_impl(&shared),
               0u, 65536u, 0u, 1u,
               &woken, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    assert(turbowasm_module_load_borrowed(
               &unshared_module,
               unshared_memory_module,
               sizeof(unshared_memory_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &unshared, &unshared_module) == TURBOWASM_OK);

    assert(turbowasm_instance_memory_wait(
               instance_impl(&unshared),
               0u, 0u, 0u, 4u,
               0u, 0,
               &result, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_EXPECTED_SHARED_MEMORY);
    assert(strcmp(
               turbowasm_trap_string(trap),
               "expected_shared_memory") == 0);

    /* notify on unshared memory is valid and wakes zero waiters. */
    woken = UINT32_MAX;
    assert(turbowasm_instance_memory_notify(
               instance_impl(&unshared),
               0u, 0u, 0u, UINT32_MAX,
               &woken, &trap) == TURBOWASM_OK);
    assert(woken == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&unshared);
    turbowasm_module_destroy(&unshared_module);
    turbowasm_instance_destroy(&shared);
    turbowasm_module_destroy(&shared_module);
}

typedef struct direct_wait_context {
    turbowasm_instance_impl *instance;
    uint32_t result;
    turbowasm_status status;
    turbowasm_trap trap;
} direct_wait_context;

static void direct_wait_thread(void *argument) {
    direct_wait_context *context =
        (direct_wait_context *)argument;

    assert(context != NULL);
    context->result = UINT32_MAX;
    context->trap = TURBOWASM_TRAP_NONE;
    context->status = turbowasm_instance_memory_wait(
        context->instance,
        0u, 0u, 0u, 4u,
        0u, INT64_C(5000000000),
        &context->result,
        &context->trap);
}

static uint32_t waiter_count_get(
    turbowasm_instance_memory *memory) {
    uint32_t count;

    assert(memory != NULL);
    salts_mutex_lock(&memory->waiter_mutex);
    count = memory->waiter_count;
    salts_mutex_unlock(&memory->waiter_mutex);
    return count;
}

static void wait_for_waiter_count(
    turbowasm_instance_memory *memory,
    uint32_t expected) {
    uint32_t iteration;

    for (iteration = 0u; iteration < 100000u; ++iteration) {
        if (waiter_count_get(memory) == expected)
            return;
        salts_thread_yield();
    }
    assert(waiter_count_get(memory) == expected);
}

static void test_notify_count_and_spurious_signal(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_memory *memory;
    direct_wait_context first = {0};
    direct_wait_context second = {0};
    salts_thread_t first_thread = NULL;
    salts_thread_t second_thread = NULL;
    uint32_t woken = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t index;
    bool signaled = false;

    assert(turbowasm_module_load_borrowed(
               &module,
               shared_memory_module,
               sizeof(shared_memory_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    memory = memory0(&instance);

    first.instance = instance_impl(&instance);
    second.instance = instance_impl(&instance);
    assert(salts_thread_create(
               &first_thread,
               direct_wait_thread,
               &first) == 0);
    assert(salts_thread_create(
               &second_thread,
               direct_wait_thread,
               &second) == 0);

    wait_for_waiter_count(memory, 2u);

    /*
     * Deliberately signal one condition without setting notified. The waiter
     * may wake internally, but must loop and remain registered.
     */
    salts_mutex_lock(&memory->waiter_mutex);
    for (index = 0u; index < memory->waiter_capacity; ++index) {
        if (memory->waiters[index].active) {
            salts_cond_signal(
                &memory->waiters[index].condition);
            signaled = true;
            break;
        }
    }
    salts_mutex_unlock(&memory->waiter_mutex);
    assert(signaled);
    salts_sleep_ms(2u);
    assert(waiter_count_get(memory) == 2u);

    assert(turbowasm_instance_memory_notify(
               instance_impl(&instance),
               0u, 0u, 0u, 1u,
               &woken, &trap) == TURBOWASM_OK);
    assert(woken == 1u);
    wait_for_waiter_count(memory, 1u);

    assert(turbowasm_instance_memory_notify(
               instance_impl(&instance),
               0u, 0u, 0u, UINT32_MAX,
               &woken, &trap) == TURBOWASM_OK);
    assert(woken == 1u);

    assert(salts_thread_join(&first_thread) == 0);
    assert(salts_thread_join(&second_thread) == 0);

    assert(first.status == TURBOWASM_OK);
    assert(second.status == TURBOWASM_OK);
    assert(first.result == 0u);
    assert(second.result == 0u);
    assert(first.trap == TURBOWASM_TRAP_NONE);
    assert(second.trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_waiter_capacity_trap(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_memory *memory;
    uint32_t result = UINT32_MAX;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t index;

    assert(turbowasm_module_load_borrowed(
               &module,
               shared_memory_module,
               sizeof(shared_memory_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    memory = memory0(&instance);

    salts_mutex_lock(&memory->waiter_mutex);
    for (index = 0u; index < memory->waiter_capacity; ++index) {
        memory->waiters[index].active = true;
        memory->waiters[index].notified = false;
        memory->waiters[index].address = 0u;
    }
    memory->waiter_count = memory->waiter_capacity;
    salts_mutex_unlock(&memory->waiter_mutex);

    assert(turbowasm_instance_memory_wait(
               instance_impl(&instance),
               0u, 0u, 0u, 4u,
               0u, INT64_C(1000000),
               &result, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_TOO_MANY_WAITERS);
    assert(strcmp(
               turbowasm_trap_string(trap),
               "too_many_waiters") == 0);

    salts_mutex_lock(&memory->waiter_mutex);
    for (index = 0u; index < memory->waiter_capacity; ++index) {
        memory->waiters[index].active = false;
        memory->waiters[index].notified = false;
        memory->waiters[index].address = 0u;
    }
    memory->waiter_count = 0u;
    salts_mutex_unlock(&memory->waiter_mutex);

    /*
     * Capacity exhaustion must not poison the registry. Once slots are
     * released, a fresh equal-value zero-timeout wait is admitted normally.
     */
    result = UINT32_MAX;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_memory_wait(
               instance_impl(&instance),
               0u, 0u, 0u, 4u,
               0u, 0,
               &result, &trap) == TURBOWASM_OK);
    assert(result == 2u);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(waiter_count_get(memory) == 0u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static const uint8_t provider_bytes[] = {
    WASM_HEADER,
    /* shared memory min=1 max=1 */
    0x05, 0x04,
    0x01, 0x03, 0x01, 0x01,
    /* export mem */
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
};

static const uint8_t waiter_consumer_bytes[] = {
    WASM_HEADER,
    /* type0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    /* import p.mem shared min=1 max=1 */
    0x02, 0x0b, 0x01,
    0x01, 0x70,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x01,
    0x03, 0x02, 0x01, 0x00,
    /* wait32(addr=0, expected=0, timeout=5s safety bound) */
    0x0a, 0x12, 0x01, 0x10, 0x00,
    0x41, 0x00,
    0x41, 0x00,
    0x42, 0x80, 0xe4, 0x97, 0xd0, 0x12,
    0xfe, 0x01, 0x02, 0x00,
    0x0b
};

static const uint8_t notifier_consumer_bytes[] = {
    WASM_HEADER,
    /* type0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    /* import p.mem shared min=1 max=1 */
    0x02, 0x0b, 0x01,
    0x01, 0x70,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x01,
    0x03, 0x02, 0x01, 0x00,
    /* notify(addr=0, count=1) */
    0x0a, 0x0c, 0x01, 0x0a, 0x00,
    0x41, 0x00,
    0x41, 0x01,
    0xfe, 0x00, 0x02, 0x00,
    0x0b
};

typedef struct invoke_wait_context {
    turbowasm_instance *instance;
    int32_t result;
    turbowasm_status status;
    turbowasm_trap trap;
} invoke_wait_context;

static int32_t invoke_i32_noargs(
    turbowasm_instance *instance,
    turbowasm_trap *out_trap,
    turbowasm_status *out_status) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    status = turbowasm_instance_invoke(
        instance, 0u,
        NULL, 0u,
        &result, 1u,
        &result_count, &trap);
    if (out_trap != NULL)
        *out_trap = trap;
    if (out_status != NULL)
        *out_status = status;
    if (status != TURBOWASM_OK)
        return 0;
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void invoke_wait_thread(void *argument) {
    invoke_wait_context *context =
        (invoke_wait_context *)argument;
    context->result = invoke_i32_noargs(
        context->instance,
        &context->trap,
        &context->status);
}

static void test_linked_wait_notify_after_linker_destroy(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module waiter_module = {0};
    turbowasm_module notifier_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance waiter = {0};
    turbowasm_instance notifier = {0};
    turbowasm_linker linker = {0};
    invoke_wait_context wait_context = {0};
    salts_thread_t wait_thread = NULL;
    turbowasm_status notify_status;
    turbowasm_trap notify_trap = TURBOWASM_TRAP_NONE;
    int32_t notify_result;
    turbowasm_instance_memory *provider_memory;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &waiter_module,
               waiter_consumer_bytes,
               sizeof(waiter_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &notifier_module,
               notifier_consumer_bytes,
               sizeof(notifier_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider,
               &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               (turbowasm_name){
                   (const uint8_t *)"p", 1u},
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &waiter,
               &waiter_module,
               &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &notifier,
               &notifier_module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    provider_memory = memory0(&provider);
    wait_context.instance = &waiter;
    assert(salts_thread_create(
               &wait_thread,
               invoke_wait_thread,
               &wait_context) == 0);

    wait_for_waiter_count(provider_memory, 1u);

    notify_result = invoke_i32_noargs(
        &notifier,
        &notify_trap,
        &notify_status);
    assert(notify_status == TURBOWASM_OK);
    assert(notify_trap == TURBOWASM_TRAP_NONE);
    assert(notify_result == 1);

    assert(salts_thread_join(&wait_thread) == 0);
    assert(wait_context.status == TURBOWASM_OK);
    assert(wait_context.trap == TURBOWASM_TRAP_NONE);
    assert(wait_context.result == 0);

    turbowasm_instance_destroy(&notifier);
    turbowasm_instance_destroy(&waiter);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&notifier_module);
    turbowasm_module_destroy(&waiter_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_fence_executes_without_memory(void) {
    static const uint8_t fence_module[] = {
        WASM_HEADER,
        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x09, 0x01, 0x07, 0x00,
        0xfe, 0x03, 0x00,
        0x41, 0x2a,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_status status;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module,
               fence_module,
               sizeof(fence_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32_noargs(
               &instance, &trap, &status) == 42);
    assert(status == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_wasm_wait64_zero_timeout(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        /* shared memory min=1 max=1 */
        0x05, 0x04,
        0x01, 0x03, 0x01, 0x01,
        /* wait64(addr=0, expected=0, timeout=0) => timed-out(2) */
        0x0a, 0x0e, 0x01, 0x0c, 0x00,
        0x41, 0x00,
        0x42, 0x00,
        0x42, 0x00,
        0xfe, 0x02, 0x03, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_status status = TURBOWASM_OK;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32_noargs(
               &instance, &trap, &status) == 2);
    assert(status == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_direct_wait_results_and_traps();
    test_notify_count_and_spurious_signal();
    test_waiter_capacity_trap();
    test_linked_wait_notify_after_linker_destroy();
    test_fence_executes_without_memory();
    test_wasm_wait64_zero_timeout();
    return 0;
}
