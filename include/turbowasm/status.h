#ifndef TURBOWASM_STATUS_H
#define TURBOWASM_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbowasm_status {
    TURBOWASM_OK = 0,
    TURBOWASM_INVALID_ARGUMENT,
    TURBOWASM_MALFORMED_MODULE,
    TURBOWASM_UNSUPPORTED,
    TURBOWASM_OUT_OF_MEMORY,
    TURBOWASM_TYPE_MISMATCH,
    TURBOWASM_TRAPPED,
    TURBOWASM_FUEL_EXHAUSTED,
    TURBOWASM_INTERRUPTED
} turbowasm_status;

const char *turbowasm_status_string(turbowasm_status status);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_STATUS_H */
