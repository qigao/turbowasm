#include <turbowasm/status.h>

const char *turbowasm_status_string(turbowasm_status status) {
    switch (status) {
        case TURBOWASM_OK: return "ok";
        case TURBOWASM_INVALID_ARGUMENT: return "invalid_argument";
        case TURBOWASM_MALFORMED_MODULE: return "malformed_module";
        case TURBOWASM_UNSUPPORTED: return "unsupported";
        case TURBOWASM_OUT_OF_MEMORY: return "out_of_memory";
        case TURBOWASM_TYPE_MISMATCH: return "type_mismatch";
        default: return "unknown";
    }
}
