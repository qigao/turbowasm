#include <threads.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include "../../guest/metallic/src/wasi/preopen.h"

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
enum { WORKERS = 4, DRAWS = 128 };
static int reference[WORKERS * DRAWS], observed[WORKERS * DRAWS];
static atomic_uint ready, release_workers;
static uintptr_t calendar_addresses[WORKERS], text_addresses[WORKERS];

static int isolated_worker(void *argument) {
    unsigned slot = (unsigned)(uintptr_t)argument;
    time_t time = (time_t)slot * 86400;
    struct tm *calendar = gmtime(&time), *local = localtime(&time);
    char *text = asctime(calendar);
    char saved[26]; memcpy(saved, text, sizeof saved);
    calendar_addresses[slot] = (uintptr_t)calendar;
    text_addresses[slot] = (uintptr_t)text;
    char tokens[] = {(char)('A' + slot), ',', (char)('a' + slot), 0};
    REQUIRE(strtok(tokens, ",") == tokens);
    for (unsigned i = 0; i < DRAWS; ++i) observed[slot * DRAWS + i] = rand();
    atomic_fetch_add(&ready, 1);
    __builtin_wasm_memory_atomic_notify((int *)&ready, UINT32_MAX);
    while (!atomic_load(&release_workers))
        __builtin_wasm_memory_atomic_wait32((int *)&release_workers, 0, -1);
    REQUIRE(calendar->tm_mday == (int)slot + 1 && local->tm_mday == (int)slot + 1);
    REQUIRE(memcmp(saved, text, sizeof saved) == 0);
    char *tail = strtok(NULL, ",");
    REQUIRE(tail == tokens + 2 && *tail == 'a' + (int)slot && strtok(NULL, ",") == NULL);
    return 0;
}
static int compare_int(const void *a, const void *b) {
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}
int libc_state(void) {
    srand(1);
    for (unsigned i = 0; i < WORKERS * DRAWS; ++i) reference[i] = rand();
    srand(1);
    time_t time = 10 * 86400;
    struct tm *calendar = gmtime(&time);
    char *text = asctime(calendar), saved[26]; memcpy(saved, text, sizeof saved);
    char tokens[] = "root,tail";
    REQUIRE(strtok(tokens, ",") == tokens);
    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(thrd_create(&children[i], isolated_worker, (void *)(uintptr_t)i) == thrd_success);
    while (atomic_load(&ready) != WORKERS) {
        unsigned value = atomic_load(&ready);
        if (value != WORKERS) __builtin_wasm_memory_atomic_wait32((int *)&ready, (int)value, -1);
    }
    REQUIRE(calendar->tm_mday == 11 && memcmp(saved, text, sizeof saved) == 0);
    REQUIRE(strcmp(strtok(NULL, ","), "tail") == 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        REQUIRE(calendar_addresses[i] != (uintptr_t)calendar && text_addresses[i] != (uintptr_t)text);
        for (unsigned j = 0; j < i; ++j)
            REQUIRE(calendar_addresses[i] != calendar_addresses[j] && text_addresses[i] != text_addresses[j]);
    }
    atomic_store(&release_workers, 1);
    __builtin_wasm_memory_atomic_notify((int *)&release_workers, UINT32_MAX);
    for (unsigned i = 0; i < WORKERS; ++i) {
        int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    qsort(reference, WORKERS * DRAWS, sizeof(int), compare_int);
    qsort(observed, WORKERS * DRAWS, sizeof(int), compare_int);
    REQUIRE(memcmp(reference, observed, sizeof reference) == 0);
    return 0;
}

static unsigned normal_calls[WORKERS], quick_calls[WORKERS], callback_order, callback_error;
#define CALLBACKS(n) \
    static void normal_##n(void) { ++normal_calls[n]; if (callback_order == 1) callback_error = 1; } \
    static void quick_##n(void) { ++quick_calls[n]; }
CALLBACKS(0) CALLBACKS(1) CALLBACKS(2) CALLBACKS(3)
static void (*const normal_callbacks[WORKERS])(void) = {normal_0, normal_1, normal_2, normal_3};
static void (*const quick_callbacks[WORKERS])(void) = {quick_0, quick_1, quick_2, quick_3};
static int register_callbacks(void *argument) {
    unsigned slot = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < 7; ++i) REQUIRE(atexit(normal_callbacks[slot]) == 0);
    for (unsigned i = 0; i < 8; ++i) REQUIRE(at_quick_exit(quick_callbacks[slot]) == 0);
    return 0;
}
static void additional_callback(void) {
    if (callback_order != 1) callback_error = 2;
    callback_order = 2;
}
static void registering_callback(void) {
    callback_order = 1;
    if (atexit(additional_callback)) callback_error = 3;
}
extern void __run_atexit_(void), __run_quick_exit_(void);
int libc_exit_callbacks(void) {
    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(thrd_create(&children[i], register_callbacks, (void *)(uintptr_t)i) == thrd_success);
    for (unsigned i = 0; i < WORKERS; ++i) {
        int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    REQUIRE(atexit(registering_callback) == 0);
    for (unsigned i = 0; i < 3; ++i) REQUIRE(atexit(normal_0) == 0);
    REQUIRE(atexit(normal_0) != 0 && at_quick_exit(quick_0) != 0);
    __run_atexit_();
    REQUIRE(callback_order == 2 && callback_error == 0);
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(normal_calls[i] == (i ? 7 : 10) && quick_calls[i] == 0);
    __run_quick_exit_();
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(quick_calls[i] == 8);
    __run_atexit_(); __run_quick_exit_();
    REQUIRE(atexit(normal_0) == 0 && at_quick_exit(quick_0) == 0);
    __run_atexit_(); __run_quick_exit_();
    REQUIRE(normal_calls[0] == 11 && quick_calls[0] == 9);
    return 0;
}

static atomic_uint handled_signals;
static void handler_a(int signal_number) {
    if (signal_number == SIGUSR1) atomic_fetch_add(&handled_signals, 1);
    signal(SIGUSR1, handler_a);
}
static void handler_b(int signal_number) {
    if (signal_number == SIGUSR1) atomic_fetch_add(&handled_signals, 1);
}
static int signal_worker(void *argument) {
    unsigned slot = (unsigned)(uintptr_t)argument;
    for (unsigned i = 0; i < DRAWS; ++i) {
        REQUIRE(signal(SIGUSR1, slot & 1 ? handler_a : handler_b) != SIG_ERR);
        REQUIRE(raise(SIGUSR1) == 0);
    }
    return 0;
}
int libc_signals(void) {
    REQUIRE(signal(SIGUSR1, handler_a) != SIG_ERR);
    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(thrd_create(&children[i], signal_worker, (void *)(uintptr_t)i) == thrd_success);
    for (unsigned i = 0; i < WORKERS; ++i) {
        int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    REQUIRE(atomic_load(&handled_signals) == WORKERS * DRAWS);
    REQUIRE(signal(SIGUSR1, SIG_IGN) != SIG_ERR && raise(SIGUSR1) == 0);
    REQUIRE(atomic_load(&handled_signals) == WORKERS * DRAWS);
    return 0;
}
int libc_abort_default(void) { abort(); }

static int preopen_worker(void *argument) {
    (void)argument;
    int base = -1; const char *relative = NULL; size_t length = 0;
    REQUIRE(preopen_lookup("./file.txt", &base, &relative, &length) == 0);
    REQUIRE(base == 3 && length == 8 && memcmp(relative, "file.txt", 8) == 0);
    return 0;
}
int libc_preopens(void) {
    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) REQUIRE(thrd_create(&children[i], preopen_worker, NULL) == thrd_success);
    for (unsigned i = 0; i < WORKERS; ++i) {
        int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    return preopen_worker(NULL);
}
static int preopen_failure(int expected) {
    int base = 99; const char *relative = NULL; size_t length = 99;
    for (unsigned i = 0; i < 2; ++i) {
        errno = 0;
        REQUIRE(preopen_lookup("file.txt", &base, &relative, &length) == -1 && errno == expected);
        REQUIRE(base == 99 && relative == NULL && length == 99);
    }
    return 0;
}
int libc_preopen_denied(void) { return preopen_failure(EACCES); }
int libc_preopen_large(void) { return preopen_failure(ENAMETOOLONG); }
int libc_preopen_invalid(void) { return preopen_failure(EIO); }
