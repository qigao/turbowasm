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
enum { STDIO_RECORDS = 1, STDIO_PROGRESS, STDIO_CLOSE_FLUSH, STDIO_ERROR_RELEASE, STDIO_REOPEN };
static unsigned stdio_mode;
static atomic_uint stdio_entered, stdio_release, stdio_fail_remaining, stdio_closes;
static cmeta_mutex_t stdio_mutex;
static char stdio_output[2][16384];
static size_t stdio_size[2], stdio_input_position;
static uint32_t preopen_error;
static bool preopen_large, preopen_invalid_tag;
static atomic_uint preopen_probes, preopen_names;

static turbowasm_status thread_failure(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args; (void)argc; (void)results; (void)capacity;
    *count = 0; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OUT_OF_MEMORY;
}

static turbowasm_status preopen_provider(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context;
    if (!args || (argc != 2 && argc != 3) || !results || !capacity) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t error = 0;
    uint8_t prestat[8] = {0,0,0,0,1,0,0,0};
    if (preopen_large) prestat[5] = 1;
    if (preopen_invalid_tag) prestat[0] = 1;
    if (argc == 2) atomic_fetch_add(&preopen_probes, 1);
    else atomic_fetch_add(&preopen_names, 1);
    if (preopen_error) error = preopen_error;
    else if (args[0].as.i32 != 3) error = TURBOWASM_WASI_ERRNO_BADF;
    else if (argc == 3 && args[2].as.i32 != 1) error = TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    else if (turbowasm_host_call_memory_write64(call, 0, (uint32_t)args[1].as.i32,
            argc == 2 ? (const void *)prestat : (const void *)".", argc == 2 ? 8 : 1, trap) != TURBOWASM_OK)
        error = TURBOWASM_WASI_ERRNO_FAULT;
    results[0] = (turbowasm_value){.kind=TURBOWASM_VALUE_I32, .as.i32=(int32_t)error};
    *count = 1; *trap = TURBOWASM_TRAP_NONE;
    cmeta_thread_yield();
    return TURBOWASM_OK;
}

static void stdio_wait_release(void) {
    atomic_store(&stdio_entered, 1);
    while (!atomic_load(&stdio_release)) cmeta_thread_yield();
}
static uint32_t stdio_write(uint32_t fd, const turbowasm_wasi_const_buffer *buffers,
    size_t count, uint32_t *written) {
    if (count != 1 || (fd != 1 && fd != 2 && fd != 50)) return TURBOWASM_WASI_ERRNO_IO;
    if (stdio_mode == STDIO_ERROR_RELEASE && atomic_exchange(&stdio_fail_remaining, 0))
        return TURBOWASM_WASI_ERRNO_IO;
    if (stdio_mode == STDIO_REOPEN && fd == 50 && !atomic_load(&stdio_entered)) stdio_wait_release();
    if ((stdio_mode == STDIO_PROGRESS || stdio_mode == STDIO_CLOSE_FLUSH || stdio_mode == STDIO_REOPEN) && fd == 2)
        atomic_store(&stdio_release, 1);
    size_t take = buffers[0].size < 2 ? buffers[0].size : 2;
    unsigned slot = fd == 2 ? 1 : 0;
    cmeta_mutex_lock(&stdio_mutex);
    if (take > sizeof(stdio_output[slot]) - stdio_size[slot]) {
        cmeta_mutex_unlock(&stdio_mutex); return TURBOWASM_WASI_ERRNO_NOMEM;
    }
    memcpy(stdio_output[slot] + stdio_size[slot], buffers[0].data, take);
    stdio_size[slot] += take;
    cmeta_mutex_unlock(&stdio_mutex);
    *written = (uint32_t)take;
    cmeta_thread_yield();
    return 0;
}
static uint32_t stdio_read(uint32_t fd, const turbowasm_wasi_buffer *buffers,
    size_t count, uint32_t *read) {
    if (fd != 0 || count != 1 || !buffers[0].size) return TURBOWASM_WASI_ERRNO_IO;
    if (stdio_mode == STDIO_PROGRESS) {
        stdio_wait_release();
        *(char *)buffers[0].data = 'Q'; *read = 1; return 0;
    }
    if (stdio_mode != STDIO_RECORDS) return TURBOWASM_WASI_ERRNO_IO;
    cmeta_mutex_lock(&stdio_mutex);
    size_t position = stdio_input_position;
    *read = position < 64 * 5 ? 1 : 0;
    if (*read) {
        unsigned value = (unsigned)(position / 5), column = (unsigned)(position % 5);
        char row[5] = {'R', (char)('0' + value / 100), (char)('0' + value / 10 % 10), (char)('0' + value % 10), '\n'};
        *(char *)buffers[0].data = row[column];
        ++stdio_input_position;
    }
    cmeta_mutex_unlock(&stdio_mutex);
    cmeta_thread_yield();
    return 0;
}

static turbowasm_status get_stdio_stage(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    (void)context; (void)call; (void)args;
    if (argc || !capacity || !results) return TURBOWASM_INVALID_ARGUMENT;
    results[0] = (turbowasm_value){.kind=TURBOWASM_VALUE_I32, .as.i32=(int32_t)atomic_load(&stdio_entered)};
    *count = 1; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static void check_stdio_records(unsigned slot, size_t payload_size) {
    size_t record_size = payload_size + 5;
    check_equal(stdio_size[slot], 64 * record_size);
    unsigned tags[4] = {0};
    for (size_t i = 0; i < stdio_size[slot]; i += record_size) {
        const char *record = stdio_output[slot] + i;
        unsigned tag = (unsigned)(record[0] - 'A');
        check_true(tag < 4);
        if (tag >= 4) return;
        ++tags[tag];
        check_equal(record[1], ':');
        for (size_t j = 0; j < payload_size; ++j) check_equal(record[j + 2], record[0]);
        check_equal(record[payload_size + 2], ':');
        check_equal(record[payload_size + 3], record[0]);
        check_equal(record[payload_size + 4], '\n');
    }
    for (unsigned i = 0; i < 4; ++i) check_equal(tags[i], 16u);
}

static turbowasm_status stdio_fdstat(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    if (!args || argc != 2 || !capacity || !results) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t error = TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (stdio_mode == STDIO_REOPEN && args[0].as.i32 == 50) {
        if (context) error = args[1].as.i32 == 1 ? 0 : TURBOWASM_WASI_ERRNO_INVAL;
        else {
            uint8_t stat[24] = {0}; stat[0] = 4; stat[8] = (uint8_t)TURBOWASM_WASI_RIGHT_FD_WRITE;
            error = turbowasm_host_call_memory_write64(call, 0, (uint32_t)args[1].as.i32, stat, sizeof stat, trap) == TURBOWASM_OK ?
                0 : TURBOWASM_WASI_ERRNO_FAULT;
        }
    }
    results[0] = (turbowasm_value){.kind=TURBOWASM_VALUE_I32, .as.i32=(int32_t)error};
    *count = 1; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

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
    if (stdio_mode) return stdio_write(fd, buffers, count, written);
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
    if (stdio_mode) return stdio_read(fd, buffers, count, read);
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
    (void)context; (void)call;
    if (!results || !capacity) return TURBOWASM_INVALID_ARGUMENT;
    results[0] = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = TURBOWASM_WASI_ERRNO_NOTCAPABLE};
    if ((stdio_mode == STDIO_CLOSE_FLUSH || stdio_mode == STDIO_REOPEN) && argc == 1 && args[0].as.i32 == 50) {
        if (stdio_mode == STDIO_CLOSE_FLUSH) stdio_wait_release();
        atomic_fetch_add(&stdio_closes, 1);
        results[0].as.i32 = 0;
    }
    *count = 1; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static uint32_t export_index(const char *symbol) {
    uint32_t index = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) index = e->item_index;
    }
    check_not_equal(index, UINT32_MAX);
    return index;
}
static void call_test(const char *symbol) {
    uint32_t index = export_index(symbol);
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
        stdio_mode = 0; stdio_size[0] = stdio_size[1] = stdio_input_position = 0;
        atomic_store(&stdio_entered, 0); atomic_store(&stdio_release, 0);
        atomic_store(&stdio_closes, 0); atomic_store(&stdio_fail_remaining, 1);
        preopen_error = 0; preopen_large = false; preopen_invalid_tag = false;
        atomic_store(&preopen_probes, 0); atomic_store(&preopen_names, 0);
        cmeta_mutex_init(&stdio_mutex);
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
        turbowasm_host_function_type stage_type = {NULL, 0, &i32, 1};
        turbowasm_host_function_type failure_type = {0};
        check_equal(turbowasm_linker_define_host_function(&linker, name("test"), name("thread_failure"),
            &failure_type, thread_failure, NULL), TURBOWASM_OK);
        turbowasm_value_kind preopen_args[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        turbowasm_host_function_type prestat_type = {preopen_args, 2, &i32, 1}, dirname_type = {preopen_args, 3, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_fdstat_get"), &prestat_type, stdio_fdstat, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_fdstat_set_flags"), &prestat_type, stdio_fdstat, &stdio_mode), TURBOWASM_OK);
        turbowasm_value_kind open_args[] = {i32,i32,i32,i32,i32,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64,i32,i32};
        turbowasm_host_function_type open_type = {open_args, 9, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("path_open"), &open_type, no_descriptor_operation, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_prestat_get"), &prestat_type, preopen_provider, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_prestat_dir_name"), &dirname_type, preopen_provider, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("test"), name("stdio_stage"), &stage_type, get_stdio_stage, NULL), TURBOWASM_OK);
        turbowasm_value_kind seek_args[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32};
        turbowasm_host_function_type seek_type = {seek_args, 4, &i32, 1};
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_close"), &close_type, no_descriptor_operation, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("wasi_snapshot_preview1"), name("fd_seek"), &seek_type, no_descriptor_operation, NULL), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&root, &module, &linker), TURBOWASM_OK);
        call_test("initialize");
    }
    after_each() {
        atomic_store(&stdio_release, 1);
        turbowasm_wasi_threads_proc_exit(&threads, &root, 0);
        while (turbowasm_wasi_threads_active(&threads)) cmeta_thread_yield();
        turbowasm_instance_destroy(&root);
        check_true(turbowasm_wasi_threads_destroy(&threads));
        turbowasm_wasi_preview1_destroy(&wasi);
        turbowasm_linker_destroy(&linker);
        turbowasm_instance_destroy(&memory);
        cmeta_mutex_destroy(&stdio_mutex);
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
    it("interrupts a real C11 join when child host failure skips the terminal epilogue") {
        turbowasm_execution_options options = {0};
        check_true(turbowasm_wasi_threads_execution_policy_apply(&policy, &options));
        turbowasm_value result = {0}; size_t count = 0; turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        check_equal(turbowasm_instance_invoke_with_options(&root, export_index("abnormal_join"),
            NULL, 0, &result, 1, &count, &trap, &options), TURBOWASM_INTERRUPTED);
        check_equal(count, (size_t)0); check_equal(trap, TURBOWASM_TRAP_NONE);
        turbowasm_status status = TURBOWASM_OK;
        check_true(turbowasm_wasi_threads_group_fatal(&threads, &status, &trap));
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(trap, TURBOWASM_TRAP_NONE);
    }
    it("serializes complete byte and wide records and input lines under contention") {
        stdio_mode = STDIO_RECORDS; call_test("stdio_records");
        check_stdio_records(0, 64); check_stdio_records(1, 16);
        check_equal(stdio_input_position, (size_t)320);
    }
    it("allows a separate stream to progress while an input provider is blocked") {
        stdio_mode = STDIO_PROGRESS; call_test("stdio_progress");
        check_equal(stdio_size[1], (size_t)7);
        check_equal(memcmp(stdio_output[1], "release", 7), 0);
    }
    it("retires a closing FILE only after a concurrent flush reference is released") {
        stdio_mode = STDIO_CLOSE_FLUSH; call_test("stdio_close_flush");
        check_equal(atomic_load(&stdio_closes), 1u);
        check_equal(stdio_size[0], (size_t)7);
        check_equal(memcmp(stdio_output[0], "pending", 7), 0);
    }
    it("releases stream ownership after provider error so another thread can recover") {
        stdio_mode = STDIO_ERROR_RELEASE; call_test("stdio_error_release");
        check_equal(stdio_size[0], (size_t)9);
        check_equal(memcmp(stdio_output[0], "recovered", 9), 0);
    }
    it("preserves recursive ownership and pending flush references across reopen") {
        stdio_mode = STDIO_REOPEN; call_test("stdio_reopen_flush");
        check_equal(atomic_load(&stdio_closes), 1u);
        check_equal(stdio_size[0], (size_t)15);
        check_equal(memcmp(stdio_output[0], "pendingreopened", 15), 0);
    }
    it("isolates implicit text and calendar state while preserving the shared random sequence") { call_test("libc_state"); }
    it("serializes bounded independent exit registries and permits reentrant registration") { call_test("libc_exit_callbacks"); }
    it("publishes signal handlers without holding a lock across a reentrant handler") { call_test("libc_signals"); }
    it("terminates default abort through proc_exit instead of recursively raising SIGABRT") {
        turbowasm_execution_options options = {.fuel=1000000, .has_fuel_limit=true};
        check_true(turbowasm_wasi_threads_execution_policy_apply(&policy, &options));
        turbowasm_value result = {0}; size_t count = 0; turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        check_equal(turbowasm_instance_invoke_with_options(&root, export_index("libc_abort_default"),
            NULL, 0, &result, 1, &count, &trap, &options), TURBOWASM_INTERRUPTED);
        uint32_t exit_code = 0;
        check_true(turbowasm_wasi_threads_group_exit_code(&threads, &exit_code));
        check_equal(exit_code, 134u);
    }
    it("publishes preopen discovery exactly once to concurrent callers") {
        call_test("libc_preopens");
        check_equal(atomic_load(&preopen_probes), 2u); check_equal(atomic_load(&preopen_names), 1u);
    }
    it("retains discovery failure without scanning indefinitely or returning partial results") {
        preopen_error = TURBOWASM_WASI_ERRNO_NOTCAPABLE; call_test("libc_preopen_denied");
        check_equal(atomic_load(&preopen_probes), 1u); check_equal(atomic_load(&preopen_names), 0u);
    }
    it("rejects unknown preopen types without continuing discovery") {
        preopen_invalid_tag = true; call_test("libc_preopen_invalid");
        check_equal(atomic_load(&preopen_probes), 1u); check_equal(atomic_load(&preopen_names), 0u);
    }
    it("rejects oversized preopen names before a truncated provider request") {
        preopen_large = true; call_test("libc_preopen_large");
        check_equal(atomic_load(&preopen_probes), 1u); check_equal(atomic_load(&preopen_names), 0u);
    }
    it("maps unknown provider discovery errors without indexing past the errno table") {
        preopen_error = UINT16_MAX; call_test("libc_preopen_invalid");
        check_equal(atomic_load(&preopen_probes), 1u); check_equal(atomic_load(&preopen_names), 0u);
    }
}
