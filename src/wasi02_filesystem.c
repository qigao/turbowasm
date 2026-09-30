#include "wasi02_filesystem.h"

#include "runtime_alloc.h"

#include <string.h>

#define TURBOWASM_WASI02_FS_DESCRIPTOR_ID \
    UINT64_C(0x7761736930326673)

static uint64_t pack_descriptor(
    turbowasm_wasi_fs_descriptor descriptor) {
    return ((uint64_t)descriptor.generation << 32u) |
           (uint64_t)descriptor.slot;
}

static turbowasm_wasi_fs_descriptor unpack_descriptor(
    uint64_t packed) {
    turbowasm_wasi_fs_descriptor descriptor;
    descriptor.slot = (uint32_t)packed;
    descriptor.generation = (uint32_t)(packed >> 32u);
    return descriptor;
}

static turbowasm_status resource_descriptor_get(
    const turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource,
    turbowasm_wasi_fs_descriptor *out_descriptor) {
    turbowasm_value rep = {0};
    turbowasm_status status;

    if (filesystem == NULL || !filesystem->initialized ||
        out_descriptor == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
        &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (rep.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_TRAPPED;

    *out_descriptor =
        unpack_descriptor((uint64_t)rep.as.i64);
    if (out_descriptor->generation == 0u)
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

static void rollback_resource(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource) {
    if (resource != 0u) {
        (void)turbowasm_component_resource_drop(
            &filesystem->resources,
            resource,
            TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
            NULL,
            NULL);
    }
}

static turbowasm_status copy_path(
    const char *path,
    turbowasm_wasi02_value *out) {
    size_t size;
    uint8_t *copy = NULL;

    if (path == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    size = strlen(path);
    if (size != 0u) {
        copy = (uint8_t *)turbowasm_rt_malloc(size);
        if (copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(copy, path, size);
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_STRING;
    out->as.string.data = copy;
    out->as.string.size = size;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_init(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi_fs *backing_filesystem,
    uint32_t max_resources) {
    if (filesystem == NULL || filesystem->initialized ||
        backing_filesystem == NULL ||
        backing_filesystem->impl == NULL ||
        max_resources == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(filesystem, 0, sizeof(*filesystem));
    if (!turbowasm_component_resource_table_init(
            &filesystem->resources, max_resources))
        return TURBOWASM_OUT_OF_MEMORY;

    filesystem->filesystem = backing_filesystem;
    filesystem->initialized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_destroy(
    turbowasm_wasi02_filesystem *filesystem) {
    if (filesystem == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!filesystem->initialized)
        return TURBOWASM_OK;
    if (filesystem->resources.live_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_component_resource_table_destroy(
        &filesystem->resources);
    memset(filesystem, 0, sizeof(*filesystem));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_get_directories(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi02_value *out_result) {
    size_t count;
    turbowasm_wasi02_value *items = NULL;
    size_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (filesystem == NULL || !filesystem->initialized ||
        filesystem->filesystem == NULL || out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_result, 0, sizeof(*out_result));
    count = turbowasm_wasi_fs_preopen_count(
        filesystem->filesystem);

    if (count != 0u) {
        if (count > SIZE_MAX / sizeof(*items))
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
            count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < count; ++i) {
        turbowasm_wasi_fs_descriptor_info info = {0};
        turbowasm_wasi02_value *pair_items;
        turbowasm_value rep = {0};
        uint32_t resource = 0u;

        if (!turbowasm_wasi_fs_preopen_at(
                filesystem->filesystem, i, &info) ||
            !info.preopen || info.guest_path == NULL) {
            status = TURBOWASM_TRAPPED;
            goto fail;
        }

        rep.kind = TURBOWASM_VALUE_I64;
        rep.as.i64 = (int64_t)pack_descriptor(
            info.descriptor);
        status = turbowasm_component_resource_new_owned(
            &filesystem->resources,
            TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
            rep,
            &resource);
        if (status != TURBOWASM_OK)
            goto fail;

        pair_items = (turbowasm_wasi02_value *)
            turbowasm_rt_calloc(2u, sizeof(*pair_items));
        if (pair_items == NULL) {
            rollback_resource(filesystem, resource);
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }

        pair_items[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
        pair_items[0].as.resource = resource;
        status = copy_path(
            info.guest_path, &pair_items[1]);
        if (status != TURBOWASM_OK) {
            rollback_resource(filesystem, resource);
            turbowasm_rt_free(pair_items);
            goto fail;
        }

        items[i].kind = TURBOWASM_WASI02_VALUE_TUPLE;
        items[i].as.tuple.items = pair_items;
        items[i].as.tuple.count = 2u;
    }

    out_result->kind = TURBOWASM_WASI02_VALUE_LIST;
    out_result->as.list.items = items;
    out_result->as.list.count = count;
    return TURBOWASM_OK;

fail:
    while (i != 0u) {
        turbowasm_wasi02_value *pair;
        --i;
        pair = &items[i];
        if (pair->kind == TURBOWASM_WASI02_VALUE_TUPLE &&
            pair->as.tuple.count == 2u &&
            pair->as.tuple.items != NULL &&
            pair->as.tuple.items[0].kind ==
                TURBOWASM_WASI02_VALUE_RESOURCE) {
            rollback_resource(
                filesystem,
                pair->as.tuple.items[0].as.resource);
        }
        turbowasm_wasi02_value_destroy(pair);
    }
    turbowasm_rt_free(items);
    return status;
}

turbowasm_status turbowasm_wasi02_filesystem_descriptor_resolve(
    const turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    turbowasm_wasi_fs_descriptor descriptor;
    turbowasm_status status;

    if (out_info == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = resource_descriptor_get(
        filesystem, resource, &descriptor);
    if (status != TURBOWASM_OK)
        return status;

    if (!turbowasm_wasi_fs_retained_descriptor_info_get(
            filesystem->filesystem,
            descriptor,
            out_info))
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_descriptor_drop(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource) {
    if (filesystem == NULL || !filesystem->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Preopen resources are logical guest handles. The backing preopen remains
     * host-owned and can be projected again by get-directories().
     */
    return turbowasm_component_resource_drop(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
        NULL,
        NULL);
}
