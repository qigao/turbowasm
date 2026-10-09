#include "../../guest/metallic/src/wasi/wasi.h"
#include <threads.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)

static int shared_io_child(void *argument) {
    (void)argument;
    const char *environment = getenv("TURBOWASM_THREADS");
    REQUIRE(environment != NULL && strcmp(environment, "supported") == 0);
    REQUIRE(getenv("MISSING") == NULL);
    const __wasi_ciovec_t outputs[2] = {{"ab", 2}, {"cdef", 4}};
    size_t count = 99;
    REQUIRE(__wasi_fd_write(1, outputs, 2, &count) == 0 && count == 5);
    char first[2] = {'?', '?'}, second[4] = {'?', '?', '?', '?'};
    const __wasi_iovec_t inputs[2] = {{first, 2}, {second, 4}};
    REQUIRE(__wasi_fd_read(0, inputs, 2, &count) == 0 && count == 5);
    REQUIRE(memcmp(first, "ab", 2) == 0 && memcmp(second, "cde?", 4) == 0);
    return 0;
}

int shared_io(void) {
    size_t argc = 99, argv_size = 99;
    REQUIRE(__wasi_args_sizes_get(&argc, &argv_size) == 0 && argc == 1 && argv_size == 8);
    uint8_t *argv[1], argv_buffer[8];
    REQUIRE(__wasi_args_get(argv, argv_buffer) == 0 && strcmp((char *)argv[0], "threads") == 0);
    thrd_t children[4];
    for (int i = 0; i < 4; ++i) REQUIRE(thrd_create(&children[i], shared_io_child, NULL) == thrd_success);
    for (int i = 0; i < 4; ++i) {
        int result;
        REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    char *large = malloc(1024u * 1024u + 1u);
    REQUIRE(large != NULL);
    memset(large, 'x', 1024u * 1024u + 1u);
    __wasi_ciovec_t output = {large, 1024u * 1024u};
    size_t count = 99;
    REQUIRE(__wasi_fd_write(42, &output, 1, &count) == 0 && count == 1024u * 1024u);
    ++output.buf_len; count = 99;
    REQUIRE(__wasi_fd_write(42, &output, 1, &count) == __WASI_ERRNO_NOMEM && count == 99);
    __wasi_iovec_t input = {large, output.buf_len};
    REQUIRE(__wasi_fd_read(0, &input, 1, &count) == __WASI_ERRNO_NOMEM && count == 99);
    output.buf = (void *)(uintptr_t)UINT32_MAX; output.buf_len = 2;
    REQUIRE(__wasi_fd_write(1, &output, 1, &count) == __WASI_ERRNO_FAULT && count == 99);
    input.buf = (void *)(uintptr_t)UINT32_MAX; input.buf_len = 2;
    REQUIRE(__wasi_fd_read(0, &input, 1, &count) == __WASI_ERRNO_FAULT && count == 99);
    output = (__wasi_ciovec_t){large, 1};
    REQUIRE(__wasi_fd_write(1, &output, 1, (size_t *)(uintptr_t)UINT32_MAX) == __WASI_ERRNO_FAULT);
    REQUIRE(__wasi_fd_write(1, &output, 0, &count) == 0 && count == 0);
    input = (__wasi_iovec_t){large, 4};
    count = 99;
    REQUIRE(__wasi_fd_read(44, &input, 1, &count) == __WASI_ERRNO_IO && count == 99 && large[0] == 'x');
    REQUIRE(__wasi_fd_read(43, &input, 1, &count) == __WASI_ERRNO_IO && count == 99 && large[0] == 'x');
    free(large);
    return 0;
}
extern uintptr_t __metallic_brk;
extern unsigned char __heap_base;
extern void __wasm_call_ctors(void);
static volatile int constructors;
__attribute__((constructor)) static void constructed(void) { ++constructors; }

int initialize(void) {
    __metallic_brk = (uintptr_t)&__heap_base;
    __metallic_threads_init();
    __wasm_call_ctors();
    REQUIRE(constructors == 1);
    return 0;
}

static atomic_int gate, arrived;
static void wait_value(atomic_int *value, int expected) {
    while (atomic_load(value) == expected)
        __builtin_wasm_memory_atomic_wait32((int *)value, expected, -1);
}
static void publish(atomic_int *value, int next) {
    atomic_store(value, next);
    __builtin_wasm_memory_atomic_notify((int *)value, UINT32_MAX);
}
static int identity(void *argument) {
    thrd_t self = thrd_current();
    REQUIRE(!thrd_equal(self, *(thrd_t *)argument));
    REQUIRE(thrd_equal(self, thrd_current()));
    REQUIRE(errno == 0 && constructors == 1);
    errno = 81;
    thrd_yield();
    REQUIRE(errno == 81);
    return 42;
}
static int exiting(void *argument) { thrd_exit((int)(intptr_t)argument); }

int lifecycle(void) {
    thrd_t root = thrd_current(), child;
    errno = 17;
    REQUIRE(thrd_create(&child, identity, &root) == thrd_success);
    thrd_t stale = child;
    int result = 0;
    REQUIRE(thrd_join(child, &result) == thrd_success && result == 42);
    REQUIRE(errno == 17 && thrd_join(stale, NULL) == thrd_error && thrd_detach(stale) == thrd_error);
    REQUIRE(thrd_join(root, NULL) == thrd_error);
    REQUIRE(thrd_create(&child, exiting, (void *)(intptr_t)-19) == thrd_success);
    REQUIRE(!thrd_equal(child, stale));
    REQUIRE(thrd_join(child, &result) == thrd_success && result == -19);
    return 0;
}

static once_flag once = ONCE_FLAG_INIT;
static int once_count, once_data;
static void once_body(void) { ++once_count; once_data = 119; }
static mtx_t counter_lock;
static int counter;
static int counting(void *argument) {
    (void)argument;
    call_once(&once, once_body);
    REQUIRE(once_data == 119);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(mtx_lock(&counter_lock) == thrd_success);
        ++counter;
        REQUIRE(mtx_unlock(&counter_lock) == thrd_success);
    }
    return 0;
}
int synchronization(void) {
    REQUIRE(mtx_init(&counter_lock, mtx_plain) == thrd_success);
    thrd_t children[4];
    for (int i = 0; i < 4; ++i) REQUIRE(thrd_create(&children[i], counting, NULL) == thrd_success);
    for (int i = 0; i < 4; ++i) { int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0); }
    REQUIRE(counter == 400 && once_count == 1);
    mtx_destroy(&counter_lock);
    mtx_t recursive;
    REQUIRE(mtx_init(&recursive, mtx_recursive | mtx_timed) == thrd_success);
    REQUIRE(mtx_lock(&recursive) == thrd_success && mtx_trylock(&recursive) == thrd_success);
    struct timespec expired = {0};
    REQUIRE(mtx_timedlock(&recursive, &expired) == thrd_success);
    REQUIRE(mtx_unlock(&recursive) == thrd_success && mtx_unlock(&recursive) == thrd_success);
    REQUIRE(mtx_unlock(&recursive) == thrd_success && mtx_unlock(&recursive) == thrd_error);
    mtx_destroy(&recursive);
    return 0;
}

static mtx_t condition_lock;
static cnd_t condition;
static int predicate, waiters;
static int waiting(void *argument) {
    (void)argument;
    REQUIRE(mtx_lock(&condition_lock) == thrd_success);
    ++waiters;
    REQUIRE(cnd_broadcast(&condition) == thrd_success);
    while (!predicate) REQUIRE(cnd_wait(&condition, &condition_lock) == thrd_success);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_success);
    return 0;
}
static int timed_contender(void *argument) {
    (void)argument;
    struct timespec deadline;
    REQUIRE(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_nsec += 2000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    REQUIRE(mtx_trylock(&condition_lock) == thrd_busy);
    REQUIRE(mtx_timedlock(&condition_lock, &deadline) == thrd_timedout);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_error);
    return 0;
}
int conditions(void) {
    REQUIRE(mtx_init(&condition_lock, mtx_timed) == thrd_success && cnd_init(&condition) == thrd_success);
    REQUIRE(mtx_lock(&condition_lock) == thrd_success);
    struct timespec expired = {0};
    REQUIRE(cnd_timedwait(&condition, &condition_lock, &expired) == thrd_timedout);
    expired.tv_sec = -1;
    REQUIRE(cnd_timedwait(&condition, &condition_lock, &expired) == thrd_timedout);
    thrd_t child;
    REQUIRE(thrd_create(&child, timed_contender, NULL) == thrd_success);
    int result;
    REQUIRE(thrd_join(child, &result) == thrd_success && result == 0);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_success);
    thrd_t children[2];
    for (int i = 0; i < 2; ++i) REQUIRE(thrd_create(&children[i], waiting, NULL) == thrd_success);
    REQUIRE(mtx_lock(&condition_lock) == thrd_success);
    while (waiters != 2) REQUIRE(cnd_wait(&condition, &condition_lock) == thrd_success);
    /* A signal without changing the predicate must not let a waiter finish. */
    REQUIRE(cnd_signal(&condition) == thrd_success);
    predicate = 1;
    REQUIRE(cnd_broadcast(&condition) == thrd_success);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_success);
    for (int i = 0; i < 2; ++i) REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    cnd_destroy(&condition); mtx_destroy(&condition_lock);
    struct timespec duration = {0, 1000000}, remaining = {1, 1};
    REQUIRE(thrd_sleep(&duration, &remaining) == 0 && remaining.tv_sec == 0 && remaining.tv_nsec == 0);
    duration.tv_nsec = 1000000000;
    REQUIRE(thrd_sleep(&duration, NULL) == -2);
    duration = (struct timespec){-1, 0};
    REQUIRE(thrd_sleep(&duration, NULL) == -2);
    return 0;
}

static atomic_int spurious_ready, spurious_wakes, spurious_done;
static int spurious_waiter(void *argument) {
    (void)argument;
    REQUIRE(mtx_lock(&condition_lock) == thrd_success);
    publish(&spurious_ready, 1);
    while (!predicate) {
        REQUIRE(cnd_wait(&condition, &condition_lock) == thrd_success);
        if (!predicate) publish(&spurious_wakes, 1);
    }
    publish(&spurious_done, 1);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_success);
    return 0;
}
int condition_spurious(void) {
    REQUIRE(mtx_init(&condition_lock, mtx_plain) == thrd_success && cnd_init(&condition) == thrd_success);
    thrd_t worker;
    REQUIRE(thrd_create(&worker, spurious_waiter, NULL) == thrd_success);
    wait_value(&spurious_ready, 0);
    /* A positive notify count proves the child was actually waiting. Leave the
     * condition sequence and application predicate unchanged to force a real
     * spurious wakeup, then wait for its acknowledgement before publishing. */
    while (!__builtin_wasm_memory_atomic_notify((int *)&condition.sequence, 1)) thrd_yield();
    wait_value(&spurious_wakes, 0);
    REQUIRE(!atomic_load(&spurious_done));
    REQUIRE(mtx_lock(&condition_lock) == thrd_success);
    predicate = 1;
    REQUIRE(cnd_broadcast(&condition) == thrd_success);
    REQUIRE(mtx_unlock(&condition_lock) == thrd_success);
    int result;
    REQUIRE(thrd_join(worker, &result) == thrd_success && result == 0);
    REQUIRE(atomic_load(&spurious_done) == 1);
    cnd_destroy(&condition); mtx_destroy(&condition_lock);
    return 0;
}
int synchronization_limits(void) {
    mtx_t mutex;
    REQUIRE(mtx_init(&mutex, mtx_recursive) == thrd_success);
    REQUIRE(mtx_lock(&mutex) == thrd_success);
    /* White-box boundary setup exercises exhaustion without billions of calls. */
    mutex.depth = UINT_MAX;
    unsigned owner = atomic_load(&mutex.owner);
    REQUIRE(mtx_trylock(&mutex) == thrd_error && mutex.depth == UINT_MAX);
    REQUIRE(atomic_load(&mutex.owner) == owner);
    mutex.depth = 1;
    REQUIRE(mtx_unlock(&mutex) == thrd_success);
    mtx_destroy(&mutex);
    cnd_t exhausted;
    REQUIRE(cnd_init(&exhausted) == thrd_success);
    atomic_store(&exhausted.sequence, UINT64_MAX - 1);
    REQUIRE(cnd_signal(&exhausted) == thrd_success);
    REQUIRE(cnd_signal(&exhausted) == thrd_error && cnd_broadcast(&exhausted) == thrd_error);
    REQUIRE(atomic_load(&exhausted.sequence) == UINT64_MAX);
    cnd_destroy(&exhausted);
    struct timespec duration = {INT64_MAX, 999999999}, remaining = {71, 73};
    REQUIRE(thrd_sleep(&duration, &remaining) == -2);
    REQUIRE(remaining.tv_sec == 71 && remaining.tv_nsec == 73);
    return 0;
}

static tss_t key;
static atomic_int destructor_calls, destructor_errors;
static void destroy_value(void *value) {
    if (tss_get(key) != NULL) atomic_store(&destructor_errors, 1);
    atomic_fetch_add(&destructor_calls, 1);
    if (tss_set(key, value) != thrd_success) atomic_store(&destructor_errors, 2);
}
static int specific(void *argument) {
    REQUIRE(tss_get(key) == NULL && tss_set(key, argument) == thrd_success && tss_get(key) == argument);
    if ((intptr_t)argument == 1) thrd_exit(7);
    return 8;
}
int thread_specific(void) {
    REQUIRE(tss_create(&key, destroy_value) == thrd_success);
    thrd_t children[2];
    for (int i = 0; i < 2; ++i) REQUIRE(thrd_create(&children[i], specific, (void *)(intptr_t)(i + 1)) == thrd_success);
    for (int i = 0; i < 2; ++i) { int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == i + 7); }
    REQUIRE(atomic_load(&destructor_calls) == 2 * TSS_DTOR_ITERATIONS && !atomic_load(&destructor_errors));
    REQUIRE(tss_get(key) == NULL);
    tss_t stale = key;
    tss_delete(key);
    REQUIRE(tss_create(&key, NULL) == thrd_success);
    REQUIRE(tss_set(stale, (void *)1) == thrd_error && tss_get(stale) == NULL);
    tss_delete(stale);
    REQUIRE(tss_set(key, (void *)2) == thrd_success && tss_get(key) == (void *)2);
    tss_delete(key);
    tss_t keys[METALLIC_TSS_CAPACITY];
    for (unsigned i = 0; i < METALLIC_TSS_CAPACITY; ++i) REQUIRE(tss_create(&keys[i], NULL) == thrd_success);
    REQUIRE(tss_create(&key, NULL) == thrd_error);
    for (unsigned i = 0; i < METALLIC_TSS_CAPACITY; ++i) tss_delete(keys[i]);
    return 0;
}

static int allocation_worker(void *argument) {
    unsigned tag = (unsigned)(uintptr_t)argument;
    for (unsigned i = 1; i <= 96; ++i) {
        size_t size = i * 31;
        unsigned char *buffer = malloc(size);
        REQUIRE(buffer);
        memset(buffer, (int)tag, size);
        unsigned char *grown = realloc(buffer, size * 2);
        if (!grown) { free(buffer); return __LINE__; }
        for (size_t j = 0; j < size; ++j) if (grown[j] != tag) { free(grown); return __LINE__; }
        free(grown);
        void *aligned = aligned_alloc(64, 512);
        REQUIRE(aligned && (uintptr_t)aligned % 64 == 0);
        free(aligned);
    }
    return 0;
}
int allocation(void) {
    thrd_t children[4];
    for (int i = 0; i < 4; ++i) REQUIRE(thrd_create(&children[i], allocation_worker, (void *)(uintptr_t)(i + 1)) == thrd_success);
    for (int i = 0; i < 4; ++i) { int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0); }
    return 0;
}

static int blocked(void *argument) {
    (void)argument;
    atomic_fetch_add(&arrived, 1);
    __builtin_wasm_memory_atomic_notify((int *)&arrived, UINT32_MAX);
    wait_value(&gate, 0);
    return 15;
}
int capacity(void) {
    thrd_t children[4], extra = {99, 99};
    for (int i = 0; i < 4; ++i) REQUIRE(thrd_create(&children[i], blocked, NULL) == thrd_success);
    while (atomic_load(&arrived) != 4) { int value = atomic_load(&arrived); if (value != 4) wait_value(&arrived, value); }
    REQUIRE(thrd_create(&extra, blocked, NULL) == thrd_nomem && extra.slot == 99 && extra.generation == 99);
    REQUIRE(thrd_detach(children[0]) == thrd_success && thrd_detach(children[0]) == thrd_error);
    REQUIRE(thrd_join(children[0], NULL) == thrd_error);
    publish(&gate, 1);
    for (int i = 1; i < 4; ++i) { int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 15); }
    /* The runtime slot can remain admitted briefly after the guest terminal.
     * Resource rejection is retryable; no sleep-based scheduling assumption. */
    int status;
    do { status = thrd_create(&extra, exiting, (void *)42); if (status == thrd_nomem) thrd_yield(); } while (status == thrd_nomem);
    REQUIRE(status == thrd_success);
    int result;
    REQUIRE(thrd_join(extra, &result) == thrd_success && result == 42);
    REQUIRE(__metallic_threads_drain(NULL) == thrd_success);
    REQUIRE(thrd_create(&extra, exiting, NULL) == thrd_error);
    return 0;
}

static thrd_t drain_target;
static thrd_t retained[METALLIC_THREAD_CAPACITY];
static atomic_int retained_completed;
static int retaining_child(void *argument) {
    atomic_fetch_add(&retained_completed, 1);
    __builtin_wasm_memory_atomic_notify((int *)&retained_completed, UINT32_MAX);
    return (int)(intptr_t)argument;
}
int retained_fill(void) {
    for (unsigned i = 0; i < METALLIC_THREAD_CAPACITY; ++i) {
        int status;
        /* Host finalization may briefly retain a worker after the guest result
         * is published. Retry only that bounded, transient admission failure. */
        do {
            status = thrd_create(&retained[i], retaining_child, (void *)(intptr_t)i);
            if (status == thrd_nomem) thrd_yield();
        } while (status == thrd_nomem);
        REQUIRE(status == thrd_success);
        wait_value(&retained_completed, (int)i);
    }
    return 0;
}
int retained_recover(void) {
    /* The native test first waits for actual host task finalization. Capacity
     * rejection here must come from retained guest records, not busy workers. */
    thrd_t extra = {99, 99}, stale = retained[0];
    REQUIRE(thrd_create(&extra, exiting, (void *)42) == thrd_nomem);
    REQUIRE(extra.slot == 99 && extra.generation == 99);
    int result;
    REQUIRE(thrd_join(stale, &result) == thrd_success && result == 0);
    REQUIRE(thrd_create(&extra, exiting, (void *)42) == thrd_success);
    REQUIRE(!thrd_equal(stale, extra) && thrd_join(stale, NULL) == thrd_error);
    REQUIRE(thrd_join(extra, &result) == thrd_success && result == 42);
    for (unsigned i = 1; i < METALLIC_THREAD_CAPACITY; ++i)
        REQUIRE(thrd_join(retained[i], &result) == thrd_success && result == (int)i);
    return 0;
}

static atomic_int drain_join_result;
static int drain_joiner(void *argument) {
    (void)argument;
    atomic_fetch_add(&arrived, 1);
    __builtin_wasm_memory_atomic_notify((int *)&arrived, UINT32_MAX);
    int result = 0;
    int status = thrd_join(drain_target, &result);
    atomic_store(&drain_join_result, status == thrd_success && result == 15 ? 1 : -1);
    return 0;
}
int drain_join(void) {
    thrd_t joiner;
    REQUIRE(thrd_create(&drain_target, blocked, NULL) == thrd_success);
    REQUIRE(thrd_create(&joiner, drain_joiner, NULL) == thrd_success);
    while (atomic_load(&arrived) != 2) { int value = atomic_load(&arrived); if (value != 2) wait_value(&arrived, value); }
    struct timespec expired = {0};
    REQUIRE(__metallic_threads_drain(&expired) == thrd_timedout);
    REQUIRE(thrd_create(&joiner, exiting, NULL) == thrd_error);
    publish(&gate, 1);
    REQUIRE(__metallic_threads_drain(NULL) == thrd_success);
    REQUIRE(atomic_load(&drain_join_result) == 1);
    REQUIRE(thrd_join(drain_target, NULL) == thrd_error);
    return 0;
}

__attribute__((import_module("test"), import_name("thread_failure")))
extern void thread_failure(void);
static int failing_child(void *argument) {
    (void)argument;
    thread_failure();
    return 1;
}
int abnormal_join(void) {
    thrd_t child;
    REQUIRE(thrd_create(&child, failing_child, NULL) == thrd_success);
    /* The host failure skips entry.S's terminal word. Group interruption must
     * unwind this real libc join instead of waiting for an impossible result. */
    (void)thrd_join(child, NULL);
    return __LINE__;
}
