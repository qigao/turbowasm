#include <metallic/threads.h>
#include <limits.h>

/* One bounded request slot, owned by the mutex. The host serializes root calls;
 * the persistent child owns the counter and acknowledges each submitted add. */
static mtx_t mutex;
static cnd_t changed;
static int counter, increment, pending, stopping, closed;

static int worker(void *unused) {
    (void)unused;
    if (mtx_lock(&mutex) != thrd_success) return 1;
    for (;;) {
        while (!pending && !stopping)
            if (cnd_wait(&changed, &mutex) != thrd_success) __builtin_trap();
        if (stopping) break;
        counter += increment;
        pending = 0;
        if (cnd_broadcast(&changed) != thrd_success) __builtin_trap();
    }
    return mtx_unlock(&mutex) == thrd_success ? 0 : 1;
}

__attribute__((constructor)) static void initialize(void) {
    thrd_t thread;
    if (mtx_init(&mutex, mtx_plain) != thrd_success || cnd_init(&changed) != thrd_success ||
        thrd_create(&thread, worker, NULL) != thrd_success || thrd_detach(thread) != thrd_success)
        __builtin_trap();
}

int counter_add(int value) {
    if (closed || mtx_lock(&mutex) != thrd_success) __builtin_trap();
    if ((value > 0 && counter > INT_MAX - value) || (value < 0 && counter < INT_MIN - value)) {
        (void)mtx_unlock(&mutex);
        __builtin_trap();
    }
    increment = value; pending = 1;
    if (cnd_broadcast(&changed) != thrd_success) __builtin_trap();
    while (pending)
        if (cnd_wait(&changed, &mutex) != thrd_success) __builtin_trap();
    int result = counter;
    if (mtx_unlock(&mutex) != thrd_success) __builtin_trap();
    return result;
}

int counter_close(void) {
    if (closed) return 0;
    if (mtx_lock(&mutex) != thrd_success) return thrd_error;
    stopping = 1;
    if (cnd_broadcast(&changed) != thrd_success || mtx_unlock(&mutex) != thrd_success)
        __builtin_trap();
    int status = metallic_threads_close(NULL);
    if (status != thrd_success) return status;
    cnd_destroy(&changed); mtx_destroy(&mutex); closed = 1;
    return 0;
}
