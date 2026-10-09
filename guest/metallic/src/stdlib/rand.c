#include <stdint.h>
#ifdef __METALLIC_THREADS__
#include <stdatomic.h>
static _Atomic uint_fast32_t state = 1;
#else

static uint_fast32_t state = 1;
#endif

void srand(unsigned seed)
{
#ifdef __METALLIC_THREADS__
    atomic_store_explicit(&state, seed, memory_order_relaxed);
#else
    state = seed;
#endif
}

int rand(void)
{
#ifdef __METALLIC_THREADS__
    uint_fast32_t current = atomic_load_explicit(&state, memory_order_relaxed), next;
    do { next = current * 48271 % 2147483647; }
    while (!atomic_compare_exchange_weak_explicit(&state, &current, next,
        memory_order_relaxed, memory_order_relaxed));
    return (int)next;
#else
    return state = state * 48271 % 2147483647;
#endif
}

int random(void) __attribute__((alias("rand")));
