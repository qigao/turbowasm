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
 */
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
     * excluding the trailing NUL. This bound is fixed at initialization.
     */
    size_t path_capacity;
} turbowasm_wasi_host_fs_config;

turbowasm_status turbowasm_wasi_host_fs_init(
    turbowasm_wasi_host_fs *adapter,
    const turbowasm_wasi_host_fs_config *config);

/*
 * Destroy succeeds only after the provider root and every child identity have
 * been closed explicitly through the provider contract.
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
