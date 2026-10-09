#ifndef METALLIC_INTERNAL_EXIT_CALLBACKS_H
#define METALLIC_INTERNAL_EXIT_CALLBACKS_H
#ifdef __METALLIC_THREADS__
#include "thread_lock.h"
#endif

/* C11 guarantees at least 32 callbacks in each independent exit registry. */
enum { METALLIC_EXIT_CALLBACKS = 32 };
typedef struct {
#ifdef __METALLIC_THREADS__
    metallic_lock lock;
#endif
    void (*functions[METALLIC_EXIT_CALLBACKS])(void);
    unsigned count;
} metallic_exit_callbacks;

static inline int metallic_exit_push(metallic_exit_callbacks *callbacks, void (*function)(void)) {
    if (!function) return 1;
#ifdef __METALLIC_THREADS__
    metallic_lock_acquire(&callbacks->lock);
#endif
    int full = callbacks->count == METALLIC_EXIT_CALLBACKS;
    if (!full) callbacks->functions[callbacks->count++] = function;
#ifdef __METALLIC_THREADS__
    metallic_lock_release(&callbacks->lock);
#endif
    return full;
}

static inline void metallic_exit_run(metallic_exit_callbacks *callbacks) {
    for (;;) {
#ifdef __METALLIC_THREADS__
        metallic_lock_acquire(&callbacks->lock);
#endif
        void (*function)(void) = callbacks->count ? callbacks->functions[--callbacks->count] : 0;
#ifdef __METALLIC_THREADS__
        metallic_lock_release(&callbacks->lock);
#endif
        if (!function) return;
        /* A callback can register another callback or use other libc services. */
        function();
    }
}
#endif
