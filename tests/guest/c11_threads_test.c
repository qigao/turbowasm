#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>
#include <turbowasm/wasi_threads.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const uint8_t provider_bytes[] = {
    0,97,115,109,1,0,0,0,
    5,5,1,3,16,0x80,2,
    7,10,1,6,'m','e','m','o','r','y',2,0
};
static turbowasm_module module, memory_module;
static turbowasm_instance root, memory;
static turbowasm_wasi_threads threads;
static turbowasm_wasi_threads_execution_policy policy;
static turbowasm_wasi_preview1 wasi;
static turbowasm_linker linker;
static uint8_t *bytes;
static atomic_uint writes, reads, io_errors;

static turbowasm_name name(const char *s) { return (turbowasm_name){(const uint8_t *)s, strlen(s)}; }
static uint32_t clock_time(void *context, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)context; (void)precision;
    if (id > 1) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = id ? cmeta_hrtime() : cmeta_realtime_ms() * UINT64_C(1000000);
    return 0;
}
static uint32_t write_output(void *context, uint32_t fd, const turbowasm_wasi_const_buffer *buffers,
    size_t count, uint32_t *written) {
    (void)context;
    atomic_fetch_add(&writes, 1);
    if (count == 0) { *written = 0; return 0; }
    if (fd == 42) {
        if (count != 1 || buffers[0].size != 1024u * 1024u) {
            atomic_fetch_add(&io_errors, 1); return TURBOWASM_WASI_ERRNO_IO;
        }
        for (size_t i = 0; i < buffers[0].size; ++i) if (((const char *)buffers[0].data)[i] != 'x') {
            atomic_fetch_add(&io_errors, 1); break;
        }
        *written = 1024u * 1024u; return 0;
    }
    if (fd != 1 || count != 2 || buffers[0].size != 2 || buffers[1].size != 4 ||
        memcmp(buffers[0].data, "ab", 2) || memcmp(buffers[1].data, "cdef", 4)) {
        atomic_fetch_add(&io_errors, 1); return TURBOWASM_WASI_ERRNO_IO;
    }
    *written = 5; return 0;
}
static uint32_t read_input(void *context, uint32_t fd, const turbowasm_wasi_buffer *buffers,
    size_t count, uint32_t *read) {
    (void)context;
    atomic_fetch_add(&reads, 1);
    if (fd == 43 || fd == 44) {
        if (count != 1 || buffers[0].size != 4) {
            atomic_fetch_add(&io_errors, 1); return TURBOWASM_WASI_ERRNO_IO;
        }
        memset(buffers[0].data, '!', 4);
        *read = 5;
        return fd == 43 ? TURBOWASM_WASI_ERRNO_IO : 0;
    }
    if (fd != 0 || count != 2 || buffers[0].size != 2 || buffers[1].size != 4) {
        atomic_fetch_add(&io_errors, 1); return TURBOWASM_WASI_ERRNO_IO;
    }
    memcpy(buffers[0].data, "ab", 2); memcpy(buffers[1].data, "cdef", 4);
    *read = 5; return 0;
}
static turbowasm_status no_descriptor_operation(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)argc;
    if (!results || !capacity) return TURBOWASM_INVALID_ARGUMENT;
    results[0] = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = TURBOWASM_WASI_ERRNO_NOTCAPABLE};
    *count = 1; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static void call_test(const char *symbol) {
    uint32_t index = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) index = e->item_index;
    }
    check_not_equal(index, UINT32_MAX);
    turbowasm_execution_options options = {.fuel = 100000000, .has_fuel_limit = true};
    check_true(turbowasm_wasi_threads_execution_policy_apply(&policy, &options));
    turbowasm_value result = {0};
    size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = turbowasm_instance_invoke_with_options(&root, index, NULL, 0,
        &result, 1, &count, &trap, &options);
    info("%s: status %d, trap %d, guest failure line %d", symbol, status, trap, result.as.i32);
    check_equal(status, TURBOWASM_OK);
    check_equal(count, (size_t)1); check_equal(result.as.i32, 0);
}

spec("Metallic C11 threads internal profile") {
    before_all() {
        FILE *file = fopen(GUEST_C11_THREADS_PATH, "rb");
        check_not_null(file);
        check_equal(fseek(file, 0, SEEK_END), 0);
        long size = ftell(file);
        check_true(size > 0 && size <= 4 * 1024 * 1024);
        check_equal(fseek(file, 0, SEEK_SET), 0);
        bytes = malloc((size_t)size); check_not_null(bytes);
        size_t read = fread(bytes, 1, (size_t)size, file);
        fclose(file); check_equal(read, (size_t)size);
        check_equal(turbowasm_module_load_borrowed(&module, bytes, (size_t)size), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed(&memory_module, provider_bytes, sizeof(provider_bytes)), TURBOWASM_OK);
    }
    after_all() {
        turbowasm_module_destroy(&module); free(bytes);
        turbowasm_module_destroy(&memory_module);
    }
    before_each() {
        atomic_store(&writes, 0); atomic_store(&reads, 0); atomic_store(&io_errors, 0);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 4), TURBOWASM_OK);
        check_true(turbowasm_wasi_threads_execution_policy_init(&policy, &threads));
        check_equal(turbowasm_instance_create(&memory, &memory_module), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("env"), &memory), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_define(&threads, &linker), TURBOWASM_OK);
        turbowasm_wasi_preview1_config config = {0};
        const char *args[] = {"threads"}, *environment[] = {"TURBOWASM_THREADS=supported"};
        config.allow_args = true; config.args = args; config.arg_count = 1;
        config.allow_environ = true; config.environment = environment; config.environment_count = 1;
        config.allow_clock = true; config.clock_time = clock_time;
        config.allow_proc_exit = true; config.proc_exit = turbowasm_wasi_threads_proc_exit; config.proc_exit_context = &threads;
        config.allow_fd_write = true; config.fd_write = write_output;
        config.allow_fd_read = true; config.fd_read = read_input;
        check_equal(turbowasm_wasi_preview1_init(&wasi, &config), TURBOWASM_OK);
        check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
        turbowasm_value_kind i32 = TURBOWASM_VALUE_I32;
        turbowasm_host_function_type close_type = {&i32, 1, &i32, 1};
        turbowasm_value_kind seek_args[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        turbowasm_host_function_type seek_type = {seek_args, 4, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_close"), &close_type, no_descriptor_operation, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_seek"), &seek_type, no_descriptor_operation, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&root, &module, &linker), TURBOWASM_OK);
        call_test("initialize");
    }
    after_each() {
        turbowasm_wasi_threads_proc_exit(&threads, &root, 0);
        while (turbowasm_wasi_threads_active(&threads)) cmeta_thread_yield();
        turbowasm_instance_destroy(&root);
        check_true(turbowasm_wasi_threads_destroy(&threads));
        turbowasm_wasi_preview1_destroy(&wasi);
        turbowasm_linker_destroy(&linker);
        turbowasm_instance_destroy(&memory);
    }
    it("creates, identifies, joins and exits threads with stale-handle rejection") { call_test("lifecycle"); }
    it("serializes contended updates and once publication with recursive locks") { call_test("synchronization"); }
    it("waits for predicates, broadcasts and honors real clock deadlines") { call_test("conditions"); }
    it("isolates TSS keys and runs bounded destructor iterations on return and exit") { call_test("thread_specific"); }
    it("preserves allocator contents and alignment under contention") { call_test("allocation"); }
    it("copies shared vector IO with short reads and bounds before provider effects") {
        call_test("shared_io");
        check_equal(atomic_load(&writes), 6u);
        check_equal(atomic_load(&reads), 6u);
        check_equal(atomic_load(&io_errors), 0u);
    }
    it("rolls back rejected spawn and recovers capacity after join and detach") { call_test("capacity"); }
    it("retains timed-out drain state and tolerates concurrent child joins") { call_test("drain_join"); }
}
