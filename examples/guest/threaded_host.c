#include "threaded_session.h"
#include <turbowasm/wasi.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t clock_time(void *context, uint32_t clock, uint64_t precision, uint64_t *out) {
    (void)context; (void)precision;
    if (clock > 1) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = clock ? cmeta_hrtime() : cmeta_realtime_ms() * UINT64_C(1000000);
    return 0;
}

/* Run with threaded_counter.wasm. This example has no socket provider: root
 * calls can run on this thread while the owned pool runs its persistent child.
 * An owner-dispatched socket embedding must drive owner progress separately. */
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    static const uint8_t memory_bytes[] = {
        0,97,115,109,1,0,0,0, 5,5,1,3,16,0x80,2,
        7,10,1,6,'m','e','m','o','r','y',2,0
    };
    turbowasm_module module = {0}, memory_module = {0};
    turbowasm_instance memory = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_preview1 wasi = {0};
    threaded_session session = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint8_t *bytes = NULL;
    int code = 3;
    if (fseek(file, 0, SEEK_END)) goto done;
    long size = ftell(file);
    if (size <= 0 || size > 4 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) goto done;
    bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) goto done;
    turbowasm_runtime_config runtime = {0};
    runtime.limits.max_module_bytes = 4 * 1024 * 1024;
    runtime.limits.max_allocation_bytes = 64 * 1024 * 1024;
    runtime.limits.max_linear_memory_bytes = 16 * 1024 * 1024;
    runtime.limits.max_table_elements = 65536;
    if (turbowasm_module_load_borrowed_with_config(&module, bytes, (size_t)size, &runtime) != TURBOWASM_OK ||
        turbowasm_module_load_borrowed(&memory_module, memory_bytes, sizeof(memory_bytes)) != TURBOWASM_OK ||
        turbowasm_instance_create(&memory, &memory_module) != TURBOWASM_OK ||
        turbowasm_wasi_threads_init_pool(&threads, 1) != TURBOWASM_OK ||
        turbowasm_linker_init(&linker) != TURBOWASM_OK ||
        turbowasm_linker_define_instance(&linker, (turbowasm_name){(const uint8_t *)"env", 3}, &memory) != TURBOWASM_OK ||
        turbowasm_wasi_threads_define(&threads, &linker) != TURBOWASM_OK) goto done;
    turbowasm_wasi_preview1_config config = {0};
    config.allow_clock = true; config.clock_time = clock_time;
    config.allow_proc_exit = true; config.proc_exit = turbowasm_wasi_threads_proc_exit;
    config.proc_exit_context = &threads;
    turbowasm_execution_options options = {.fuel = 1000000, .has_fuel_limit = true};
    if (turbowasm_wasi_preview1_init(&wasi, &config) != TURBOWASM_OK ||
        turbowasm_wasi_preview1_define(&wasi, &linker) != TURBOWASM_OK ||
        threaded_session_open(&session, &module, &linker, &threads, false, &options, &trap) != TURBOWASM_OK)
        goto done;
    for (int i = 1; i <= 3; ++i) {
        turbowasm_value argument = {.kind = TURBOWASM_VALUE_I32, .as.i32 = 7}, result;
        size_t count = 0;
        if (threaded_session_call(&session, "counter_add", &argument, 1, &result, 1, &count, &trap) != TURBOWASM_OK ||
            count != 1 || result.as.i32 != 7 * i) goto done;
    }
    int32_t closed = -1;
    if (threaded_session_close(&session, "counter_close", NULL, 0, &closed, &trap) != TURBOWASM_OK || closed)
        goto done;
    code = 0;
done:
    /* Even failed startup may have admitted a child. Request exit, then keep
     * providers/module storage alive until all child tasks actually terminate. */
    turbowasm_wasi_threads_proc_exit(&threads, &session.instance, (uint32_t)code);
    while (turbowasm_wasi_threads_active(&threads)) cmeta_thread_yield();
    if (!threaded_session_destroy(&session) || !turbowasm_wasi_threads_destroy(&threads)) return 4;
    turbowasm_wasi_preview1_destroy(&wasi); turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&memory); turbowasm_module_destroy(&memory_module);
    turbowasm_module_destroy(&module); free(bytes); fclose(file);
    return code;
}
