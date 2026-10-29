#include "clock_host.h"
#include "reactor_session.h"
#include <salts/fs.h>
#include <salts/clock.h>
#include <stdlib.h>

enum { MODULE_LIMIT = 16 * 1024 * 1024, ALLOCATION_LIMIT = 64 * 1024 * 1024 };

static bool elapsed_ns(void *context, uint64_t *out) {
    const uint64_t *origin = context;
    uint64_t now = cmeta_hrtime();
    if (now < *origin) return false;
    *out = now - *origin;
    return true;
}

/* Supply the trusted clock.wasm built with this example. Each session can use
 * its own clock origin and its own capability scope, even with shared code.
 */
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    uint8_t *program = NULL;
    salts_file_t file = SALTS_INVALID_FILE;
    salts_fs_stat_t file_info = {0};
    turbowasm_module module = {0};
    turbowasm_linker linker = {0};
    reactor_session session = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint64_t origin = cmeta_hrtime();
    app_clock clock = {elapsed_ns, &origin};
    int code = 1;
    turbowasm_runtime_config config = {0};
    config.limits.max_module_bytes = MODULE_LIMIT;
    config.limits.max_allocation_bytes = ALLOCATION_LIMIT;
    config.limits.max_linear_memory_bytes = MODULE_LIMIT;
    config.limits.max_table_elements = 1024;
    /* Bound host allocation before reading. The host keeps this input file
     * immutable while loading; guest memory limits cannot bound a file read. */
    if (salts_fs_stat(argv[1], &file_info) || !file_info.is_file ||
        !file_info.size || file_info.size > MODULE_LIMIT) goto done;
    size_t length = (size_t)file_info.size;
    program = malloc(length);
    if (!program) goto done;
    file = salts_fs_open(argv[1], SALTS_FS_O_RDONLY, 0);
    if (file == SALTS_INVALID_FILE) goto done;
    for (size_t offset = 0; offset < length;) {
        int count = salts_fs_read(file, (char *)program + offset, length - offset);
        if (count <= 0) goto done;
        offset += (size_t)count;
    }
    int close_status = salts_fs_close(file);
    file = SALTS_INVALID_FILE;
    if (close_status || turbowasm_module_load_borrowed_with_config(&module,
            program, length, &config) != TURBOWASM_OK ||
        turbowasm_linker_init_with_config(&linker, &config) != TURBOWASM_OK ||
        app_clock_define(&linker, &clock) != TURBOWASM_OK ||
        reactor_session_open(&session, &module, &linker, 1000000, &trap) != TURBOWASM_OK)
        goto done;
    turbowasm_linker_destroy(&linker);

    const char *exports[] = {"clock_initial", "clock_sample"};
    int64_t previous = 0;
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); ++i) {
        turbowasm_value result = {0};
        size_t count = 0;
        if (reactor_session_call(&session, exports[i], NULL, 0,
                &result, 1, &count, &trap) != TURBOWASM_OK || count != 1 ||
            result.kind != TURBOWASM_VALUE_I64 || result.as.i64 < previous)
            goto done;
        previous = result.as.i64;
    }
    int32_t application_status = -1;
    if (reactor_session_close(&session, "clock_close", &application_status, &trap) !=
            TURBOWASM_OK || application_status != 0) goto done;
    code = 0;
done:
    (void)reactor_session_destroy(&session);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    free(program);
    if (file != SALTS_INVALID_FILE && salts_fs_close(file)) code = 1;
    return code;
}
