#include "wasi02_filesystem.h"

#include "runtime_alloc.h"
#include "wasi_fs_internal.h"

#include <string.h>

enum {
    /*
     * Local nominal identity inside this dedicated lease table. The semantic
     * WIT identity remains in wasi02_descriptor.c; this table contains only
     * wasi:filesystem/types@0.2.8#descriptor resources.
     */
    TW_WASI02_FS_DESCRIPTOR_RESOURCE_ID = 1u
};

static turbowasm_value descriptor_rep(
    turbowasm_wasi_fs_descriptor descriptor) {
    turbowasm_value rep;
    uint64_t bits =
        ((uint64_t)descriptor.generation << 32u) |
        (uint64_t)descriptor.slot;

    memset(&rep, 0, sizeof(rep));
    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = (int64_t)bits;
    return rep;
}

static bool descriptor_from_rep(
    turbowasm_value rep,
    turbowasm_wasi_fs_descriptor *out) {
    uint64_t bits;

    if (out == NULL || rep.kind != TURBOWASM_VALUE_I64)
        return false;
    bits = (uint64_t)rep.as.i64;
    out->slot = (uint32_t)bits;
    out->generation = (uint32_t)(bits >> 32u);
    return out->generation != 0u;
}

static turbowasm_status lease_drop_destructor(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    turbowasm_wasi02_filesystem *adapter =
        (turbowasm_wasi02_filesystem *)context;
    turbowasm_wasi_fs_descriptor descriptor;

    /*
     * Preopen leases deliberately do not consume the backing descriptor.
     * Decode the rep only as an integrity check; backing lifetime belongs to
     * turbowasm_wasi_fs and may outlive every Component lease.
     */
    if (adapter == NULL || !adapter->initialized ||
        resource_identity !=
            TW_WASI02_FS_DESCRIPTOR_RESOURCE_ID ||
        !descriptor_from_rep(rep, &descriptor))
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_init(
    turbowasm_wasi02_filesystem *adapter,
    turbowasm_wasi_fs *filesystem,
    uint32_t max_resource_leases) {
    if (adapter == NULL || adapter->initialized ||
        adapter->leases.entries != NULL ||
        adapter->leases.capacity != 0u ||
        adapter->leases.live_count != 0u ||
        adapter->leases.max_entries != 0u ||
        filesystem == NULL || filesystem->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_component_resource_table_init(
            &adapter->leases, max_resource_leases))
        return TURBOWASM_INVALID_ARGUMENT;

    adapter->filesystem = filesystem;
    adapter->initialized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_destroy(
    turbowasm_wasi02_filesystem *adapter) {
    if (adapter == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!adapter->initialized)
        return TURBOWASM_OK;
    if (adapter->leases.live_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_component_resource_table_destroy(
        &adapter->leases);
    adapter->filesystem = NULL;
    adapter->initialized = false;
    return TURBOWASM_OK;
}

static void rollback_directory_value(
    turbowasm_wasi02_filesystem *adapter,
    turbowasm_wasi02_value *out,
    size_t completed) {
    size_t i;

    if (out == NULL ||
        out->kind != TURBOWASM_WASI02_VALUE_LIST)
        return;

    for (i = 0u; i < completed; ++i) {
        turbowasm_wasi02_value *tuple =
            &out->as.list.items[i];
        if (tuple->kind == TURBOWASM_WASI02_VALUE_TUPLE &&
            tuple->as.tuple.count == 2u &&
            tuple->as.tuple.items != NULL &&
            tuple->as.tuple.items[0].kind ==
                TURBOWASM_WASI02_VALUE_RESOURCE) {
            (void)turbowasm_wasi02_filesystem_drop(
                adapter,
                tuple->as.tuple.items[0].as.resource);
        }
    }
    turbowasm_wasi02_value_destroy(out);
}

turbowasm_status turbowasm_wasi02_filesystem_get_directories(
    turbowasm_wasi02_filesystem *adapter,
    turbowasm_wasi02_value *out) {
    size_t count;
    size_t i;

    if (adapter == NULL || !adapter->initialized ||
        adapter->filesystem == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    count = turbowasm_wasi_fs_internal_preopen_count(
        adapter->filesystem);
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_LIST;

    if (count != 0u) {
        if (count > SIZE_MAX / sizeof(*out->as.list.items))
            return TURBOWASM_OUT_OF_MEMORY;
        out->as.list.items =
            (turbowasm_wasi02_value *)turbowasm_rt_calloc(
                count, sizeof(*out->as.list.items));
        if (out->as.list.items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }
    out->as.list.count = count;

    for (i = 0u; i < count; ++i) {
        turbowasm_wasi_fs_descriptor_info info;
        turbowasm_wasi02_value *tuple =
            &out->as.list.items[i];
        turbowasm_component_resource_handle handle;
        turbowasm_value rep;
        size_t path_size;
        turbowasm_status status;

        if (!turbowasm_wasi_fs_internal_preopen_at(
                adapter->filesystem, i, &info) ||
            !info.preopen || info.guest_path == NULL) {
            rollback_directory_value(adapter, out, i);
            return TURBOWASM_TRAPPED;
        }

        rep = descriptor_rep(info.descriptor);
        status = turbowasm_component_resource_new_owned(
            &adapter->leases,
            TW_WASI02_FS_DESCRIPTOR_RESOURCE_ID,
            rep,
            &handle);
        if (status != TURBOWASM_OK) {
            rollback_directory_value(adapter, out, i);
            return status;
        }

        tuple->kind = TURBOWASM_WASI02_VALUE_TUPLE;
        tuple->as.tuple.items =
            (turbowasm_wasi02_value *)turbowasm_rt_calloc(
                2u, sizeof(*tuple->as.tuple.items));
        if (tuple->as.tuple.items == NULL) {
            (void)turbowasm_wasi02_filesystem_drop(
                adapter, handle);
            rollback_directory_value(adapter, out, i);
            return TURBOWASM_OUT_OF_MEMORY;
        }
        tuple->as.tuple.count = 2u;
        tuple->as.tuple.items[0].kind =
            TURBOWASM_WASI02_VALUE_RESOURCE;
        tuple->as.tuple.items[0].as.resource = handle;

        path_size = strlen(info.guest_path);
        tuple->as.tuple.items[1].kind =
            TURBOWASM_WASI02_VALUE_STRING;
        if (path_size != 0u) {
            tuple->as.tuple.items[1].as.string.data =
                (uint8_t *)turbowasm_rt_malloc(path_size);
            if (tuple->as.tuple.items[1].as.string.data == NULL) {
                (void)turbowasm_wasi02_filesystem_drop(
                    adapter, handle);
                rollback_directory_value(adapter, out, i);
                return TURBOWASM_OUT_OF_MEMORY;
            }
            memcpy(
                tuple->as.tuple.items[1].as.string.data,
                info.guest_path,
                path_size);
        }
        tuple->as.tuple.items[1].as.string.size = path_size;
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_resolve(
    const turbowasm_wasi02_filesystem *adapter,
    uint32_t resource_handle,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    turbowasm_value rep;
    turbowasm_wasi_fs_descriptor descriptor;
    turbowasm_status status;

    if (adapter == NULL || !adapter->initialized ||
        adapter->filesystem == NULL || out_info == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        &adapter->leases,
        resource_handle,
        TW_WASI02_FS_DESCRIPTOR_RESOURCE_ID,
        &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (!descriptor_from_rep(rep, &descriptor))
        return TURBOWASM_TRAPPED;
    if (!turbowasm_wasi_fs_internal_descriptor_info_get(
            adapter->filesystem, descriptor, out_info))
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_drop(
    turbowasm_wasi02_filesystem *adapter,
    uint32_t resource_handle) {
    if (adapter == NULL || !adapter->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_component_resource_drop(
        &adapter->leases,
        resource_handle,
        TW_WASI02_FS_DESCRIPTOR_RESOURCE_ID,
        lease_drop_destructor,
        adapter);
}
