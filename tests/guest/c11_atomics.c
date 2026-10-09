#include <stdatomic.h>
#include <stdint.h>
#include <threads.h>

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)

/* Volatile keeps the width-specific operations observable even under LTO. */
#define WIDTH_CHECK(type) do { \
    volatile _Atomic(type) value; \
    atomic_init(&value, (type)0); \
    REQUIRE(atomic_is_lock_free(&value)); \
    atomic_store_explicit(&value, (type)0xf0, memory_order_release); \
    REQUIRE(atomic_load_explicit(&value, memory_order_acquire) == (type)0xf0); \
    REQUIRE(atomic_fetch_or(&value, (type)0x0f) == (type)0xf0); \
    REQUIRE(atomic_fetch_and(&value, (type)0x3c) == (type)0xff); \
    REQUIRE(atomic_fetch_xor(&value, (type)0x0c) == (type)0x3c); \
    REQUIRE(atomic_exchange(&value, (type)-1) == (type)0x30); \
    REQUIRE(atomic_fetch_add(&value, (type)1) == (type)-1); \
    REQUIRE(atomic_load(&value) == 0); \
    REQUIRE(atomic_fetch_sub(&value, (type)1) == 0); \
    type expected = 0; \
    REQUIRE(!atomic_compare_exchange_strong_explicit(&value, &expected, (type)7, \
        memory_order_acq_rel, memory_order_acquire)); \
    REQUIRE(expected == (type)-1); \
    REQUIRE(atomic_compare_exchange_strong(&value, &expected, (type)7)); \
    expected = 7; \
    while (!atomic_compare_exchange_weak_explicit(&value, &expected, (type)9, \
        memory_order_relaxed, memory_order_relaxed)) REQUIRE(expected == 7); \
    REQUIRE(atomic_load(&value) == 9); \
} while (0)

enum { WORKERS = 4, ROUNDS = 200 };
static atomic_uint ready, released, cas_count;
static _Atomic(uint64_t) wide_count;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static unsigned guarded_count, published[WORKERS];
static atomic_uint publication[WORKERS];

static int counting(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    atomic_fetch_add(&ready, 1);
    __builtin_wasm_memory_atomic_notify((int *)&ready, UINT32_MAX);
    while (!atomic_load_explicit(&released, memory_order_acquire))
        __builtin_wasm_memory_atomic_wait32((int *)&released, 0, -1);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        atomic_fetch_add_explicit(&wide_count, UINT64_C(0x100000001), memory_order_relaxed);
        unsigned expected = atomic_load_explicit(&cas_count, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(&cas_count, &expected, expected + 1,
            memory_order_relaxed, memory_order_relaxed)) {}
        while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) thrd_yield();
        ++guarded_count;
        atomic_flag_clear_explicit(&guard, memory_order_release);
    }
    published[index] = 73u + index;
    atomic_store_explicit(&publication[index], 1, memory_order_release);
    __builtin_wasm_memory_atomic_notify((int *)&publication[index], UINT32_MAX);
    return 0;
}

int atomics(void) {
    WIDTH_CHECK(uint8_t); WIDTH_CHECK(uint16_t);
    WIDTH_CHECK(uint32_t); WIDTH_CHECK(uint64_t);
    int elements[4];
    _Atomic(int *) pointer;
    atomic_init(&pointer, elements);
    REQUIRE(atomic_is_lock_free(&pointer));
    REQUIRE(atomic_fetch_add(&pointer, 2) == elements);
    REQUIRE(atomic_load(&pointer) == elements + 2);
    REQUIRE(atomic_fetch_sub(&pointer, 1) == elements + 2);
    REQUIRE(atomic_load(&pointer) == elements + 1);
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_seq_cst);

    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        REQUIRE(thrd_create(&children[i], counting, (void *)(uintptr_t)i) == thrd_success);
    for (;;) {
        unsigned arrived = atomic_load(&ready);
        if (arrived == WORKERS) break;
        __builtin_wasm_memory_atomic_wait32((int *)&ready, (int)arrived, -1);
    }
    atomic_store_explicit(&released, 1, memory_order_release);
    __builtin_wasm_memory_atomic_notify((int *)&released, UINT32_MAX);
    for (unsigned i = 0; i < WORKERS; ++i) {
        while (!atomic_load_explicit(&publication[i], memory_order_acquire))
            __builtin_wasm_memory_atomic_wait32((int *)&publication[i], 0, -1);
        /* Validate release/acquire publication before join adds synchronization. */
        REQUIRE(published[i] == 73u + i);
        int result;
        REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    REQUIRE(atomic_load(&wide_count) == UINT64_C(0x100000001) * WORKERS * ROUNDS);
    REQUIRE(atomic_load(&cas_count) == WORKERS * ROUNDS);
    REQUIRE(guarded_count == WORKERS * ROUNDS);
    return 0;
}
