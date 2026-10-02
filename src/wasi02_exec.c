#include "wasi02_exec.h"

#include <string.h>

#ifndef TURBOWASM_WASI02_HAS_FILESYSTEM
#define TURBOWASM_WASI02_HAS_FILESYSTEM 1
#endif

turbowasm_status turbowasm_wasi02_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_wasi02_exec_capabilities *capabilities) {
    turbowasm_component_exec_imports sets[5];
    size_t count = 0u;
    turbowasm_status status;

    if (exec == NULL || binary == NULL ||
        capabilities == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(sets, 0, sizeof(sets));

    if (capabilities->provider != NULL) {
        status = turbowasm_wasi02_component_imports(
            capabilities->provider, &sets[count]);
        if (status != TURBOWASM_OK)
            return status;
        ++count;
    }

    if (capabilities->filesystem != NULL) {
#if TURBOWASM_WASI02_HAS_FILESYSTEM
        status = turbowasm_wasi02_filesystem_imports(
            capabilities->filesystem, &sets[count]);
        if (status != TURBOWASM_OK)
            return status;
        ++count;
#else
        return TURBOWASM_UNSUPPORTED;
#endif
    }

    if (capabilities->poll != NULL) {
        status = turbowasm_wasi02_poll_imports(
            capabilities->poll, &sets[count]);
        if (status != TURBOWASM_OK)
            return status;
        ++count;
    }

    if (capabilities->streams != NULL) {
        if (capabilities->poll == NULL ||
            capabilities->streams->poll == NULL ||
            capabilities->streams->poll !=
                capabilities->poll)
            return TURBOWASM_INVALID_ARGUMENT;

        status = turbowasm_wasi02_streams_imports(
            capabilities->streams, &sets[count]);
        if (status != TURBOWASM_OK)
            return status;
        ++count;
    }

    if (capabilities->sockets != NULL) {
        if (capabilities->streams == NULL ||
            capabilities->poll == NULL ||
            capabilities->sockets->streams !=
                capabilities->streams ||
            capabilities->sockets->poll !=
                capabilities->poll)
            return TURBOWASM_INVALID_ARGUMENT;

        status = turbowasm_wasi02_sockets_imports(
            capabilities->sockets, &sets[count]);
        if (status != TURBOWASM_OK)
            return status;
        ++count;
    }

    if (count == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_component_exec_init_with_import_sets(
        exec, binary, sets, count);
}
