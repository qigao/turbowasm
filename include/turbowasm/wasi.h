#ifndef TURBOWASM_WASI_H
#define TURBOWASM_WASI_H

#include <turbowasm/link.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURBOWASM_WASI_ERRNO_SUCCESS = 0,
    TURBOWASM_WASI_ERRNO_FAULT = 21
};

typedef struct turbowasm_wasi_preview1 {
    void *impl;
} turbowasm_wasi_preview1;

typedef struct turbowasm_wasi_preview1_config {
    bool allow_args;
    const char *const *args;
    size_t arg_count;

    bool allow_environ;
    const char *const *environ;
    size_t environ_count;
} turbowasm_wasi_preview1_config;

/*
 * Initialize one Preview1 capability object.
 *
 * argv/environ strings are copied and owned by the object. Each environment
 * entry is passed to the guest verbatim (normally "KEY=VALUE").
 */
turbowasm_status turbowasm_wasi_preview1_init(
    turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config *config);

void turbowasm_wasi_preview1_destroy(
    turbowasm_wasi_preview1 *wasi);

/*
 * Register enabled capabilities in the canonical
 * "wasi_snapshot_preview1" linker namespace.
 *
 * The WASI object must outlive consumer instances whose host bindings refer to
 * it. The linker itself retains its normal destroy-after-instantiation rule.
 */
turbowasm_status turbowasm_wasi_preview1_define(
    turbowasm_wasi_preview1 *wasi,
    turbowasm_linker *linker);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_H */
