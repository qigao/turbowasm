#ifndef TURBOWASM_WASI_HOST_FS_H
#define TURBOWASM_WASI_HOST_FS_H

#include <turbowasm/wasi_fs.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional host-filesystem implementation of turbowasm_wasi_fs_provider.
 *
 * The adapter depends only on the Salts secure root-relative filesystem
 * capability. Native fd/HANDLE values never enter TurboWasm public ABI.
 *
 * Salts root file/directory close consumes its opaque identity even if the
 * underlying native close reports an error. WASIHostFS therefore treats close
 * as ownership-consuming and reports success once that close has been
 * attempted, so the generic descriptor table never retains a dangling Salts
 * identity.
 *
 * Concurrent provider calls pin native identities. Per-identity locks serialize
 * file position, whole vector I/O, append flags and directory cursors; different
 * files progress independently. Close returns BUSY while calls are in flight,
 * preserving ownership for retry after those calls finish. Rename returns BUSY
 * while a child directory is live or an open/rename transaction is in flight;
 * opens racing an admitted rename also return BUSY before native effects.
 * Init/destroy require exclusive caller lifecycle ownership and no new entrants.
 */
enum {
    TURBOWASM_WASI_HOST_FS_PATH_MAX = 4096
};

typedef struct turbowasm_wasi_host_fs {
    void *impl;
} turbowasm_wasi_host_fs;

typedef struct turbowasm_wasi_host_fs_config {
    /* Absolute host directory admitted as the provider preopen root. */
    const char *host_root;

    /* Maximum number of simultaneously live child file/directory identities. */
    size_t file_capacity;

    /*
     * Maximum root-relative path bytes retained per directory identity,
     * excluding the trailing NUL. Must not exceed
     * TURBOWASM_WASI_HOST_FS_PATH_MAX and is fixed at initialization.
     */
    size_t path_capacity;
} turbowasm_wasi_host_fs_config;

turbowasm_status turbowasm_wasi_host_fs_init(
    turbowasm_wasi_host_fs *adapter,
    const turbowasm_wasi_host_fs_config *config);

/*
 * Destroy succeeds only after the provider root and every child identity have
 * been closed explicitly through the provider contract and all calls drained.
 */
turbowasm_status turbowasm_wasi_host_fs_destroy(
    turbowasm_wasi_host_fs *adapter);

bool turbowasm_wasi_host_fs_provider(
    turbowasm_wasi_host_fs *adapter,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_HOST_FS_H */
