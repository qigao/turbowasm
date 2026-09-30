#include "wasi02_filesystem.h"
#include "wasi02_component.h"


#include "runtime_alloc.h"

#include <string.h>

#define TURBOWASM_WASI02_FS_PREOPEN_DESCRIPTOR_ID \
    UINT64_C(0x7761736930326673)
#define TURBOWASM_WASI02_FS_CHILD_DESCRIPTOR_ID \
    UINT64_C(0x7761736930326674)

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
        TURBOWASM_WASI02_FS_PREOPEN_DESCRIPTOR_ID,
        &rep);
    if (status != TURBOWASM_OK) {
        status = turbowasm_component_resource_rep(
            &filesystem->resources,
            resource,
            TURBOWASM_WASI02_FS_CHILD_DESCRIPTOR_ID,
            &rep);
        if (status != TURBOWASM_OK)
            return status;
    }
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
            TURBOWASM_WASI02_FS_PREOPEN_DESCRIPTOR_ID,
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
            TURBOWASM_WASI02_FS_PREOPEN_DESCRIPTOR_ID,
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
    turbowasm_value rep = {0};
    turbowasm_status status;
    turbowasm_wasi_fs_descriptor descriptor;
    uint32_t close_error;

    if (filesystem == NULL || !filesystem->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Preopen resources are guest-owned logical handles backed by host-owned
     * capabilities. Consuming the logical handle must not close the backing.
     */
    status = turbowasm_component_resource_drop(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_PREOPEN_DESCRIPTOR_ID,
        NULL,
        NULL);
    if (status == TURBOWASM_OK)
        return TURBOWASM_OK;

    /*
     * Child resources own their backing descriptor. Close first so a retriable
     * provider close failure leaves the logical handle live and retryable.
     */
    status = turbowasm_component_resource_rep(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_CHILD_DESCRIPTOR_ID,
        &rep);
    if (status != TURBOWASM_OK ||
        rep.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_TRAPPED;

    descriptor = unpack_descriptor((uint64_t)rep.as.i64);
    close_error = turbowasm_wasi_fs_close_descriptor(
        filesystem->filesystem, descriptor);
    if (close_error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return TURBOWASM_TRAPPED;

    return turbowasm_component_resource_drop(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_CHILD_DESCRIPTOR_ID,
        NULL,
        NULL);
}


enum {
    TW_WASI02_FS_PATH_SYMLINK_FOLLOW = 1u << 0,
    TW_WASI02_FS_OPEN_CREATE = 1u << 0,
    TW_WASI02_FS_OPEN_DIRECTORY = 1u << 1,
    TW_WASI02_FS_OPEN_EXCLUSIVE = 1u << 2,
    TW_WASI02_FS_OPEN_TRUNCATE = 1u << 3,
    TW_WASI02_FS_DESC_READ = 1u << 0,
    TW_WASI02_FS_DESC_WRITE = 1u << 1,
    TW_WASI02_FS_DESC_FILE_SYNC = 1u << 2,
    TW_WASI02_FS_DESC_DATA_SYNC = 1u << 3,
    TW_WASI02_FS_DESC_READ_SYNC = 1u << 4,
    TW_WASI02_FS_DESC_MUTATE_DIRECTORY = 1u << 5
};

static bool p2_error_code_from_errno(
    uint32_t error,
    uint32_t *out_index) {
    uint32_t index;

    switch (error) {
        case TURBOWASM_WASI_ERRNO_AGAIN: index = 1u; break;
        case TURBOWASM_WASI_ERRNO_BADF: index = 3u; break;
        case TURBOWASM_WASI_ERRNO_EXIST: index = 7u; break;
        case TURBOWASM_WASI_ERRNO_FBIG: index = 8u; break;
        case TURBOWASM_WASI_ERRNO_INTR: index = 11u; break;
        case TURBOWASM_WASI_ERRNO_INVAL: index = 12u; break;
        case TURBOWASM_WASI_ERRNO_IO: index = 13u; break;
        case TURBOWASM_WASI_ERRNO_ISDIR: index = 14u; break;
        case TURBOWASM_WASI_ERRNO_NAMETOOLONG: index = 18u; break;
        case TURBOWASM_WASI_ERRNO_NOENT: index = 20u; break;
        case TURBOWASM_WASI_ERRNO_NOMEM: index = 22u; break;
        case TURBOWASM_WASI_ERRNO_NOSPC: index = 23u; break;
        case TURBOWASM_WASI_ERRNO_NOTDIR: index = 24u; break;
        case TURBOWASM_WASI_ERRNO_NOTEMPTY: index = 25u; break;
        case TURBOWASM_WASI_ERRNO_NOSYS: index = 27u; break;
        case TURBOWASM_WASI_ERRNO_NOTCAPABLE: index = 31u; break;
        default:
            return false;
    }

    if (out_index != NULL)
        *out_index = index;
    return true;
}

static bool p2_descriptor_type_from_preview1(
    uint8_t file_type,
    uint32_t *out_index) {
    uint32_t index;

    switch (file_type) {
        case TURBOWASM_WASI_FILETYPE_UNKNOWN: index = 0u; break;
        case TURBOWASM_WASI_FILETYPE_BLOCK_DEVICE: index = 1u; break;
        case TURBOWASM_WASI_FILETYPE_CHARACTER_DEVICE: index = 2u; break;
        case TURBOWASM_WASI_FILETYPE_DIRECTORY: index = 3u; break;
        case TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK: index = 5u; break;
        case TURBOWASM_WASI_FILETYPE_REGULAR_FILE: index = 6u; break;
        case TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM:
        case TURBOWASM_WASI_FILETYPE_SOCKET_STREAM:
            index = 7u;
            break;
        default:
            return false;
    }

    if (out_index != NULL)
        *out_index = index;
    return true;
}

static turbowasm_status make_result_error(
    uint32_t error,
    turbowasm_wasi02_value *out) {
    uint32_t index;
    turbowasm_wasi02_value *value;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (error == TURBOWASM_WASI_ERRNO_MFILE)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!p2_error_code_from_errno(error, &index))
        return TURBOWASM_UNSUPPORTED;

    value = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*value));
    if (value == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    value->kind = TURBOWASM_WASI02_VALUE_ENUM;
    value->as.enum_index = index;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.is_error = true;
    out->as.result.value = value;
    return TURBOWASM_OK;
}

static turbowasm_status make_result_error_index(
    uint32_t index,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *value;

    if (out == NULL || index >= 37u)
        return TURBOWASM_INVALID_ARGUMENT;
    value = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*value));
    if (value == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    value->kind = TURBOWASM_WASI02_VALUE_ENUM;
    value->as.enum_index = index;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.is_error = true;
    out->as.result.value = value;
    return TURBOWASM_OK;
}


static turbowasm_status make_result_unit_ok(
    turbowasm_wasi02_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.is_error = false;
    return TURBOWASM_OK;
}

static turbowasm_status make_datetime_option(
    uint64_t nanoseconds,
    bool valid,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *record;
    turbowasm_wasi02_value *fields;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_OPTION;
    if (!valid)
        return TURBOWASM_OK;

    record = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*record));
    if (record == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    fields = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        2u, sizeof(*fields));
    if (fields == NULL) {
        turbowasm_rt_free(record);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    record->kind = TURBOWASM_WASI02_VALUE_RECORD;
    record->as.record.items = fields;
    record->as.record.count = 2u;
    fields[0].kind = TURBOWASM_WASI02_VALUE_U64;
    fields[0].as.u64 = nanoseconds / UINT64_C(1000000000);
    fields[1].kind = TURBOWASM_WASI02_VALUE_U32;
    fields[1].as.u32 =
        (uint32_t)(nanoseconds % UINT64_C(1000000000));

    out->as.option.has_value = true;
    out->as.option.value = record;
    return TURBOWASM_OK;
}

static turbowasm_status make_result_stat_ok(
    const turbowasm_wasi_fs_stat *stat,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *payload;
    turbowasm_wasi02_value *fields;
    uint32_t type_index;
    turbowasm_status status;

    if (stat == NULL || out == NULL ||
        !p2_descriptor_type_from_preview1(
            stat->file_type, &type_index))
        return TURBOWASM_UNSUPPORTED;

    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    fields = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        6u, sizeof(*fields));
    if (fields == NULL) {
        turbowasm_rt_free(payload);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    payload->kind = TURBOWASM_WASI02_VALUE_RECORD;
    payload->as.record.items = fields;
    payload->as.record.count = 6u;

    fields[0].kind = TURBOWASM_WASI02_VALUE_ENUM;
    fields[0].as.enum_index = type_index;
    fields[1].kind = TURBOWASM_WASI02_VALUE_U64;
    fields[1].as.u64 = stat->link_count;
    fields[2].kind = TURBOWASM_WASI02_VALUE_U64;
    fields[2].as.u64 = stat->size;

    status = make_datetime_option(
        stat->accessed_ns,
        (stat->timestamp_valid &
         TURBOWASM_WASI_FS_TIME_ACCESSED_VALID) != 0u,
        &fields[3]);
    if (status != TURBOWASM_OK)
        goto fail;
    status = make_datetime_option(
        stat->modified_ns,
        (stat->timestamp_valid &
         TURBOWASM_WASI_FS_TIME_MODIFIED_VALID) != 0u,
        &fields[4]);
    if (status != TURBOWASM_OK)
        goto fail;
    status = make_datetime_option(
        stat->changed_ns,
        (stat->timestamp_valid &
         TURBOWASM_WASI_FS_TIME_CHANGED_VALID) != 0u,
        &fields[5]);
    if (status != TURBOWASM_OK)
        goto fail;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.value = payload;
    return TURBOWASM_OK;

fail:
    turbowasm_wasi02_value_destroy(payload);
    turbowasm_rt_free(payload);
    return status;
}

static turbowasm_status make_result_resource_ok(
    uint32_t resource,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *payload;

    if (resource == 0u || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    payload->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    payload->as.resource = resource;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.value = payload;
    return TURBOWASM_OK;
}

static turbowasm_status child_resource_from_fd(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t guest_fd,
    uint32_t *out_resource) {
    turbowasm_wasi_fs_descriptor_info info = {0};
    turbowasm_value rep = {0};

    if (filesystem == NULL || out_resource == NULL ||
        !turbowasm_wasi_fs_descriptor_info_get(
            filesystem->filesystem, guest_fd, &info))
        return TURBOWASM_TRAPPED;

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = (int64_t)pack_descriptor(info.descriptor);
    return turbowasm_component_resource_new_owned(
        &filesystem->resources,
        TURBOWASM_WASI02_FS_CHILD_DESCRIPTOR_ID,
        rep,
        out_resource);
}

static uint64_t p2_open_rights(
    uint32_t descriptor_flags,
    uint32_t open_flags) {
    uint64_t rights =
        TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_OPEN;

    if ((descriptor_flags & TW_WASI02_FS_DESC_READ) != 0u)
        rights |= TURBOWASM_WASI_RIGHT_FD_READ;
    if ((descriptor_flags & TW_WASI02_FS_DESC_WRITE) != 0u)
        rights |= TURBOWASM_WASI_RIGHT_FD_WRITE;
    if ((open_flags & TW_WASI02_FS_OPEN_DIRECTORY) != 0u)
        rights |= TURBOWASM_WASI_RIGHT_FD_READDIR;
    if ((descriptor_flags &
         TW_WASI02_FS_DESC_MUTATE_DIRECTORY) != 0u) {
        rights |=
            TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
            TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
            TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
    }
    return rights;
}

static turbowasm_status filesystem_call_stat(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_wasi02_value *arguments,
    bool at_path,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi_fs_descriptor_info info = {0};
    turbowasm_wasi_fs_stat stat = {0};
    uint32_t error;

    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, arguments[0].as.resource, &info) !=
        TURBOWASM_OK)
        return make_result_error(
            TURBOWASM_WASI_ERRNO_BADF, out);

    if (at_path) {
        error = turbowasm_wasi_fs_path_stat(
            filesystem->filesystem,
            info.guest_fd,
            arguments[1].as.flags &
                TW_WASI02_FS_PATH_SYMLINK_FOLLOW,
            arguments[2].as.string.data,
            arguments[2].as.string.size,
            &stat);
    } else {
        error = turbowasm_wasi_fs_fd_stat(
            filesystem->filesystem,
            info.guest_fd,
            &stat);
    }

    return error == TURBOWASM_WASI_ERRNO_SUCCESS
        ? make_result_stat_ok(&stat, out)
        : make_result_error(error, out);
}

static turbowasm_status filesystem_call_path_mutation(
    turbowasm_wasi02_filesystem *filesystem,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint32_t error;

    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, arguments[0].as.resource, &info) !=
        TURBOWASM_OK)
        return make_result_error(
            TURBOWASM_WASI_ERRNO_BADF, out);

    if (strcmp(
            function_name,
            "[method]descriptor.create-directory-at") == 0) {
        error = turbowasm_wasi_fs_path_create_directory(
            filesystem->filesystem,
            info.guest_fd,
            arguments[1].as.string.data,
            arguments[1].as.string.size);
    } else if (strcmp(
                   function_name,
                   "[method]descriptor.remove-directory-at") == 0) {
        error = turbowasm_wasi_fs_path_remove_directory(
            filesystem->filesystem,
            info.guest_fd,
            arguments[1].as.string.data,
            arguments[1].as.string.size);
    } else {
        error = turbowasm_wasi_fs_path_unlink_file(
            filesystem->filesystem,
            info.guest_fd,
            arguments[1].as.string.data,
            arguments[1].as.string.size);
    }

    return error == TURBOWASM_WASI_ERRNO_SUCCESS
        ? make_result_unit_ok(out)
        : make_result_error(error, out);
}

static turbowasm_status filesystem_call_open_at(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi_fs_descriptor_info base = {0};
    turbowasm_wasi_fs_descriptor_info child = {0};
    uint32_t path_flags = arguments[1].as.flags;
    uint32_t open_flags = arguments[3].as.flags;
    uint32_t descriptor_flags = arguments[4].as.flags;
    uint64_t rights;
    uint64_t inheriting;
    uint64_t mutate_rights =
        TURBOWASM_WASI_RIGHT_FD_WRITE |
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
    bool requires_mutate;
    bool base_can_mutate;
    uint32_t guest_fd = 0u;
    uint32_t resource = 0u;
    uint32_t error;
    turbowasm_status status;

    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, arguments[0].as.resource, &base) !=
        TURBOWASM_OK)
        return make_result_error(
            TURBOWASM_WASI_ERRNO_BADF, out);

    if ((descriptor_flags &
         (TW_WASI02_FS_DESC_FILE_SYNC |
          TW_WASI02_FS_DESC_DATA_SYNC |
          TW_WASI02_FS_DESC_READ_SYNC)) != 0u)
        return make_result_error(
            TURBOWASM_WASI_ERRNO_NOSYS, out);

    requires_mutate =
        (descriptor_flags &
         (TW_WASI02_FS_DESC_WRITE |
          TW_WASI02_FS_DESC_MUTATE_DIRECTORY)) != 0u ||
        (open_flags &
         (TW_WASI02_FS_OPEN_CREATE |
          TW_WASI02_FS_OPEN_TRUNCATE)) != 0u;
    base_can_mutate =
        (base.rights_base &
         (TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
          TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
          TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE)) != 0u;
    if (requires_mutate && !base_can_mutate)
        return make_result_error_index(33u, out); /* read-only */

    rights = p2_open_rights(
        descriptor_flags, open_flags);
    inheriting = base.rights_inheriting;
    if ((descriptor_flags &
         TW_WASI02_FS_DESC_MUTATE_DIRECTORY) == 0u)
        inheriting &= ~mutate_rights;

    error = turbowasm_wasi_fs_path_open(
        filesystem->filesystem,
        base.guest_fd,
        path_flags & TW_WASI02_FS_PATH_SYMLINK_FOLLOW,
        arguments[2].as.string.data,
        arguments[2].as.string.size,
        open_flags &
            (TW_WASI02_FS_OPEN_CREATE |
             TW_WASI02_FS_OPEN_DIRECTORY |
             TW_WASI02_FS_OPEN_EXCLUSIVE |
             TW_WASI02_FS_OPEN_TRUNCATE),
        rights,
        inheriting,
        0u,
        &guest_fd);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return make_result_error(error, out);

    if (!turbowasm_wasi_fs_descriptor_info_get(
            filesystem->filesystem,
            guest_fd,
            &child)) {
        (void)turbowasm_wasi_fs_close_fd(
            filesystem->filesystem, guest_fd);
        return TURBOWASM_TRAPPED;
    }

    status = child_resource_from_fd(
        filesystem, guest_fd, &resource);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi_fs_close_descriptor(
            filesystem->filesystem,
            child.descriptor);
        return status;
    }

    status = make_result_resource_ok(resource, out);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi02_filesystem_descriptor_drop(
            filesystem, resource);
        return status;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_filesystem_call(
    turbowasm_wasi02_filesystem *filesystem,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;
    size_t i;
    turbowasm_status status;

    if (filesystem == NULL || !filesystem->initialized ||
        function_name == NULL || out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    iface = turbowasm_wasi02_find_interface(
        "wasi:filesystem", "types");
    function = turbowasm_wasi02_find_function(
        iface, function_name);
    if (function == NULL ||
        function->param_count != argument_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < argument_count; ++i) {
        if (!turbowasm_wasi02_value_matches_type(
                function->params[i].type,
                &arguments[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    memset(out_result, 0, sizeof(*out_result));
    if (strcmp(
            function_name,
            "[method]descriptor.stat") == 0) {
        status = filesystem_call_stat(
            filesystem, arguments, false, out_result);
    } else if (strcmp(
                   function_name,
                   "[method]descriptor.stat-at") == 0) {
        status = filesystem_call_stat(
            filesystem, arguments, true, out_result);
    } else if (strcmp(
                   function_name,
                   "[method]descriptor.open-at") == 0) {
        status = filesystem_call_open_at(
            filesystem, arguments, out_result);
    } else if (strcmp(
                   function_name,
                   "[method]descriptor.create-directory-at") == 0 ||
               strcmp(
                   function_name,
                   "[method]descriptor.remove-directory-at") == 0 ||
               strcmp(
                   function_name,
                   "[method]descriptor.unlink-file-at") == 0) {
        status = filesystem_call_path_mutation(
            filesystem, function_name,
            arguments, out_result);
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    if (status != TURBOWASM_OK)
        return status;
    if (!turbowasm_wasi02_value_matches_type(
            function->result, out_result)) {
        turbowasm_wasi02_value_destroy(out_result);
        return TURBOWASM_MALFORMED_MODULE;
    }
    return TURBOWASM_OK;
}

static bool component_name_is(
    turbowasm_component_name name,
    const char *expected) {
    size_t size;

    if (expected == NULL)
        return false;
    size = strlen(expected);
    return size == name.size &&
           (size == 0u ||
            (name.bytes != NULL &&
             memcmp(name.bytes, expected, size) == 0));
}

static const turbowasm_component_type *type_from_ref(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    if (graph == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return NULL;
    return turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
}

static bool bind_preopens_shape(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index) {
    const turbowasm_component_type *function_type;
    const turbowasm_component_type *list_type;
    const turbowasm_component_type *tuple_type;
    const turbowasm_component_type *own_type;
    const turbowasm_component_type *resource_type;
    const turbowasm_component_type *path_type = NULL;
    turbowasm_component_type_ref result_ref;
    turbowasm_component_type_ref path_ref;
    uint64_t identity;

    if (filesystem == NULL || !filesystem->initialized ||
        graph == NULL)
        return false;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count != 0u ||
        !function_type->as.function.has_result)
        return false;

    result_ref = function_type->as.function.result;
    list_type = type_from_ref(graph, result_ref);
    if (list_type == NULL ||
        list_type->kind != TURBOWASM_COMPONENT_TYPE_LIST)
        return false;

    tuple_type = type_from_ref(
        graph, list_type->as.list.element_type);
    if (tuple_type == NULL ||
        tuple_type->kind != TURBOWASM_COMPONENT_TYPE_TUPLE ||
        tuple_type->as.tuple.count != 2u)
        return false;

    own_type = type_from_ref(
        graph, tuple_type->as.tuple.elements[0]);
    if (own_type == NULL ||
        own_type->kind != TURBOWASM_COMPONENT_TYPE_OWN)
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, own_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
        return false;
    identity = resource_type->as.resource.identity;
    if (identity == 0u)
        return false;

    path_ref = tuple_type->as.tuple.elements[1];
    if (path_ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        if (path_ref.as.inline_type !=
            TURBOWASM_COMPONENT_TYPE_STRING)
            return false;
    } else {
        path_type = turbowasm_component_type_graph_get(
            graph, path_ref.as.indexed);
        if (path_type == NULL ||
            path_type->kind != TURBOWASM_COMPONENT_TYPE_STRING)
            return false;
    }

    if (filesystem->descriptor_identity_bound &&
        filesystem->descriptor_identity != identity)
        return false;
    filesystem->descriptor_identity = identity;
    filesystem->descriptor_identity_bound = true;
    return true;
}

static const turbowasm_wasi02_type_desc *fs_wasi_type_base(
    const turbowasm_wasi02_type_desc *type) {
    uint32_t depth = 0u;

    while (type != NULL &&
           type->kind == TURBOWASM_WASI02_TYPE_ALIAS) {
        if (++depth > 32u)
            return NULL;
        type = type->as.alias.target;
    }
    return type;
}

static bool fs_ref_kind(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind *out_kind,
    const turbowasm_component_type **out_type) {
    if (graph == NULL || out_kind == NULL || out_type == NULL)
        return false;

    *out_type = NULL;
    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        *out_kind = ref.as.inline_type;
        return true;
    }
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    *out_type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (*out_type == NULL)
        return false;
    *out_kind = (*out_type)->kind;
    return true;
}

static bool fs_bind_descriptor_identity(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *handle_type,
    turbowasm_component_type_kind expected_handle_kind) {
    const turbowasm_component_type *resource_type;
    uint64_t identity;

    if (filesystem == NULL || graph == NULL ||
        handle_type == NULL ||
        handle_type->kind != expected_handle_kind)
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
        return false;

    identity = resource_type->as.resource.identity;
    if (identity == 0u)
        return false;
    if (filesystem->descriptor_identity_bound &&
        filesystem->descriptor_identity != identity)
        return false;

    filesystem->descriptor_identity = identity;
    filesystem->descriptor_identity_bound = true;
    return true;
}

static bool fs_component_type_matches_wasi(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_wasi02_type_desc *wasi_type,
    turbowasm_component_type_kind resource_handle_kind,
    uint32_t depth) {
    const turbowasm_wasi02_type_desc *base;
    const turbowasm_component_type *type;
    turbowasm_component_type_kind kind;
    uint32_t i;

    if (depth > 64u)
        return false;
    base = fs_wasi_type_base(wasi_type);
    if (base == NULL ||
        !fs_ref_kind(graph, ref, &kind, &type))
        return false;

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
            return kind == TURBOWASM_COMPONENT_TYPE_BOOL;
        case TURBOWASM_WASI02_TYPE_U8:
            return kind == TURBOWASM_COMPONENT_TYPE_U8;
        case TURBOWASM_WASI02_TYPE_U32:
            return kind == TURBOWASM_COMPONENT_TYPE_U32;
        case TURBOWASM_WASI02_TYPE_U64:
            return kind == TURBOWASM_COMPONENT_TYPE_U64;
        case TURBOWASM_WASI02_TYPE_STRING:
            return kind == TURBOWASM_COMPONENT_TYPE_STRING;

        case TURBOWASM_WASI02_TYPE_LIST:
            return kind == TURBOWASM_COMPONENT_TYPE_LIST &&
                   type != NULL &&
                   fs_component_type_matches_wasi(
                       filesystem, graph,
                       type->as.list.element_type,
                       base->as.list.element,
                       resource_handle_kind,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (kind != TURBOWASM_COMPONENT_TYPE_TUPLE ||
                type == NULL ||
                type->as.tuple.count != base->as.tuple.count)
                return false;
            for (i = 0u; i < base->as.tuple.count; ++i) {
                if (!fs_component_type_matches_wasi(
                        filesystem, graph,
                        type->as.tuple.elements[i],
                        base->as.tuple.elements[i],
                        resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_RECORD:
            if (kind != TURBOWASM_COMPONENT_TYPE_RECORD ||
                type == NULL ||
                type->as.record.count != base->as.record.count)
                return false;
            for (i = 0u; i < base->as.record.count; ++i) {
                const turbowasm_component_record_field *field =
                    &type->as.record.fields[i];
                const turbowasm_wasi02_record_field *expected =
                    &base->as.record.fields[i];
                size_t name_size;

                if (expected->name == NULL)
                    return false;
                name_size = strlen(expected->name);
                if (name_size != field->name_size ||
                    field->name == NULL ||
                    memcmp(
                        field->name,
                        expected->name,
                        name_size) != 0 ||
                    !fs_component_type_matches_wasi(
                        filesystem, graph, field->type,
                        expected->type, resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_OPTION:
            return kind == TURBOWASM_COMPONENT_TYPE_OPTION &&
                   type != NULL &&
                   fs_component_type_matches_wasi(
                       filesystem, graph,
                       type->as.option.payload,
                       base->as.option.payload,
                       resource_handle_kind,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_RESULT:
            if (kind != TURBOWASM_COMPONENT_TYPE_RESULT ||
                type == NULL ||
                type->as.result.has_ok !=
                    (base->as.result.ok != NULL) ||
                type->as.result.has_error !=
                    (base->as.result.error != NULL))
                return false;
            if (type->as.result.has_ok &&
                !fs_component_type_matches_wasi(
                    filesystem, graph,
                    type->as.result.ok,
                    base->as.result.ok,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            if (type->as.result.has_error &&
                !fs_component_type_matches_wasi(
                    filesystem, graph,
                    type->as.result.error,
                    base->as.result.error,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            return true;

        case TURBOWASM_WASI02_TYPE_ENUM: {
            const turbowasm_component_label *labels;

            if (kind != TURBOWASM_COMPONENT_TYPE_ENUM ||
                type == NULL ||
                type->as.enumeration.count !=
                    base->as.enumeration.count ||
                type->as.enumeration.labels == NULL ||
                base->as.enumeration.labels == NULL)
                return false;
            labels = type->as.enumeration.labels;
            for (i = 0u; i < base->as.enumeration.count; ++i) {
                size_t name_size;

                if (base->as.enumeration.labels[i] == NULL)
                    return false;
                name_size = strlen(
                    base->as.enumeration.labels[i]);
                if (name_size != labels[i].name_size ||
                    labels[i].name == NULL ||
                    memcmp(
                        labels[i].name,
                        base->as.enumeration.labels[i],
                        name_size) != 0)
                    return false;
            }
            return true;
        }

        case TURBOWASM_WASI02_TYPE_FLAGS: {
            const turbowasm_component_label *labels;

            if (kind != TURBOWASM_COMPONENT_TYPE_FLAGS ||
                type == NULL ||
                type->as.flags.count != base->as.flags.count ||
                type->as.flags.labels == NULL ||
                base->as.flags.labels == NULL)
                return false;
            labels = type->as.flags.labels;
            for (i = 0u; i < base->as.flags.count; ++i) {
                size_t name_size;

                if (base->as.flags.labels[i] == NULL)
                    return false;
                name_size = strlen(
                    base->as.flags.labels[i]);
                if (name_size != labels[i].name_size ||
                    labels[i].name == NULL ||
                    memcmp(
                        labels[i].name,
                        base->as.flags.labels[i],
                        name_size) != 0)
                    return false;
            }
            return true;
        }

        case TURBOWASM_WASI02_TYPE_RESOURCE:
            return type != NULL &&
                   fs_bind_descriptor_identity(
                       filesystem, graph, type,
                       resource_handle_kind);

        case TURBOWASM_WASI02_TYPE_UNIT:
        case TURBOWASM_WASI02_TYPE_ALIAS:
        default:
            return false;
    }
}

static const turbowasm_wasi02_function_desc *
fs_function_by_component_name(
    turbowasm_component_name name) {
    const turbowasm_wasi02_interface_desc *iface =
        turbowasm_wasi02_find_interface(
            "wasi:filesystem", "types");
    uint32_t i;

    if (iface == NULL ||
        (name.size != 0u && name.bytes == NULL))
        return NULL;

    for (i = 0u; i < iface->function_count; ++i) {
        const turbowasm_wasi02_function_desc *function =
            &iface->functions[i];
        size_t size = strlen(function->name);

        if (size == name.size &&
            (size == 0u ||
             memcmp(name.bytes, function->name, size) == 0))
            return function;
    }
    return NULL;
}

static bool bind_filesystem_method_shape(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_wasi02_function_desc *function) {
    const turbowasm_component_type *function_type;
    uint32_t i;

    if (filesystem == NULL || graph == NULL ||
        function == NULL)
        return false;
    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count !=
            function->param_count ||
        function_type->as.function.has_result !=
            (function->result != NULL))
        return false;

    for (i = 0u; i < function->param_count; ++i) {
        if (!fs_component_type_matches_wasi(
                filesystem, graph,
                function_type->as.function.params[i],
                function->params[i].type,
                TURBOWASM_COMPONENT_TYPE_BORROW,
                0u))
            return false;
    }

    if (function->result != NULL &&
        !fs_component_type_matches_wasi(
            filesystem, graph,
            function_type->as.function.result,
            function->result,
            TURBOWASM_COMPONENT_TYPE_OWN,
            0u))
        return false;
    return true;
}

static bool wasi02_fs_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;

    if (component_name_is(
            instance_name,
            "wasi:filesystem/preopens@0.2.8") &&
        component_name_is(
            function_name,
            "get-directories"))
        return bind_preopens_shape(
            filesystem, graph, function_type);

    if (component_name_is(
            instance_name,
            "wasi:filesystem/types@0.2.8")) {
        const turbowasm_wasi02_function_desc *function =
            fs_function_by_component_name(function_name);
        return bind_filesystem_method_shape(
            filesystem, graph, function_type, function);
    }

    return false;
}

static void rollback_wasi_preopen_resources(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi02_value *value) {
    size_t i;

    if (filesystem == NULL || value == NULL ||
        value->kind != TURBOWASM_WASI02_VALUE_LIST)
        return;

    for (i = 0u; i < value->as.list.count; ++i) {
        turbowasm_wasi02_value *pair =
            &value->as.list.items[i];
        if (pair->kind == TURBOWASM_WASI02_VALUE_TUPLE &&
            pair->as.tuple.count == 2u &&
            pair->as.tuple.items != NULL &&
            pair->as.tuple.items[0].kind ==
                TURBOWASM_WASI02_VALUE_RESOURCE) {
            (void)turbowasm_wasi02_filesystem_descriptor_drop(
                filesystem,
                pair->as.tuple.items[0].as.resource);
        }
    }
}

static turbowasm_status preopens_to_component_value(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi02_value *source,
    turbowasm_component_value *out) {
    turbowasm_component_value *items = NULL;
    size_t i;

    if (filesystem == NULL || source == NULL || out == NULL ||
        source->kind != TURBOWASM_WASI02_VALUE_LIST ||
        (source->as.list.count != 0u &&
         source->as.list.items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    if (source->as.list.count != 0u) {
        if (source->as.list.count >
            SIZE_MAX / sizeof(*items))
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_component_value *)
            turbowasm_rt_calloc(
                source->as.list.count,
                sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < source->as.list.count; ++i) {
        const turbowasm_wasi02_value *pair =
            &source->as.list.items[i];
        turbowasm_component_value *pair_items;
        const turbowasm_wasi02_value *resource;
        const turbowasm_wasi02_value *path;

        if (pair->kind != TURBOWASM_WASI02_VALUE_TUPLE ||
            pair->as.tuple.count != 2u ||
            pair->as.tuple.items == NULL) {
            goto mismatch;
        }
        resource = &pair->as.tuple.items[0];
        path = &pair->as.tuple.items[1];
        if (resource->kind != TURBOWASM_WASI02_VALUE_RESOURCE ||
            path->kind != TURBOWASM_WASI02_VALUE_STRING ||
            (path->as.string.size != 0u &&
             path->as.string.data == NULL))
            goto mismatch;

        pair_items = (turbowasm_component_value *)
            turbowasm_rt_calloc(2u, sizeof(*pair_items));
        if (pair_items == NULL)
            goto oom;

        pair_items[0].kind = TURBOWASM_COMPONENT_TYPE_OWN;
        pair_items[0].as.resource_rep.kind =
            TURBOWASM_VALUE_I32;
        pair_items[0].as.resource_rep.as.i32 =
            (int32_t)resource->as.resource;

        pair_items[1].kind = TURBOWASM_COMPONENT_TYPE_STRING;
        if (path->as.string.size != 0u) {
            pair_items[1].as.string.data =
                (uint8_t *)turbowasm_rt_malloc(
                    path->as.string.size);
            if (pair_items[1].as.string.data == NULL) {
                turbowasm_rt_free(pair_items);
                goto oom;
            }
            memcpy(
                pair_items[1].as.string.data,
                path->as.string.data,
                path->as.string.size);
        }
        pair_items[1].as.string.size =
            path->as.string.size;

        items[i].kind = TURBOWASM_COMPONENT_TYPE_TUPLE;
        items[i].as.tuple.items = pair_items;
        items[i].as.tuple.count = 2u;
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_COMPONENT_TYPE_LIST;
    out->as.list.items = items;
    out->as.list.count = source->as.list.count;
    return TURBOWASM_OK;

mismatch:
    while (i != 0u) {
        --i;
        turbowasm_component_value_destroy(&items[i]);
    }
    turbowasm_rt_free(items);
    return TURBOWASM_TYPE_MISMATCH;

oom:
    while (i != 0u) {
        --i;
        turbowasm_component_value_destroy(&items[i]);
    }
    turbowasm_rt_free(items);
    return TURBOWASM_OUT_OF_MEMORY;
}

static void rollback_method_result_resource(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_wasi02_function_desc *function,
    turbowasm_wasi02_value *result) {
    if (filesystem == NULL || function == NULL ||
        result == NULL ||
        strcmp(
            function->name,
            "[method]descriptor.open-at") != 0 ||
        result->kind != TURBOWASM_WASI02_VALUE_RESULT ||
        result->as.result.is_error ||
        result->as.result.value == NULL ||
        result->as.result.value->kind !=
            TURBOWASM_WASI02_VALUE_RESOURCE)
        return;

    (void)turbowasm_wasi02_filesystem_descriptor_drop(
        filesystem,
        result->as.result.value->as.resource);
}

static turbowasm_status invoke_filesystem_method(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_component_name function_name,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result) {
    const turbowasm_wasi02_function_desc *function =
        fs_function_by_component_name(function_name);
    turbowasm_wasi02_value
        wasi_arguments[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {{0}};
    turbowasm_wasi02_value result = {0};
    size_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (filesystem == NULL || function == NULL ||
        out_result == NULL ||
        argument_count != function->param_count ||
        argument_count > TURBOWASM_COMPONENT_MAX_FLAT_PARAMS ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < argument_count; ++i) {
        status = turbowasm_wasi02_component_value_to_wasi(
            function->params[i].type,
            &arguments[i],
            &wasi_arguments[i]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_wasi02_filesystem_call(
        filesystem,
        function->name,
        wasi_arguments,
        argument_count,
        &result);
    if (status != TURBOWASM_OK)
        goto done;

    status = turbowasm_wasi02_component_value_from_wasi(
        function->result,
        &result,
        out_result);
    if (status != TURBOWASM_OK)
        rollback_method_result_resource(
            filesystem, function, &result);

done:
    for (i = 0u; i < argument_count; ++i)
        turbowasm_wasi02_value_destroy(&wasi_arguments[i]);
    turbowasm_wasi02_value_destroy(&result);
    return status;
}

static turbowasm_status wasi02_fs_invoke(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;
    turbowasm_wasi02_value result = {0};
    turbowasm_status status;

    if (filesystem == NULL || !filesystem->initialized ||
        out_result == NULL || trap == NULL ||
        !wasi02_fs_can_bind(
            context, instance_name, function_name,
            graph, function_type))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;

    if (component_name_is(
            instance_name,
            "wasi:filesystem/types@0.2.8"))
        return invoke_filesystem_method(
            filesystem, function_name,
            arguments, argument_count, out_result);

    if (argument_count != 0u)
        return TURBOWASM_TYPE_MISMATCH;

    status = turbowasm_wasi02_filesystem_get_directories(
        filesystem, &result);
    if (status != TURBOWASM_OK)
        return status;

    status = preopens_to_component_value(
        filesystem, &result, out_result);
    if (status != TURBOWASM_OK)
        rollback_wasi_preopen_resources(
            filesystem, &result);

    turbowasm_wasi02_value_destroy(&result);
    return status;
}

static bool imported_resource_identity(
    turbowasm_wasi02_filesystem *filesystem,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    turbowasm_component_type_kind *out_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;

    if (filesystem == NULL ||
        !filesystem->descriptor_identity_bound ||
        graph == NULL ||
        type.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, type.as.indexed);
    if (handle_type == NULL ||
        (handle_type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
         handle_type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity !=
            filesystem->descriptor_identity)
        return false;

    if (out_kind != NULL)
        *out_kind = handle_type->kind;
    return true;
}

static turbowasm_status wasi02_fs_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;
    turbowasm_component_type_kind kind;
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint32_t handle;

    if (value == NULL || out_handle == NULL ||
        !imported_resource_identity(
            filesystem, graph, type, &kind) ||
        value->kind != kind ||
        value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle = (uint32_t)value->as.resource_rep.as.i32;
    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, handle, &info) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_fs_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;
    turbowasm_component_type_kind kind;
    turbowasm_wasi_fs_descriptor_info info = {0};

    if (out == NULL ||
        !imported_resource_identity(
            filesystem, graph, type, &kind) ||
        turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, handle, &info) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->as.resource_rep.kind = TURBOWASM_VALUE_I32;
    out->as.resource_rep.as.i32 = (int32_t)handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_fs_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;

    if (filesystem == NULL ||
        !filesystem->initialized ||
        !filesystem->descriptor_identity_bound ||
        resource_identity != filesystem->descriptor_identity)
        return TURBOWASM_TYPE_MISMATCH;

    return turbowasm_wasi02_filesystem_descriptor_drop(
        filesystem, handle);
}

turbowasm_status turbowasm_wasi02_filesystem_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_filesystem *filesystem) {
    turbowasm_component_exec_imports imports;

    if (exec == NULL || binary == NULL ||
        filesystem == NULL || !filesystem->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(&imports, 0, sizeof(imports));
    imports.context = filesystem;
    imports.can_bind = wasi02_fs_can_bind;
    imports.invoke = wasi02_fs_invoke;
    imports.resource_lower = wasi02_fs_resource_lower;
    imports.resource_lift = wasi02_fs_resource_lift;
    imports.resource_drop = wasi02_fs_resource_drop;

    return turbowasm_component_exec_init_with_imports(
        exec, binary, &imports);
}
