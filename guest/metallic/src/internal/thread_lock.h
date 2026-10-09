#ifndef METALLIC_INTERNAL_THREAD_LOCK_H
#define METALLIC_INTERNAL_THREAD_LOCK_H
#include <stdatomic.h>
#include <stdint.h>

typedef atomic_int metallic_lock;
static inline int metallic_try_lock(metallic_lock *lock) {
    int expected = 0;
    return atomic_compare_exchange_strong_explicit(lock, &expected, 1,
        memory_order_acquire, memory_order_relaxed);
}
static inline int metallic_lock_acquire(metallic_lock *lock) {
    while (!metallic_try_lock(lock))
        __builtin_wasm_memory_atomic_wait32((int *)lock, 1, -1);
    return 0;
}
static inline void metallic_lock_release(metallic_lock *lock) {
    atomic_store_explicit(lock, 0, memory_order_release);
    __builtin_wasm_memory_atomic_notify((int *)lock, 1);
}
#endif
