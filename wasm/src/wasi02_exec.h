#ifndef TURBOWASM_WASI02_EXEC_H
#define TURBOWASM_WASI02_EXEC_H

#include "wasi02_component.h"
#include "wasi02_filesystem.h"
#include "wasi02_poll.h"
#include "wasi02_streams.h"
#include "wasi02_sockets.h"

typedef struct turbowasm_wasi02_exec_capabilities {
    turbowasm_wasi02_provider *provider;
    turbowasm_wasi02_filesystem *filesystem;
    turbowasm_wasi02_poll *poll;
    turbowasm_wasi02_streams *streams;
    turbowasm_wasi02_sockets *sockets;
} turbowasm_wasi02_exec_capabilities;

/*
 * Internal aggregate Component-import initializer.
 *
 * Capability contexts are borrowed and must outlive the executable.
 * NULL capability pointers simply omit that import set.
 */
turbowasm_status turbowasm_wasi02_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_wasi02_exec_capabilities *capabilities);

/* Shared descriptor assembly for sync and async retained instance creation. */
turbowasm_status turbowasm_wasi02_exec_import_sets(
    const turbowasm_wasi02_exec_capabilities *capabilities,
    turbowasm_component_exec_imports *out_sets, size_t capacity, size_t *out_count);

#endif /* TURBOWASM_WASI02_EXEC_H */
