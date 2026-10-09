#include "tinytest.h"
#include "reactor_session.h"
#include <turbowasm/wasi.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef TURBOWASM_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"

static uint32_t exported_function(const turbowasm_module *m, const char *symbol) {
    for (size_t i = 0; i < turbowasm_module_export_count(m); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(m, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) return e->item_index;
    }
    check(false);
    return UINT32_MAX;
}
#endif

static turbowasm_module module, consumer_module;
static uint8_t *bytes, *consumer_bytes;
static turbowasm_module sjlj_modules[2];
static uint8_t *sjlj_bytes[2];
#ifdef GUEST_CMETA_PATH
static turbowasm_module cmeta_module;
static uint8_t *cmeta_bytes;
#endif
static reactor_session session, other;
static turbowasm_wasi_preview1 wasi;
static turbowasm_linker linker;
static turbowasm_trap trap;
static int init_mode, reentry_mode;
static turbowasm_status nested_status;
static bool nested_destroy, exited, fail_write;
static uint32_t exit_code;
static char output[64];
static size_t output_size;
static atomic_bool host_entered, host_release;
static cmeta_thread_t worker;
static bool worker_started;
static turbowasm_status worker_status;

static turbowasm_name name(const char *text) {
    return (turbowasm_name){(const uint8_t *)text, strlen(text)};
}
static void load(const char *path, turbowasm_module *out, uint8_t **storage) {
    FILE *f = fopen(path, "rb");
    check_not_null(f);
    int seek = fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (seek || size <= 0 || size > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f); check(false); return;
    }
    *storage = malloc((size_t)size);
    if (!*storage) { fclose(f); check(false); return; }
    size_t read = fread(*storage, 1, (size_t)size, f);
    fclose(f);
    check_equal(read, (size_t)size);
    turbowasm_runtime_config config = {0};
    config.limits.max_module_bytes = 16 * 1024 * 1024;
    config.limits.max_allocation_bytes = 64 * 1024 * 1024;
    config.limits.max_linear_memory_bytes = 1024 * 1024;
    config.limits.max_table_elements = 1024;
    check_equal(turbowasm_module_load_borrowed_with_config(out, *storage, (size_t)size, &config), TURBOWASM_OK);
}
static turbowasm_status open_sjlj(reactor_session *s, int mode, uint64_t fuel) {
    turbowasm_status status = reactor_session_open(s, &sjlj_modules[mode], &linker, fuel, &trap);
#ifdef TURBOWASM_TEST_MIR
    if (status == TURBOWASM_OK) {
        turbowasm_jit_backend backend = {0};
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(s->instance.impl, &backend, 1), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIXED
        /* Deliberately cross both directions: outer MIR -> interpreted leaf
         * -> MIR longjmp, then propagate the exception back to its handler. */
        turbowasm_instance_impl *impl = s->instance.impl;
        impl->jit_functions[exported_function(s->module, "sjlj_leaf")].state = TURBOWASM_JIT_INTERPRET_ONLY;
#endif
    }
#endif
    return status;
}
static turbowasm_status call(reactor_session *s, const char *symbol, int *argument, int32_t *out) {
    turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32}, result = {0};
    if (argument) arg.as.i32 = *argument;
    size_t count = 0;
    turbowasm_trap local_trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = reactor_session_call(s, symbol, argument ? &arg : NULL,
        argument ? 1 : 0, out ? &result : NULL, out ? 1 : 0, &count, &local_trap);
    if (status == TURBOWASM_OK && out) {
        if (count != 1 || result.kind != TURBOWASM_VALUE_I32) return TURBOWASM_TYPE_MISMATCH;
        *out = result.as.i32;
    }
#ifdef TURBOWASM_TEST_MIR
    if (status == TURBOWASM_OK && s->instance.impl &&
        ((turbowasm_instance_impl *)s->instance.impl)->jit_backend_attached) {
        turbowasm_instance_impl *impl = s->instance.impl;
        check_equal(impl->jit_functions[exported_function(s->module, symbol)].state, TURBOWASM_JIT_COMPILED);
    }
#endif
    return status;
}
static turbowasm_status control(void *context, turbowasm_host_call *host,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *out_trap) {
    (void)context; (void)host;
    if (argc != 1 || !args || capacity != 1 || !results) return TURBOWASM_INVALID_ARGUMENT;
    int value = init_mode;
    if (args[0].as.i32 == 1) {
        value = 42;
        if (reentry_mode == 1) {
            int32_t ignored;
            nested_status = call(&session, "initialized", NULL, &ignored);
            nested_destroy = reactor_session_destroy(&session);
        } else if (reentry_mode == 2) {
            atomic_store(&host_entered, true);
            while (!atomic_load(&host_release)) cmeta_thread_yield();
        }
    }
    results[0] = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = value};
    *count = 1; *out_trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static uint32_t write_output(void *context, uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *written) {
    (void)context;
    *written = 0;
    if (fail_write) return TURBOWASM_WASI_ERRNO_IO;
    if (fd != 1) return TURBOWASM_WASI_ERRNO_BADF;
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].size > sizeof(output) - output_size) return TURBOWASM_WASI_ERRNO_IO;
        memcpy(output + output_size, buffers[i].data, buffers[i].size);
        output_size += buffers[i].size;
        *written += (uint32_t)buffers[i].size;
    }
    return 0;
}
static uint32_t read_input(void *context, uint32_t fd,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *read) {
    (void)context; (void)buffers; (void)count;
    *read = 0;
    return fd == 0 ? 0 : TURBOWASM_WASI_ERRNO_BADF;
}
/* The fixture's streams are pipes, with no filesystem capability. Metallic's
 * stream vtables reference seek even when this guest only writes output. */
static turbowasm_status pipe_seek(void *context, turbowasm_host_call *host,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *out_trap) {
    (void)context; (void)host; (void)args;
    if (argc != 4 || !results || capacity != 1) return TURBOWASM_INVALID_ARGUMENT;
    results[0] = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = TURBOWASM_WASI_ERRNO_NOTSUP};
    *count = 1; *out_trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static void on_exit(void *context, turbowasm_instance *instance, uint32_t code) {
    (void)context; (void)instance; exited = true; exit_code = code;
}
static void invoke_worker(void *arg) {
    (void)arg;
    int32_t result;
    worker_status = call(&session, "ask_host", NULL, &result);
}

spec("Metallic Reactor lifecycle") {
    before_all() {
        load(GUEST_REACTOR_PATH, &module, &bytes);
        load(GUEST_REACTOR_CONSUMER_PATH, &consumer_module, &consumer_bytes);
        load(GUEST_SJLJ_OPTIMIZED_PATH, &sjlj_modules[0], &sjlj_bytes[0]);
        load(GUEST_SJLJ_UNOPTIMIZED_PATH, &sjlj_modules[1], &sjlj_bytes[1]);
#ifdef GUEST_CMETA_PATH
        load(GUEST_CMETA_PATH, &cmeta_module, &cmeta_bytes);
#endif
    }
    after_all() {
#ifdef GUEST_CMETA_PATH
        turbowasm_module_destroy(&cmeta_module); free(cmeta_bytes);
#endif
        for (int i = 0; i < 2; ++i) { turbowasm_module_destroy(&sjlj_modules[i]); free(sjlj_bytes[i]); }
        turbowasm_module_destroy(&consumer_module); free(consumer_bytes);
        turbowasm_module_destroy(&module); free(bytes);
    }
    before_each() {
        memset(&session, 0, sizeof(session)); memset(&other, 0, sizeof(other));
        atomic_init(&session.busy, false); atomic_init(&other.busy, false);
        atomic_init(&host_entered, false); atomic_init(&host_release, false);
        init_mode = reentry_mode = 0; nested_status = TURBOWASM_OK; nested_destroy = true;
        exited = fail_write = worker_started = false; output_size = 0;
        memset(output, 0, sizeof(output)); trap = TURBOWASM_TRAP_NONE;
        turbowasm_wasi_preview1_config wc = {0};
        wc.allow_fd_write = true; wc.fd_write = write_output;
        wc.allow_fd_read = true; wc.fd_read = read_input;
        wc.allow_proc_exit = true; wc.proc_exit = on_exit;
        check_equal(turbowasm_wasi_preview1_init(&wasi, &wc), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
        turbowasm_value_kind i32 = TURBOWASM_VALUE_I32;
        turbowasm_host_function_type type = {&i32, 1, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("test"), name("control"), &type, control, NULL), TURBOWASM_OK);
        turbowasm_value_kind seek_params[] = {TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I64, TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32};
        turbowasm_host_function_type seek_type = {seek_params, 4, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_seek"), &seek_type, pipe_seek, NULL), TURBOWASM_OK);
    }
    after_each() {
        atomic_store(&host_release, true);
        if (worker_started) check_equal(cmeta_thread_join(&worker), 0);
        check_true(reactor_session_destroy(&other));
        check_true(reactor_session_destroy(&session));
        turbowasm_linker_destroy(&linker); turbowasm_wasi_preview1_destroy(&wasi);
    }
    it("initializes once, keeps allocated state and isolates independent instances") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        check_equal(reactor_session_open(&other, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        int argument = 2; int32_t result;
        for (int i = 1; i <= 4; ++i) {
            check_equal(call(&session, "step", &argument, &result), TURBOWASM_OK); check_equal(result, 6 * i);
            check_equal(call(&session, "initialized", NULL, &result), TURBOWASM_OK); check_equal(result, 1);
        }
        check_equal(call(&other, "step", &argument, &result), TURBOWASM_OK); check_equal(result, 6);
    }
    it("keeps the instance usable after a business error and reuses bounded allocations") {
        check_equal(reactor_session_open(&session, &module, &linker, 10000000, &trap), TURBOWASM_OK);
        int argument = -1; int32_t result;
        check_equal(call(&session, "step", &argument, &result), TURBOWASM_OK); check_equal(result, -1);
        for (int i = 0; i < 8; ++i) {
            check_equal(call(&session, "allocation_cycle", NULL, &result), TURBOWASM_OK); check_equal(result, 0);
        }
        check_equal(session.state, REACTOR_READY);
    }
    it("rejects repeated host initialization without resetting the heap") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(call(&session, "_initialize", NULL, NULL), TURBOWASM_INVALID_ARGUMENT);
        int argument = 1; int32_t result;
        check_equal(call(&session, "step", &argument, &result), TURBOWASM_OK); check_equal(result, 3);
    }
    it("rejects missing initialization, a start section and the wrong initializer signature before instantiation") {
        static const uint8_t missing[] = {0,97,115,109,1,0,0,0};
        static const uint8_t start[] = {
            0,97,115,109,1,0,0,0, 1,4,1,0x60,0,0, 3,2,1,0,
            7,15,1,11,'_','i','n','i','t','i','a','l','i','z','e',0,0,
            8,1,0, 10,4,1,2,0,0x0b
        };
        static const uint8_t wrong[] = {
            0,97,115,109,1,0,0,0, 1,5,1,0x60,0,1,0x7e, 3,2,1,0,
            7,15,1,11,'_','i','n','i','t','i','a','l','i','z','e',0,0,
            10,6,1,4,0,0x42,0,0x0b
        };
        const uint8_t *fixtures[] = {missing, start, wrong};
        const size_t sizes[] = {sizeof(missing), sizeof(start), sizeof(wrong)};
        for (size_t i = 0; i < 3; ++i) {
            turbowasm_module invalid = {0};
            check_equal(turbowasm_module_load_borrowed(&invalid, fixtures[i], sizes[i]), TURBOWASM_OK);
            turbowasm_status status = reactor_session_open(&session, &invalid, NULL, 1000, &trap);
            turbowasm_module_destroy(&invalid);
            check_equal(status, TURBOWASM_UNSUPPORTED);
            check_equal(session.state, REACTOR_EMPTY); check_null(session.instance.impl);
        }
    }
    it("traps recursive CRT initialization before restarting constructors") {
        init_mode = 3;
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_TRAPPED);
        check_equal(session.state, REACTOR_FAILED);
    }
    it("retires instances when constructors trap or exhaust their budget") {
        init_mode = 1;
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_TRAPPED);
        check_equal(call(&session, "initialized", NULL, NULL), TURBOWASM_INVALID_ARGUMENT);
        init_mode = 2;
        check_equal(reactor_session_open(&other, &module, &linker, 1000, &trap), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(other.state, REACTOR_FAILED);
    }
    it("flushes explicit application cleanup and makes close terminal") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        int32_t application_status = -1;
        check_equal(reactor_session_close(&session, "finish", &application_status, &trap), TURBOWASM_OK);
        check_equal(application_status, 0); check_equal(output, "closed");
        check_equal(session.state, REACTOR_CLOSED);
        check_equal(call(&session, "step", NULL, NULL), TURBOWASM_INVALID_ARGUMENT);
    }
    it("reports a failed output flush during terminal application cleanup") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        fail_write = true; int32_t application_status = 0;
        check_equal(reactor_session_close(&session, "finish", &application_status, &trap), TURBOWASM_OK);
        check_equal(application_status, -2); check_equal(session.state, REACTOR_CLOSED);
    }
    it("does not call guest cleanup after a trap") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        check_equal(call(&session, "fail", NULL, NULL), TURBOWASM_TRAPPED);
        int32_t application_status;
        check_equal(reactor_session_close(&session, "finish", &application_status, &trap), TURBOWASM_INVALID_ARGUMENT);
        check_equal(output_size, 0u); check_true(reactor_session_destroy(&session));
    }
    it("treats proc_exit and business fuel exhaustion as terminal") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        check_equal(call(&session, "terminate", NULL, NULL), TURBOWASM_INTERRUPTED);
        check_true(exited); check_equal(exit_code, 37u); check_equal(session.state, REACTOR_FAILED);
        check_equal(reactor_session_open(&other, &module, &linker, 100000, &trap), TURBOWASM_OK);
        check_equal(call(&other, "forever", NULL, NULL), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(other.state, REACTOR_FAILED);
    }
    it("rejects reentrant invocation and destruction from a host callback") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        reentry_mode = 1; int32_t result;
        check_equal(call(&session, "ask_host", NULL, &result), TURBOWASM_OK); check_equal(result, 42);
        check_equal(nested_status, TURBOWASM_INVALID_ARGUMENT); check_false(nested_destroy);
    }
    it("rejects concurrent calls and teardown while another owner is active") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        reentry_mode = 2;
        check_equal(cmeta_thread_create(&worker, invoke_worker, NULL), 0); worker_started = true;
        while (!atomic_load(&host_entered)) cmeta_thread_yield();
        int32_t result;
        check_equal(call(&session, "initialized", NULL, &result), TURBOWASM_INVALID_ARGUMENT);
        check_false(reactor_session_destroy(&session));
        atomic_store(&host_release, true);
        check_equal(cmeta_thread_join(&worker), 0); worker_started = false;
        check_equal(worker_status, TURBOWASM_OK);
    }
    it("links a second compiled Reactor to a retained provider instance") {
        check_equal(reactor_session_open(&session, &module, &linker, 1000000, &trap), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("provider"), &session.instance), TURBOWASM_OK);
        check_equal(reactor_session_open(&other, &consumer_module, &linker, 1000000, &trap), TURBOWASM_OK);
        turbowasm_linker_destroy(&linker);
        int argument = 4; int32_t result;
        check_equal(call(&other, "use_provider", &argument, &result), TURBOWASM_OK); check_equal(result, 13);
        check_equal(call(&other, "use_provider", &argument, &result), TURBOWASM_OK); check_equal(result, 25);
    }
    it("implements zero, positive, negative and extreme longjmp values at O0 and LTO") {
        int values[] = {0, 1, 7, -9, INT_MIN, INT_MAX};
        reactor_session *sessions[] = {&session, &other};
        for (int mode = 0; mode < 2; ++mode) {
            check_equal(open_sjlj(sessions[mode], mode, 1000000), TURBOWASM_OK);
            for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
                int32_t result = -1;
                check_equal(call(sessions[mode], "sjlj_values", &values[i], &result), TURBOWASM_OK);
                check_equal(result, 0);
            }
        }
    }
    it("restores nested continuations, volatile locals and the shadow stack at O0 and LTO") {
        const char *cases[] = {"sjlj_nested", "sjlj_repeated", "sjlj_restore_stack", "sjlj_callback"};
        reactor_session *sessions[] = {&session, &other};
        for (int mode = 0; mode < 2; ++mode) {
            check_equal(open_sjlj(sessions[mode], mode, 10000000), TURBOWASM_OK);
            for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                int32_t result = -1;
                info("SJLJ mode %d, case %s", mode, cases[i]);
                check_equal(call(sessions[mode], cases[i], NULL, &result), TURBOWASM_OK);
                check_equal(result, 0);
            }
        }
    }
    it("does not catch traps or bypass fuel limits with longjmp") {
        check_equal(open_sjlj(&session, 0, 100000), TURBOWASM_OK);
        int32_t result;
        check_equal(call(&session, "sjlj_trap", NULL, &result), TURBOWASM_TRAPPED);
        check_equal(session.state, REACTOR_FAILED);
        check_equal(open_sjlj(&other, 1, 100000), TURBOWASM_OK);
        check_equal(call(&other, "sjlj_fuel", NULL, &result), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(other.state, REACTOR_FAILED);
    }
    it("preserves the identity of an unrelated Wasm exception at O0 and LTO") {
        reactor_session *sessions[] = {&session, &other};
        for (int mode = 0; mode < 2; ++mode) {
            check_equal(open_sjlj(sessions[mode], mode, 1000000), TURBOWASM_OK);
            int32_t result;
            check_equal(call(sessions[mode], "sjlj_unrelated_exception", NULL, &result), TURBOWASM_EXCEPTION);
            check_equal(sessions[mode]->state, REACTOR_FAILED);
        }
    }
#ifdef GUEST_CMETA_PATH
    it("runs CMeta metadata, exact call admission and over-aligned lifecycle in a Metallic guest") {
        check_equal(reactor_session_open(&session, &cmeta_module, NULL, 10000000, &trap), TURBOWASM_OK);
        const char *cases[] = {"cmeta_metadata", "cmeta_calls", "cmeta_alignment"};
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            int32_t result = -1;
            info("CMeta guest case %s", cases[i]);
            check_equal(call(&session, cases[i], NULL, &result), TURBOWASM_OK);
            check_equal(result, 0);
        }
    }
    it("retains CMeta objects between calls and keeps ownership local to each instance") {
        check_equal(reactor_session_open(&session, &cmeta_module, NULL, 10000000, &trap), TURBOWASM_OK);
        check_equal(reactor_session_open(&other, &cmeta_module, NULL, 10000000, &trap), TURBOWASM_OK);
        int32_t result = -1;
        check_equal(call(&session, "cmeta_object_open", NULL, &result), TURBOWASM_OK); check_equal(result, 42);
        check_equal(call(&other, "cmeta_object_read", NULL, &result), TURBOWASM_OK); check_equal(result, -1);
        check_equal(call(&session, "cmeta_object_read", NULL, &result), TURBOWASM_OK); check_equal(result, 42);
        check_equal(reactor_session_close(&session, "cmeta_object_close", &result, &trap), TURBOWASM_OK);
        check_equal(result, 1);
        check_equal(reactor_session_close(&other, "cmeta_object_close", &result, &trap), TURBOWASM_OK);
        check_equal(result, 0);
    }
#endif
}
