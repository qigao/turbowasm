#include "reactor_session.h"
#include <stdio.h>
#include <stdlib.h>

/* Run with the counter.wasm built alongside this example. A real embedder may
 * share the loaded module while creating independent sessions per client. */
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    uint8_t *bytes = NULL;
    turbowasm_module module = {0};
    reactor_session session = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    int code = 3;
    if (fseek(file, 0, SEEK_END)) goto done;
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) goto done;
    bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) goto done;
    turbowasm_runtime_config config = {0};
    config.limits.max_module_bytes = 16 * 1024 * 1024;
    config.limits.max_allocation_bytes = 64 * 1024 * 1024;
    config.limits.max_linear_memory_bytes = 16 * 1024 * 1024;
    config.limits.max_table_elements = 65536;
    if (turbowasm_module_load_borrowed_with_config(&module, bytes, (size_t)length, &config) != TURBOWASM_OK ||
        reactor_session_open(&session, &module, NULL, 1000000, &trap) != TURBOWASM_OK) goto done;
    for (int i = 1; i <= 3; ++i) {
        turbowasm_value arg = {.kind = TURBOWASM_VALUE_I32, .as.i32 = 7}, result = {0};
        size_t count = 0;
        if (reactor_session_call(&session, "counter_add", &arg, 1, &result, 1, &count, &trap) != TURBOWASM_OK ||
            count != 1 || result.kind != TURBOWASM_VALUE_I32 || result.as.i32 != 7 * i) goto done;
    }
    int32_t application_status = -1;
    if (reactor_session_close(&session, "counter_close", &application_status, &trap) != TURBOWASM_OK || application_status)
        goto done;
    code = 0;
done:
    (void)reactor_session_destroy(&session);
    turbowasm_module_destroy(&module);
    free(bytes);
    fclose(file);
    return code;
}
