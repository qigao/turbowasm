#include "../../src/wasi/wasi.h"
#include "internal.h"
#include <limits.h>

static int valid_time(const struct timespec *time) {
    return time && time->tv_nsec >= 0 && time->tv_nsec < 1000000000L;
}

int64_t __metallic_deadline_ns(const struct timespec *deadline) {
    __wasi_timestamp_t now;
    if (!valid_time(deadline)) return -1;
    if (deadline->tv_sec < 0) return 0;
    if (__wasi_clock_time_get(__WASI_CLOCKID_REALTIME, 1, &now)) return -1;
    uint64_t seconds = now / UINT64_C(1000000000);
    long nanos = (long)(now % UINT64_C(1000000000));
    if ((uint64_t)deadline->tv_sec < seconds ||
        ((uint64_t)deadline->tv_sec == seconds && deadline->tv_nsec <= nanos)) return 0;
    uint64_t difference = (uint64_t)deadline->tv_sec - seconds;
    /* A one-second slice lets real-time clock adjustments take effect. This
     * bounds waiting, not the caller's deadline; every slice rechecks it. */
    if (difference > 1) return INT64_C(1000000000);
    int64_t remaining = (int64_t)difference * INT64_C(1000000000) + deadline->tv_nsec - nanos;
    return remaining > INT64_C(1000000000) ? INT64_C(1000000000) : remaining;
}

void call_once(once_flag *flag, void (*function)(void)) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&flag->state, &expected, 1,
            memory_order_acquire, memory_order_acquire)) {
        function();
        atomic_store_explicit(&flag->state, 2, memory_order_release);
        __builtin_wasm_memory_atomic_notify((int *)&flag->state, UINT_MAX);
    } else {
        while (atomic_load_explicit(&flag->state, memory_order_acquire) != 2)
            __builtin_wasm_memory_atomic_wait32((int *)&flag->state, 1, -1);
    }
}

int mtx_init(mtx_t *mutex, int type) {
    if (!mutex || (type & ~(mtx_recursive | mtx_timed))) return thrd_error;
    atomic_init(&mutex->owner, 0);
    mutex->depth = 0; mutex->type = (unsigned)type;
    return thrd_success;
}
void mtx_destroy(mtx_t *mutex) {
    if (atomic_load_explicit(&mutex->owner, memory_order_relaxed)) __builtin_trap();
    mutex->type = UINT_MAX;
}
int mtx_trylock(mtx_t *mutex) {
    if (!mutex || mutex->type == UINT_MAX) return thrd_error;
    unsigned token = __metallic_thread_token(), expected = 0;
    if (atomic_compare_exchange_strong_explicit(&mutex->owner, &expected, token,
            memory_order_acquire, memory_order_relaxed)) {
        mutex->depth = 1;
        return thrd_success;
    }
    if (expected == token && (mutex->type & mtx_recursive)) {
        if (mutex->depth == UINT_MAX) return thrd_error;
        ++mutex->depth;
        return thrd_success;
    }
    return thrd_busy;
}
static int lock_until(mtx_t *mutex, const struct timespec *deadline) {
    for (;;) {
        int result = mtx_trylock(mutex);
        if (result != thrd_busy) return result;
        int64_t timeout = deadline ? __metallic_deadline_ns(deadline) : -1;
        if (deadline && timeout <= 0) return timeout == 0 ? thrd_timedout : thrd_error;
        unsigned owner = atomic_load_explicit(&mutex->owner, memory_order_relaxed);
        if (owner) __builtin_wasm_memory_atomic_wait32((int *)&mutex->owner, (int)owner, timeout);
    }
}
int mtx_lock(mtx_t *mutex) { return lock_until(mutex, NULL); }
int mtx_timedlock(mtx_t *restrict mutex, const struct timespec *restrict deadline) {
    if (!mutex || !(mutex->type & mtx_timed) || !valid_time(deadline)) return thrd_error;
    return lock_until(mutex, deadline);
}
int mtx_unlock(mtx_t *mutex) {
    if (!mutex || atomic_load_explicit(&mutex->owner, memory_order_relaxed) != __metallic_thread_token())
        return thrd_error;
    if (--mutex->depth == 0) {
        atomic_store_explicit(&mutex->owner, 0, memory_order_release);
        __builtin_wasm_memory_atomic_notify((int *)&mutex->owner, 1);
    }
    return thrd_success;
}

int cnd_init(cnd_t *condition) {
    if (!condition) return thrd_error;
    atomic_init(&condition->sequence, 0);
    return thrd_success;
}
void cnd_destroy(cnd_t *condition) {
    /* As required by C11, destruction is owner-exclusive with no waiters. */
    atomic_store_explicit(&condition->sequence, UINT64_MAX, memory_order_relaxed);
}
static int signal_condition(cnd_t *condition, unsigned count) {
    if (!condition) return thrd_error;
    uint64_t current = atomic_load_explicit(&condition->sequence, memory_order_relaxed);
    do {
        if (current == UINT64_MAX) return thrd_error;
    } while (!atomic_compare_exchange_weak_explicit(&condition->sequence, &current, current + 1,
        memory_order_release, memory_order_relaxed));
    __builtin_wasm_memory_atomic_notify((int *)&condition->sequence, count);
    return thrd_success;
}
int cnd_signal(cnd_t *condition) { return signal_condition(condition, 1); }
int cnd_broadcast(cnd_t *condition) { return signal_condition(condition, UINT_MAX); }
static int condition_wait(cnd_t *condition, mtx_t *mutex, const struct timespec *deadline) {
    if (!condition || !mutex || (deadline && !valid_time(deadline)) ||
        atomic_load_explicit(&mutex->owner, memory_order_relaxed) != __metallic_thread_token() ||
        mutex->depth != 1) return thrd_error;
    uint64_t sequence = atomic_load_explicit(&condition->sequence, memory_order_acquire);
    if (sequence == UINT64_MAX) return thrd_error;
    if (mtx_unlock(mutex) != thrd_success) return thrd_error;
    int result = thrd_success;
    while (atomic_load_explicit(&condition->sequence, memory_order_acquire) == sequence) {
        int64_t timeout = deadline ? __metallic_deadline_ns(deadline) : -1;
        if (deadline && timeout <= 0) {
            result = timeout == 0 ? thrd_timedout : thrd_error;
            break;
        }
        int wait = __builtin_wasm_memory_atomic_wait64((long long *)&condition->sequence, (long long)sequence, timeout);
        /* A notify may race another waiter consuming a signal. C11 permits
         * spurious wakeups; user predicates remain protected by the mutex. */
        if (wait == 0) break;
    }
    return mtx_lock(mutex) == thrd_success ? result : thrd_error;
}
int cnd_wait(cnd_t *condition, mtx_t *mutex) { return condition_wait(condition, mutex, NULL); }
int cnd_timedwait(cnd_t *restrict condition, mtx_t *restrict mutex, const struct timespec *restrict deadline) {
    if (!deadline) return thrd_error;
    return condition_wait(condition, mutex, deadline);
}

static atomic_int sleep_word;
void thrd_yield(void) {
    __builtin_wasm_memory_atomic_wait32((int *)&sleep_word, 0, 1000);
}
int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    if (!valid_time(duration) || duration->tv_sec < 0 || (uint64_t)duration->tv_sec >
        (UINT64_MAX - (uint64_t)duration->tv_nsec) / UINT64_C(1000000000)) return -2;
    uint64_t interval = (uint64_t)duration->tv_sec * UINT64_C(1000000000) + (uint64_t)duration->tv_nsec;
    __wasi_timestamp_t now;
    if (__wasi_clock_time_get(__WASI_CLOCKID_MONOTONIC, 1, &now) || interval > UINT64_MAX - now) return -2;
    uint64_t end = now + interval;
    while (now < end) {
        uint64_t left = end - now;
        int64_t slice = left > INT64_MAX ? INT64_MAX : (int64_t)left;
        __builtin_wasm_memory_atomic_wait32((int *)&sleep_word, 0, slice);
        if (__wasi_clock_time_get(__WASI_CLOCKID_MONOTONIC, 1, &now)) return -2;
    }
    if (remaining) *remaining = (struct timespec){0};
    return 0;
}
