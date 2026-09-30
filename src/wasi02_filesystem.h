#ifndef TURBOWASM_WASI02_FILESYSTEM_H
#define TURBOWASM_WASI02_FILESYSTEM_H

#include "component_resource.h"
#include "component_exec.h"
#include "wasi02_provider.h"

#include <turbowasm/wasi_fs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * W3 filesystem resource projection.
 *
 * Preopen resources are guest-owned logical handles backed by host-owned
 * preopen capabilities in turbowasm_wasi_fs. Dropping the WIT resource
 * consumes only the logical handle; it does not close the host preopen.
 *
 * Child descriptors created by later W3 slices may use the same resource
 * table with owning close semantics.
 */
typedef struct turbowasm_wasi02_filesystem {
    turbowasm_wasi_fs *filesystem;
    turbowasm_component_resource_table resources;
    uint64_t descriptor_identity;
    bool descriptor_identity_bound;
    bool initialized;
} turbowasm_wasi02_filesystem;

turbowasm_status turbowasm_wasi02_filesystem_init(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi_fs *backing_filesystem,
    uint32_t max_resources);

/*
 * Destroy is ownership-strict: all WIT descriptor handles must have been
 * dropped first. The backing turbowasm_wasi_fs remains caller-owned.
 */
turbowasm_status turbowasm_wasi02_filesystem_destroy(
    turbowasm_wasi02_filesystem *filesystem);

/*
 * Implements wasi:filesystem/preopens@0.2.8.get-directories.
 * The returned W2 value owns its list/tuple/string storage, while each
 * descriptor resource handle remains live in the bridge until explicitly
 * dropped with turbowasm_wasi02_filesystem_descriptor_drop().
 */
turbowasm_status turbowasm_wasi02_filesystem_get_directories(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi02_value *out_result);

/*
 * Resolve one WIT descriptor resource back to the retained, generation-safe
 * filesystem descriptor. This revalidates slot+generation on every call.
 */
turbowasm_status turbowasm_wasi02_filesystem_descriptor_resolve(
    const turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource,
    turbowasm_wasi_fs_descriptor_info *out_info);

/* Consume one WIT logical descriptor resource. */
turbowasm_status turbowasm_wasi02_filesystem_descriptor_drop(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource);

/*
 * W3c synchronous descriptor operations that map losslessly onto the existing
 * filesystem provider contract. Results use WIT-shaped result values.
 */
turbowasm_status turbowasm_wasi02_filesystem_create_directory_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result);

turbowasm_status turbowasm_wasi02_filesystem_remove_directory_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result);

turbowasm_status turbowasm_wasi02_filesystem_unlink_file_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result);

turbowasm_status turbowasm_wasi02_filesystem_open_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    uint32_t path_flags,
    turbowasm_wasi02_string_view path,
    uint32_t open_flags,
    uint32_t descriptor_flags,
    turbowasm_wasi02_value *out_result);

/*
 * Instantiate the real W3 preopens import path through Component canonical ABI.
 * The bridge is borrowed and must outlive the executable.
 */
turbowasm_status turbowasm_wasi02_filesystem_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_filesystem *filesystem);

#endif /* TURBOWASM_WASI02_FILESYSTEM_H */
