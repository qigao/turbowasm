#include <tinytest.h>
#include "threaded_session.h"
#include <turbowasm/wasi.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { REACTOR, COMMAND, EXIT_LIVE, RECURSIVE, PROFILE_COUNT };
static turbowasm_module modules[PROFILE_COUNT], memory_module;
static uint8_t *bytes[PROFILE_COUNT];
static turbowasm_instance memory;
static turbowasm_linker linker;
static turbowasm_wasi_threads threads;
static turbowasm_wasi_preview1 wasi;
static threaded_session session;
static turbowasm_trap trap;
static bool clock_error, worker_started;
static atomic_bool requested, observe_call, call_entered;
static cmeta_thread_t worker;
static turbowasm_status worker_status;
static const uint8_t memory_bytes[] = {
    0,97,115,109,1,0,0,0,
    5,5,1,3,16,0x80,2,
    7,10,1,6,'m','e','m','o','r','y',2,0
};
static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, strlen(s)};
}
static uint32_t clock_time(void *context, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)context; (void)precision;
    if (clock_error || id > 1) return TURBOWASM_WASI_ERRNO_IO;
    *out = id ? cmeta_hrtime() : cmeta_realtime_ms() * UINT64_C(1000000);
    return 0;
}
static bool interrupt(void *context) {
    (void)context;
    if (atomic_load(&observe_call)) {
        atomic_store(&call_entered, true);
        while (!atomic_load(&requested)) cmeta_thread_yield();
    }
    return atomic_load(&requested);
}
static void invoke_worker(void *context) {
    (void)context;
    size_t count = 0; turbowasm_value result; turbowasm_trap worker_trap;
    worker_status = threaded_session_call(&session, "profile_spin", NULL, 0,
        &result, 1, &count, &worker_trap);
}
static turbowasm_status open_profile(unsigned which, uint64_t fuel) {
    turbowasm_execution_options options = {.fuel = fuel, .has_fuel_limit = true,
        .should_interrupt = interrupt};
    return threaded_session_open(&session, &modules[which], &linker, &threads,
        which == COMMAND || which == EXIT_LIVE, &options, &trap);
}
static int32_t call(const char *symbol, int32_t value, bool argument) {
    turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32, .as.i32 = value}, result = {0};
    size_t count = 0;
    turbowasm_status status = threaded_session_call(&session, symbol, argument ? &arg : NULL,
        argument ? 1 : 0, &result, 1, &count, &trap);
    info("%s: status %d, trap %d, result %d", symbol, status, trap, result.as.i32);
    check_equal(status, TURBOWASM_OK); check_equal(count, (size_t)1);
    return result.as.i32;
}
static int32_t close_profile(int32_t mode) {
    turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32, .as.i32 = mode};
    int32_t result = -1;
    check_equal(threaded_session_close(&session, "profile_close", &arg, 1, &result, &trap), TURBOWASM_OK);
    return result;
}
static void drain(void) {
    uint64_t start = cmeta_hrtime();
    while (turbowasm_wasi_threads_active(&threads) && cmeta_hrtime() - start < UINT64_C(10000000000))
        cmeta_thread_yield();
    check_equal(turbowasm_wasi_threads_active(&threads), (size_t)0);
}
static uint32_t exported(const char *symbol) {
    for (size_t i = 0; i < turbowasm_module_export_count(session.module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(session.module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) return e->item_index;
    }
    check(false); return UINT32_MAX;
}

spec("Installed-capable Metallic threaded profile") {
    before_all() {
        const char *paths[] = {GUEST_THREADED_REACTOR_PATH, GUEST_THREADED_COMMAND_PATH,
            GUEST_THREADED_EXIT_PATH, GUEST_THREADED_RECURSIVE_PATH};
        for (unsigned i = 0; i < PROFILE_COUNT; ++i) {
            FILE *f = fopen(paths[i], "rb"); check_not_null(f);
            check_equal(fseek(f, 0, SEEK_END), 0);
            long size = ftell(f); check_true(size > 0 && size <= 4 * 1024 * 1024);
            check_equal(fseek(f, 0, SEEK_SET), 0);
            bytes[i] = malloc((size_t)size); check_not_null(bytes[i]);
            size_t read = fread(bytes[i], 1, (size_t)size, f); fclose(f);
            check_equal(read, (size_t)size);
            check_equal(turbowasm_module_load_borrowed(&modules[i], bytes[i], (size_t)size), TURBOWASM_OK);
        }
        check_equal(turbowasm_module_load_borrowed(&memory_module, memory_bytes, sizeof(memory_bytes)), TURBOWASM_OK);
    }
    after_all() {
        for (unsigned i = 0; i < PROFILE_COUNT; ++i) {
            turbowasm_module_destroy(&modules[i]); free(bytes[i]);
        }
        turbowasm_module_destroy(&memory_module);
    }
    before_each() {
        session = (threaded_session){0}; clock_error = worker_started = false;
        atomic_store(&requested, false); atomic_store(&observe_call, false); atomic_store(&call_entered, false);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 4), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&memory, &memory_module), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("env"), &memory), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_define(&threads, &linker), TURBOWASM_OK);
        turbowasm_wasi_preview1_config config = {0};
        config.allow_clock = true; config.clock_time = clock_time;
        config.allow_proc_exit = true; config.proc_exit = turbowasm_wasi_threads_proc_exit;
        config.proc_exit_context = &threads;
        check_equal(turbowasm_wasi_preview1_init(&wasi, &config), TURBOWASM_OK);
        check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
    }
    after_each() {
        atomic_store(&requested, true);
        if (worker_started) {
            check_equal(cmeta_thread_join(&worker), SALTS_OK); cmeta_thread_destroy(&worker);
        }
        turbowasm_wasi_threads_proc_exit(&threads, &session.instance, 0);
        drain();
        check_true(threaded_session_destroy(&session));
        check_true(turbowasm_wasi_threads_destroy(&threads));
        turbowasm_wasi_preview1_destroy(&wasi);
        turbowasm_linker_destroy(&linker); turbowasm_instance_destroy(&memory);
    }
    it("initializes constructors and root TLS once and retains state across calls") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_constructor_count", 0, false), 1);
        check_equal(call("profile_read", 5, true), 22);
        check_equal(call("profile_begin", 0, true), 0);
        check_equal(call("profile_read", 7, true), 29);
        check_equal(close_profile(1), 0);
        check_equal(session.state, THREADED_CLOSED);
    }
    it("retains a live detached and joinable child on close timeout and permits retry") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_begin", 0, true), 0);
        void *original = session.instance.impl;
        check_equal(close_profile(0), 2);
        check_equal(session.state, THREADED_CLOSING);
        check_equal(session.instance.impl, original);
        check_false(threaded_session_destroy(&session));
        size_t count = 0; turbowasm_value result;
        check_equal(threaded_session_call(&session, "profile_constructor_count", NULL, 0,
            &result, 1, &count, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(close_profile(1), 0);
        drain(); check_true(threaded_session_destroy(&session));
    }
    it("retains closed admission on invalid deadlines and clock failure") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_begin", 0, true), 0);
        check_equal(close_profile(2), 4);
        clock_error = true;
        check_equal(close_profile(0), 4);
        check_false(threaded_session_destroy(&session));
        clock_error = false;
        check_equal(close_profile(1), 0);
    }
    it("rejects recursive CRT before resetting heap or running constructors again") {
        check_equal(open_profile(RECURSIVE, 10000000), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(session.state, THREADED_FAILED);
    }
    it("guards duplicate CRT while preserving existing guest state") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_read", 5, true), 22);
        size_t count = 0;
        turbowasm_execution_options options = {.fuel = 1000000, .has_fuel_limit = true};
        check_equal(turbowasm_instance_invoke_with_options(&session.instance, exported("_initialize"),
            NULL, 0, NULL, 0, &count, &trap, &options), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(call("profile_constructor_count", 0, false), 1);
        check_equal(call("profile_read", 1, true), 23);
        check_equal(close_profile(1), 0);
    }
    it("runs the threaded command CRT and preserves normal process exit") {
        check_equal(open_profile(COMMAND, 10000000), TURBOWASM_INTERRUPTED);
        uint32_t code = UINT32_MAX;
        check_true(turbowasm_wasi_threads_group_exit_code(&threads, &code));
        check_equal(code, 0u); drain();
    }
    it("lets main return terminate children instead of implicitly joining them") {
        check_equal(open_profile(EXIT_LIVE, 10000000), TURBOWASM_INTERRUPTED);
        uint32_t code = 0;
        check_true(turbowasm_wasi_threads_group_exit_code(&threads, &code));
        check_equal(code, 23u); drain();
    }
    it("bounds startup and propagates external interruption before CRT execution") {
        check_equal(open_profile(REACTOR, 1), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(session.state, THREADED_FAILED);
    }
    it("checks the root interruption policy during startup") {
        atomic_store(&requested, true);
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_INTERRUPTED);
        check_equal(session.state, THREADED_FAILED);
    }
    it("stops children after root fuel exhaustion and skips guest cleanup") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_begin", 0, true), 0);
        session.options.fuel = 64;
        size_t count = 0; turbowasm_value result;
        check_equal(threaded_session_call(&session, "profile_spin", NULL, 0,
            &result, 1, &count, &trap), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(session.state, THREADED_FAILED);
        int32_t application_status = -1;
        check_equal(threaded_session_close(&session, "profile_close", NULL, 0,
            &application_status, &trap), TURBOWASM_INVALID_ARGUMENT);
        drain();
    }
    it("retains the first child trap through host teardown") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32, .as.i32 = 1}, result;
        size_t count = 0;
        turbowasm_status status = threaded_session_call(&session, "profile_begin", &arg, 1,
            &result, 1, &count, &trap);
        check_true(status == TURBOWASM_INTERRUPTED || status == TURBOWASM_OK);
        drain();
        check_true(turbowasm_wasi_threads_group_fatal(&threads, &status, &trap));
        check_equal(status, TURBOWASM_TRAPPED); check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
    }
    it("rejects concurrent root calls, close and destroy while an invocation is active") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        atomic_store(&observe_call, true);
        check_equal(cmeta_thread_create(&worker, invoke_worker, NULL), SALTS_OK); worker_started = true;
        uint64_t start = cmeta_hrtime();
        while (!atomic_load(&call_entered) && cmeta_hrtime() - start < UINT64_C(10000000000))
            cmeta_thread_yield();
        check_true(atomic_load(&call_entered));
        size_t count = 0; turbowasm_value result;
        check_equal(threaded_session_call(&session, "profile_constructor_count", NULL, 0,
            &result, 1, &count, &trap), TURBOWASM_INVALID_ARGUMENT);
        int32_t application_status = -1;
        turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32, .as.i32 = 1};
        check_equal(threaded_session_close(&session, "profile_close", &arg, 1,
            &application_status, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_false(threaded_session_destroy(&session));
        atomic_store(&requested, true);
        check_equal(cmeta_thread_join(&worker), SALTS_OK); cmeta_thread_destroy(&worker); worker_started = false;
        check_equal(worker_status, TURBOWASM_INTERRUPTED);
    }
    it("allows repeated guest close after all child storage was reclaimed") {
        check_equal(open_profile(REACTOR, 10000000), TURBOWASM_OK);
        check_equal(call("profile_begin", 0, true), 0);
        check_equal(call("profile_close", 1, true), 0);
        check_equal(call("profile_close", 1, true), 0);
        check_equal(close_profile(1), 0);
    }
#ifdef GUEST_PROFILE_CMETA
    it("uses installed threaded CMeta archives with independent per-thread objects") {
        check_equal(open_profile(REACTOR, 100000000), TURBOWASM_OK);
        check_equal(call("cmeta_concurrent", 0, false), 0);
        check_equal(close_profile(1), 0);
    }
#endif
}
