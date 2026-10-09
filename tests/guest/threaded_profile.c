#include <metallic/threads.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#if defined(__STDC_NO_THREADS__) && __STDC_NO_THREADS__
#error "The installed THREADS profile must advertise C11 threads"
#endif

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
static unsigned constructors;
static int *payload;
static _Thread_local int local = 17;
static atomic_uint ready, release_children;
static int results[2];
static thrd_t root_thread;

__attribute__((constructor)) static void construct(void) {
    ++constructors;
    payload = malloc(4 * sizeof(*payload));
    if (!payload) __builtin_trap();
    payload[0] = 83; payload[1] = 0; payload[2] = 0; payload[3] = 0;
    root_thread = thrd_current();
    errno = 42;
#ifdef GUEST_RECURSIVE_CRT
    extern void _initialize(void);
    _initialize();
#endif
}
static int child_checks(unsigned i) {
    REQUIRE(constructors == 1 && local == 17 && errno == 0);
    REQUIRE(payload && payload[0] == 83 && !thrd_equal(thrd_current(), root_thread));
    REQUIRE(metallic_threads_close(NULL) == thrd_error);
    local = 100 + (int)i; errno = 70 + (int)i;
    payload[i + 1] = 31 + (int)i;
    return 0;
}
static int child(void *arg) {
    unsigned value = (unsigned)(uintptr_t)arg, i = value & 1;
    int result = child_checks(i);
    results[i] = result;
    atomic_fetch_add(&ready, 1);
    __builtin_wasm_memory_atomic_notify((int *)&ready, UINT32_MAX);
    if (value & 2) __builtin_trap();
    while (!atomic_load(&release_children))
        __builtin_wasm_memory_atomic_wait32((int *)&release_children, 0, -1);
    if (local != 100 + (int)i || errno != 70 + (int)i) results[i] = __LINE__;
    return result;
}
int profile_begin(int fatal) {
    REQUIRE(constructors == 1 && local >= 17 && errno == 42 && payload[0] == 83);
    thrd_t children[2];
    for (unsigned i = 0; i < 2; ++i)
        REQUIRE(thrd_create(&children[i], child, (void *)(uintptr_t)(i | (fatal ? 2 : 0))) == thrd_success);
    REQUIRE(thrd_detach(children[0]) == thrd_success);
    for (;;) {
        unsigned count = atomic_load(&ready);
        if (count == 2) break;
        __builtin_wasm_memory_atomic_wait32((int *)&ready, (int)count, -1);
    }
    return 0;
}
int profile_read(int delta) {
    if (!payload || constructors != 1 || errno != 42 || local < 17) return -1;
    local += delta;
    return local;
}
int profile_close(int mode) {
    if (mode == 1) {
        atomic_store(&release_children, 1);
        __builtin_wasm_memory_atomic_notify((int *)&release_children, UINT32_MAX);
    }
    struct timespec deadline = {0, mode == 2 ? 1000000000 : 0};
    int status = metallic_threads_close(mode == 1 ? NULL : &deadline);
    if (status != thrd_success) return status;
    if (payload) {
        REQUIRE(!results[0] && !results[1]);
        if (atomic_load(&ready)) REQUIRE(payload[1] == 31 && payload[2] == 32);
        free(payload); payload = NULL;
    }
    thrd_t extra;
    REQUIRE(thrd_create(&extra, child, NULL) == thrd_error);
    return thrd_success;
}
int profile_constructor_count(void) { return (int)constructors; }
int profile_spin(void) { for (;;) atomic_signal_fence(memory_order_seq_cst); }

static int clock_child(void *argument) {
    REQUIRE(clock() == *(const clock_t *)argument);
    return 0;
}
int profile_clock(int available) {
    /* Above 32 bits to detect truncation; the provider also supplies fractional
     * microseconds to verify conversion to CLOCKS_PER_SEC. */
    clock_t expected = available ? (clock_t)4294967301LL : (clock_t)-1;
    REQUIRE(clock() == expected);
    thrd_t worker;
    REQUIRE(thrd_create(&worker, clock_child, &expected) == thrd_success);
    int result;
    REQUIRE(thrd_join(worker, &result) == thrd_success && result == 0);
    return 0;
}

#ifdef GUEST_TLS_BYTES
static _Thread_local volatile unsigned char tls_probe[GUEST_TLS_BYTES]
    __attribute__((aligned(GUEST_TLS_ALIGNMENT)));
static int tls_child(void *argument) { (void)argument; return 0; }
int profile_tls_limit(int index) {
    unsigned offset = (unsigned)index % sizeof(tls_probe);
    tls_probe[offset] = 83;
    thrd_t output = {99, 99};
    REQUIRE(thrd_create(&output, tls_child, NULL) == thrd_nomem);
    REQUIRE(output.slot == 99 && output.generation == 99);
    REQUIRE(tls_probe[offset] == 83 && local == 17 && errno == 42);
    return 0;
}
#endif

#ifdef GUEST_PROFILE_COMMAND
int main(void) {
    int status = profile_begin(0);
    if (status) return status;
#ifdef GUEST_COMMAND_EXIT_LIVE
    return 23;
#else
    return profile_close(1);
#endif
}
#endif
