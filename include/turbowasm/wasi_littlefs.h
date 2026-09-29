#ifndef TURBOWASM_WASI_LITTLEFS_H
#define TURBOWASM_WASI_LITTLEFS_H

#include <turbowasm/wasi_fs.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURBOWASM_WASI_LITTLEFS_PATH_MAX = 512
};

/*
 * Optional littlefs-backed implementation of turbowasm_wasi_fs_provider.
 *
 * littlefs is intentionally kept out of TurboWasm::Runtime and
 * TurboWasm::WASI. The filesystem pointer must reference a mounted lfs_t
 * owned by the caller and must outlive this adapter.
 */
typedef struct turbowasm_wasi_littlefs {
    void *impl;
} turbowasm_wasi_littlefs;

typedef struct turbowasm_wasi_littlefs_config {
    void *filesystem;
    size_t file_capacity;
    const uint8_t *root_path;
    size_t root_path_length;
} turbowasm_wasi_littlefs_config;

/*
 * Initialization allocates a fixed descriptor table plus one littlefs file
 * cache per slot. No adapter allocation occurs in provider hot operations.
 */
turbowasm_status turbowasm_wasi_littlefs_init(
    turbowasm_wasi_littlefs *adapter,
    const turbowasm_wasi_littlefs_config *config);

/*
 * Destroy succeeds only after every provider identity, including the root
 * identity, has been closed explicitly.
 */
turbowasm_status turbowasm_wasi_littlefs_destroy(
    turbowasm_wasi_littlefs *adapter);

bool turbowasm_wasi_littlefs_provider(
    turbowasm_wasi_littlefs *adapter,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_LITTLEFS_H */
