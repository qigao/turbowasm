#ifndef TURBOWASM_WASI_FS_INTERNAL_H
#define TURBOWASM_WASI_FS_INTERNAL_H

#include <turbowasm/wasi_fs.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * Internal capability inspection for typed WASI adapters.
 * These helpers never expose provider/native handles and are not installed.
 */
size_t turbowasm_wasi_fs_internal_preopen_count(
    const turbowasm_wasi_fs *filesystem);

bool turbowasm_wasi_fs_internal_preopen_at(
    const turbowasm_wasi_fs *filesystem,
    size_t index,
    turbowasm_wasi_fs_descriptor_info *out_info);

bool turbowasm_wasi_fs_internal_descriptor_info_get(
    const turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_descriptor descriptor,
    turbowasm_wasi_fs_descriptor_info *out_info);

#endif /* TURBOWASM_WASI_FS_INTERNAL_H */
