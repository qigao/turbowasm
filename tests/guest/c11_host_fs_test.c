#include <tinytest.h>
#include <turbowasm/wasi_threads.h>
#include <turbowasm/wasi_host_fs.h>
#include <turbowasm/wasi_sockets.h>
#include <cmeta_fs.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <tstr.h>
#include <stdatomic.h>
#include <string.h>

static const uint8_t memory_bytes[] = {
    0,97,115,109,1,0,0,0, 5,5,1,3,16,0x80,2,
    7,10,1,6,'m','e','m','o','r','y',2,0
};
static turbowasm_module module, memory_module;
static turbowasm_instance root, memory;
static turbowasm_wasi_threads threads;
static turbowasm_wasi_threads_execution_policy policy;
static turbowasm_wasi_host_fs host_fs;
static turbowasm_wasi_fs fs;
static turbowasm_wasi_preview1 wasi;
static turbowasm_linker linker;
static atomic_uint random_calls;
static tstr directory;
static cmeta_fs_buf_t program;

static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)};
}
static uint32_t clock_time(void *context, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)context; (void)precision;
    if (id > 1) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = id ? cmeta_hrtime() : cmeta_realtime_ms() * UINT64_C(1000000);
    return 0;
}
/* Deterministic test provider: tests projection/concurrent invocation, not
 * entropy quality. HostFS itself is the production native provider. */
static uint32_t random_fill(void *context, uint8_t *bytes, size_t length) {
    (void)context;
    atomic_fetch_add(&random_calls, 1);
    memset(bytes, 0x6d, length);
    return 0;
}
static void call(const char *symbol) {
    uint32_t index = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) index = e->item_index;
    }
    check_not_equal(index, UINT32_MAX);
    turbowasm_execution_options options = {.fuel = 100000000, .has_fuel_limit = true};
    check(turbowasm_wasi_threads_execution_policy_apply(&policy, &options));
    turbowasm_value result = {0}; size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = turbowasm_instance_invoke_with_options(&root, index, NULL, 0,
        &result, 1, &count, &trap, &options);
    info("%s: status %d, trap %d, guest failure line %d", symbol, status, trap, result.as.i32);
    check_equal(status, TURBOWASM_OK); check_equal(trap, TURBOWASM_TRAP_NONE);
    check_equal(count, (size_t)1); check_equal(result.as.i32, 0);
}
spec("compiled Metallic C11 threads with native HostFS") {
    it("runs concurrent file creation, append, seek, stat, rename, removal and random projection") {
        directory = tstr_new_len(NULL, 2047);
        tstr temp = tstr_new_len(NULL, 2047);
        check(directory != NULL); check(temp != NULL);
        check_equal(cmeta_fs_get_tmpdir(temp, 2048), 0);
        check_equal(cmeta_fs_path_join(directory, 2048, temp, "turbowasm_c11_host_fs"), 0);
        tstr_free(temp);
        check_equal(cmeta_fs_mkdir(directory, 0755), 0);
        check_equal(cmeta_fs_read_file(GUEST_C11_HOST_FS_PATH, &program), 0);
        check_equal(turbowasm_module_load_borrowed(&module, (const uint8_t *)program.base, program.len), TURBOWASM_OK);
        check_equal(turbowasm_module_load_borrowed(&memory_module, memory_bytes, sizeof(memory_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&memory, &memory_module), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_init_pool(&threads, 4), TURBOWASM_OK);
        check(turbowasm_wasi_threads_execution_policy_init(&policy, &threads));
        turbowasm_wasi_host_fs_config host_config = {.host_root = directory, .file_capacity = 8, .path_capacity = 256};
        check_equal(turbowasm_wasi_host_fs_init(&host_fs, &host_config), TURBOWASM_OK);
        turbowasm_wasi_fs_config fs_config = {.descriptor_capacity = 9};
        turbowasm_wasi_fs_file root_file;
        check(turbowasm_wasi_host_fs_provider(&host_fs, &fs_config.provider, &root_file));
        check_equal(turbowasm_wasi_fs_init(&fs, &fs_config), TURBOWASM_OK);
        turbowasm_wasi_fs_descriptor descriptor;
        check_equal(turbowasm_wasi_fs_bind_descriptor(&fs, 3, root_file, true, ".", &descriptor), TURBOWASM_OK);
        turbowasm_wasi_preview1_config_v2 v2;
        turbowasm_wasi_preview1_config_v2_init(&v2);
        v2.base.filesystem = &fs; v2.base.allow_filesystem = true;
        v2.base.allow_fd_read = v2.base.allow_fd_write = true;
        v2.base.allow_clock = true; v2.base.clock_time = clock_time;
        v2.base.allow_random = true; v2.base.random_fill = random_fill;
        v2.base.allow_proc_exit = true; v2.base.proc_exit = turbowasm_wasi_threads_proc_exit;
        v2.base.proc_exit_context = &threads;
        check_equal(turbowasm_wasi_preview1_init_v2(&wasi, &v2), TURBOWASM_OK);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("env"), &memory), TURBOWASM_OK);
        check_equal(turbowasm_wasi_threads_define(&threads, &linker), TURBOWASM_OK);
        check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked(&root, &module, &linker), TURBOWASM_OK);
        call("initialize"); call("filesystem");
        while (turbowasm_wasi_threads_active(&threads)) cmeta_thread_yield();
        check(!turbowasm_wasi_threads_group_fatal(&threads, NULL, NULL));
        check_equal(atomic_load(&random_calls), 32u);
        /* Every opened guest file has been closed; only the preopen remains. */
        for (uint32_t fd = 4; fd < 12; ++fd) {
            turbowasm_wasi_fs_descriptor_info info;
            check(!turbowasm_wasi_fs_descriptor_info_get(&fs, fd, &info));
        }
        turbowasm_instance_destroy(&root);
        check(turbowasm_wasi_threads_destroy(&threads));
        check_equal(turbowasm_wasi_preview1_destroy_checked(&wasi), TURBOWASM_OK);
        turbowasm_linker_destroy(&linker); turbowasm_instance_destroy(&memory);
        check_equal(turbowasm_wasi_fs_close_fd(&fs, 3), 0u);
        check_equal(turbowasm_wasi_fs_destroy(&fs), TURBOWASM_OK);
        check_equal(turbowasm_wasi_host_fs_destroy(&host_fs), TURBOWASM_OK);
        turbowasm_module_destroy(&module); turbowasm_module_destroy(&memory_module);
        cmeta_fs_buf_free(&program);
        check_equal(cmeta_fs_rmdir(directory), 0); /* no leaked named files */
        tstr_free(directory);
    }
}
