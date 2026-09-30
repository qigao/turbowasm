#ifndef TURBOWASM_WASI02_FILESYSTEM_H
#define TURBOWASM_WASI02_FILESYSTEM_H

#include "component_resource.h"
#include "wasi02_provider.h"

#include <turbowasm/wasi_fs.h>

#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_wasi02_filesystem {
    turbowasm_wasi_fs *filesystem;
    turbowasm_component_resource_table leases;
    bool initialized;
} turbowasm_wasi02_filesystem;

turbowasm_status turbowasm_wasi02_filesystem_init(
    turbowasm_wasi02_filesystem *adapter,
    turbowasm_wasi_fs *filesystem,
    uint32_t max_resource_leases);

turbowasm_status turbowasm_wasi02_filesystem_destroy(
    turbowasm_wasi02_filesystem *adapter);

/*
 * Produce the exact preopens WIT value:
 *   list<tuple<descriptor, string>>
 *
 * Resource handles are independent logical leases. Callers must eventually
 * drop each descriptor handle with turbowasm_wasi02_filesystem_drop().
 */
turbowasm_status turbowasm_wasi02_filesystem_get_directories(
    turbowasm_wasi02_filesystem *adapter,
    turbowasm_wasi02_value *out);

/* Resolve one logical Component descriptor lease back to its retained,
 * generation-checked filesystem capability.
 */
turbowasm_status turbowasm_wasi02_filesystem_resolve(
    const turbowasm_wasi02_filesystem *adapter,
    uint32_t resource_handle,
    turbowasm_wasi_fs_descriptor_info *out_info);

/* Drop a logical preopen lease. The backing preopen remains filesystem-owned. */
turbowasm_status turbowasm_wasi02_filesystem_drop(
    turbowasm_wasi02_filesystem *adapter,
    uint32_t resource_handle);

#endif /* TURBOWASM_WASI02_FILESYSTEM_H */
