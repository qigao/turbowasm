#include "http_host.h"
#include "reactor_session.h"
#include <salts/fs.h>
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>

enum { MODULE_LIMIT = 16 * 1024 * 1024, ALLOCATION_LIMIT = 64 * 1024 * 1024 };

/* Usage: http_example http.wasm tcp://127.0.0.1:8080 localhost /resource
 * These arguments are trusted host configuration. The guest sees endpoint 0.
 * This command demonstrates plain HTTP; TLS requires a host-owned trust profile.
 */
int main(int argc, char **argv) {
    if (argc != 5) return 1;
    uint8_t *program = NULL;
    salts_file_t file = SALTS_INVALID_FILE;
    salts_fs_stat_t file_info = {0};
    turbowasm_module module = {0};
    turbowasm_linker linker = {0};
    reactor_session session = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    app_http http = {0};
    app_http_endpoint endpoint = {.connection_uri = argv[2],
        .authority = argv[3], .target = argv[4]}; /* GET-only host policy. */
    chttp_client_config client_config = app_http_client_config();
    turbowasm_runtime_config config = {0};
    config.limits.max_module_bytes = MODULE_LIMIT;
    config.limits.max_allocation_bytes = ALLOCATION_LIMIT;
    config.limits.max_linear_memory_bytes = MODULE_LIMIT;
    config.limits.max_table_elements = 1024;
    int code = 1;
    /* Bound the read separately from Runtime allocations. Keep this trusted
     * input file immutable until loading has completed. */
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
    if (close_status || app_http_init(&http, &endpoint, 1, &client_config, 5000) != SALTS_OK ||
        turbowasm_module_load_borrowed_with_config(&module, program, length, &config) != TURBOWASM_OK ||
        turbowasm_linker_init_with_config(&linker, &config) != TURBOWASM_OK ||
        app_http_define(&linker, &http) != TURBOWASM_OK ||
        reactor_session_open(&session, &module, &linker, 1000000, &trap) != TURBOWASM_OK)
        goto done;
    turbowasm_linker_destroy(&linker);
    const char *exports[] = {"fetch", "response_length"};
    int32_t values[2] = {0};
    for (size_t i = 0; i < 2; ++i) {
        turbowasm_value result = {0};
        size_t count = 0;
        if (reactor_session_call(&session, exports[i], NULL, 0, &result, 1,
                &count, &trap) != TURBOWASM_OK || count != 1 ||
            result.kind != TURBOWASM_VALUE_I32) goto done;
        values[i] = result.as.i32;
    }
    /* Application output, not a diagnostic log. Body bytes remain in guest
     * memory for guest processing; length may exceed capacity on -2. */
    tstr output = tstr_format("HTTP result: {}, body length: {}\n", values[0], values[1]);
    if (!output) goto done;
    bool written = fwrite(output, 1, tstr_len(output), stdout) == tstr_len(output);
    tstr_free(output);
    if (!written || values[0] < 0) goto done;
    int32_t application_status = -1;
    if (reactor_session_close(&session, "http_close", &application_status, &trap) !=
            TURBOWASM_OK || application_status != 0) goto done;
    code = 0;
done:
    /* This executable exits on cleanup failure. An embedding application must
     * retain owners and retry draining instead of freeing live contexts. */
    if (!reactor_session_destroy(&session)) return 1;
    turbowasm_linker_destroy(&linker);
    if (app_http_destroy(&http, 5000) != SALTS_OK) return 1;
    turbowasm_module_destroy(&module);
    free(program);
    if (file != SALTS_INVALID_FILE && salts_fs_close(file)) code = 1;
    return code;
}
