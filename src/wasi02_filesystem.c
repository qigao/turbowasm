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
        status = turbowasm_component_resource_new_borrowed(
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

static turbowasm_status descriptor_backing_close(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;
    turbowasm_wasi_fs_descriptor descriptor;
    uint32_t error;

    if (filesystem == NULL || !filesystem->initialized ||
        resource_identity != TURBOWASM_WASI02_FS_DESCRIPTOR_ID ||
        rep.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_INVALID_ARGUMENT;

    descriptor = unpack_descriptor((uint64_t)rep.as.i64);
    error = turbowasm_wasi_fs_close_descriptor(
        filesystem->filesystem, descriptor);
    return error == TURBOWASM_WASI_ERRNO_SUCCESS
        ? TURBOWASM_OK
        : TURBOWASM_TRAPPED;
}

turbowasm_status turbowasm_wasi02_filesystem_descriptor_drop(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t resource) {
    if (filesystem == NULL || !filesystem->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_component_resource_drop(
        &filesystem->resources,
        resource,
        TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
        descriptor_backing_close,
        filesystem);
}

enum {
    TW_WASI02_PATH_SYMLINK_FOLLOW = 1u << 0,
    TW_WASI02_OPEN_CREATE = 1u << 0,
    TW_WASI02_OPEN_DIRECTORY = 1u << 1,
    TW_WASI02_OPEN_EXCLUSIVE = 1u << 2,
    TW_WASI02_OPEN_TRUNCATE = 1u << 3,
    TW_WASI02_DESCRIPTOR_READ = 1u << 0,
    TW_WASI02_DESCRIPTOR_WRITE = 1u << 1,
    TW_WASI02_DESCRIPTOR_FILE_SYNC = 1u << 2,
    TW_WASI02_DESCRIPTOR_DATA_SYNC = 1u << 3,
    TW_WASI02_DESCRIPTOR_REQUESTED_WRITE_SYNC = 1u << 4,
    TW_WASI02_DESCRIPTOR_MUTATE_DIRECTORY = 1u << 5,

    TW_WASI_PREVIEW1_LOOKUP_SYMLINK_FOLLOW = 1u,
    TW_WASI_PREVIEW1_O_CREAT = 1u,
    TW_WASI_PREVIEW1_O_DIRECTORY = 2u,
    TW_WASI_PREVIEW1_O_EXCL = 4u,
    TW_WASI_PREVIEW1_O_TRUNC = 8u,
    TW_WASI_PREVIEW1_FDFLAG_DSYNC = 2u,
    TW_WASI_PREVIEW1_FDFLAG_RSYNC = 8u,
    TW_WASI_PREVIEW1_FDFLAG_SYNC = 16u
};

static bool errno_to_wasi02_error_code(
    uint32_t error,
    uint32_t *out_case) {
    if (out_case == NULL)
        return false;

    switch (error) {
        case TURBOWASM_WASI_ERRNO_AGAIN:
            *out_case = 1u;  /* would-block */
            return true;
        case TURBOWASM_WASI_ERRNO_BADF:
            *out_case = 3u;  /* bad-descriptor */
            return true;
        case TURBOWASM_WASI_ERRNO_EXIST:
            *out_case = 7u;  /* exist */
            return true;
        case TURBOWASM_WASI_ERRNO_FAULT:
        case TURBOWASM_WASI_ERRNO_INVAL:
            *out_case = 12u; /* invalid */
            return true;
        case TURBOWASM_WASI_ERRNO_FBIG:
            *out_case = 8u;  /* file-too-large */
            return true;
        case TURBOWASM_WASI_ERRNO_INTR:
            *out_case = 11u; /* interrupted */
            return true;
        case TURBOWASM_WASI_ERRNO_IO:
            *out_case = 13u; /* io */
            return true;
        case TURBOWASM_WASI_ERRNO_ISDIR:
            *out_case = 14u; /* is-directory */
            return true;
        case TURBOWASM_WASI_ERRNO_NAMETOOLONG:
            *out_case = 18u; /* name-too-long */
            return true;
        case TURBOWASM_WASI_ERRNO_NOENT:
            *out_case = 20u; /* no-entry */
            return true;
        case TURBOWASM_WASI_ERRNO_NOMEM:
            *out_case = 22u; /* insufficient-memory */
            return true;
        case TURBOWASM_WASI_ERRNO_NOSPC:
            *out_case = 23u; /* insufficient-space */
            return true;
        case TURBOWASM_WASI_ERRNO_NOSYS:
            *out_case = 27u; /* unsupported */
            return true;
        case TURBOWASM_WASI_ERRNO_NOTDIR:
            *out_case = 24u; /* not-directory */
            return true;
        case TURBOWASM_WASI_ERRNO_NOTEMPTY:
            *out_case = 25u; /* not-empty */
            return true;
        case TURBOWASM_WASI_ERRNO_NOTCAPABLE:
            *out_case = 31u; /* not-permitted */
            return true;
        default:
            return false;
    }
}

static turbowasm_status make_wasi02_error_result(
    uint32_t error,
    turbowasm_wasi02_value *out_result) {
    turbowasm_wasi02_value *payload;
    uint32_t error_case;

    if (out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (error == TURBOWASM_WASI_ERRNO_MFILE)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!errno_to_wasi02_error_code(error, &error_case))
        return TURBOWASM_UNSUPPORTED;

    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    payload->kind = TURBOWASM_WASI02_VALUE_ENUM;
    payload->as.enum_index = error_case;

    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out_result->as.result.is_error = true;
    out_result->as.result.value = payload;
    return TURBOWASM_OK;
}

static turbowasm_status make_wasi02_unit_success(
    turbowasm_wasi02_value *out_result) {
    if (out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out_result->as.result.is_error = false;
    out_result->as.result.value = NULL;
    return TURBOWASM_OK;
}

static turbowasm_status descriptor_resource_create_owned(
    turbowasm_wasi02_filesystem *filesystem,
    turbowasm_wasi_fs_descriptor descriptor,
    uint32_t *out_resource) {
    turbowasm_value rep = {0};

    if (filesystem == NULL || out_resource == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = (int64_t)pack_descriptor(descriptor);
    return turbowasm_component_resource_new_owned(
        &filesystem->resources,
        TURBOWASM_WASI02_FS_DESCRIPTOR_ID,
        rep,
        out_resource);
}

static turbowasm_status make_wasi02_resource_success(
    uint32_t resource,
    turbowasm_wasi02_value *out_result) {
    turbowasm_wasi02_value *payload;

    if (out_result == NULL || resource == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    payload->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    payload->as.resource = resource;

    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out_result->as.result.is_error = false;
    out_result->as.result.value = payload;
    return TURBOWASM_OK;
}

typedef uint32_t (*wasi02_path_mutation_fn)(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length);

static turbowasm_status wasi02_path_mutation(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    wasi02_path_mutation_fn mutation,
    turbowasm_wasi02_value *out_result) {
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint32_t error;

    if (filesystem == NULL || !filesystem->initialized ||
        mutation == NULL || out_result == NULL ||
        (path.size != 0u && path.data == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, descriptor_resource, &info) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    error = mutation(
        filesystem->filesystem,
        info.guest_fd,
        path.data,
        path.size);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS)
        return make_wasi02_unit_success(out_result);
    return make_wasi02_error_result(error, out_result);
}

turbowasm_status turbowasm_wasi02_filesystem_create_directory_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result) {
    return wasi02_path_mutation(
        filesystem,
        descriptor_resource,
        path,
        turbowasm_wasi_fs_path_create_directory,
        out_result);
}

turbowasm_status turbowasm_wasi02_filesystem_remove_directory_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result) {
    return wasi02_path_mutation(
        filesystem,
        descriptor_resource,
        path,
        turbowasm_wasi_fs_path_remove_directory,
        out_result);
}

turbowasm_status turbowasm_wasi02_filesystem_unlink_file_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    turbowasm_wasi02_string_view path,
    turbowasm_wasi02_value *out_result) {
    return wasi02_path_mutation(
        filesystem,
        descriptor_resource,
        path,
        turbowasm_wasi_fs_path_unlink_file,
        out_result);
}

turbowasm_status turbowasm_wasi02_filesystem_open_at(
    turbowasm_wasi02_filesystem *filesystem,
    uint32_t descriptor_resource,
    uint32_t path_flags,
    turbowasm_wasi02_string_view path,
    uint32_t open_flags,
    uint32_t descriptor_flags,
    turbowasm_wasi02_value *out_result) {
    turbowasm_wasi_fs_descriptor_info base = {0};
    turbowasm_wasi_fs_descriptor_info child = {0};
    uint32_t dirflags = 0u;
    uint32_t oflags = 0u;
    uint32_t fdflags = 0u;
    uint64_t rights_base = TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET;
    uint64_t rights_inheriting = 0u;
    uint32_t guest_fd = 0u;
    uint32_t resource = 0u;
    uint32_t error;
    turbowasm_status status;

    if (filesystem == NULL || !filesystem->initialized ||
        out_result == NULL ||
        (path.size != 0u && path.data == NULL) ||
        (path_flags & ~UINT32_C(0x1)) != 0u ||
        (open_flags & ~UINT32_C(0xf)) != 0u ||
        (descriptor_flags & ~UINT32_C(0x3f)) != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    if (turbowasm_wasi02_filesystem_descriptor_resolve(
            filesystem, descriptor_resource, &base) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    if ((path_flags & TW_WASI02_PATH_SYMLINK_FOLLOW) != 0u)
        dirflags |= TW_WASI_PREVIEW1_LOOKUP_SYMLINK_FOLLOW;
    if ((open_flags & TW_WASI02_OPEN_CREATE) != 0u)
        oflags |= TW_WASI_PREVIEW1_O_CREAT;
    if ((open_flags & TW_WASI02_OPEN_DIRECTORY) != 0u)
        oflags |= TW_WASI_PREVIEW1_O_DIRECTORY;
    if ((open_flags & TW_WASI02_OPEN_EXCLUSIVE) != 0u)
        oflags |= TW_WASI_PREVIEW1_O_EXCL;
    if ((open_flags & TW_WASI02_OPEN_TRUNCATE) != 0u)
        oflags |= TW_WASI_PREVIEW1_O_TRUNC;

    if ((descriptor_flags & TW_WASI02_DESCRIPTOR_READ) != 0u) {
        rights_base |= TURBOWASM_WASI_RIGHT_FD_READ |
                       TURBOWASM_WASI_RIGHT_FD_SEEK |
                       TURBOWASM_WASI_RIGHT_FD_TELL;
    }
    if ((descriptor_flags & TW_WASI02_DESCRIPTOR_WRITE) != 0u) {
        rights_base |= TURBOWASM_WASI_RIGHT_FD_WRITE |
                       TURBOWASM_WASI_RIGHT_FD_SEEK |
                       TURBOWASM_WASI_RIGHT_FD_TELL;
    }

    if ((descriptor_flags & TW_WASI02_DESCRIPTOR_FILE_SYNC) != 0u)
        fdflags |= TW_WASI_PREVIEW1_FDFLAG_SYNC;
    if ((descriptor_flags & TW_WASI02_DESCRIPTOR_DATA_SYNC) != 0u)
        fdflags |= TW_WASI_PREVIEW1_FDFLAG_DSYNC;
    if ((descriptor_flags &
         TW_WASI02_DESCRIPTOR_REQUESTED_WRITE_SYNC) != 0u)
        fdflags |= TW_WASI_PREVIEW1_FDFLAG_RSYNC;

    if ((open_flags & TW_WASI02_OPEN_DIRECTORY) != 0u ||
        (descriptor_flags &
         TW_WASI02_DESCRIPTOR_MUTATE_DIRECTORY) != 0u) {
        rights_base |= TURBOWASM_WASI_RIGHT_FD_READDIR |
                       TURBOWASM_WASI_RIGHT_PATH_OPEN |
                       TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET;
        if ((descriptor_flags &
             TW_WASI02_DESCRIPTOR_MUTATE_DIRECTORY) != 0u) {
            rights_base |=
                TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
                TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
                TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
        }
        rights_inheriting = rights_base;
    }

    error = turbowasm_wasi_fs_path_open(
        filesystem->filesystem,
        base.guest_fd,
        dirflags,
        path.data,
        path.size,
        oflags,
        rights_base,
        rights_inheriting,
        fdflags,
        &guest_fd);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return make_wasi02_error_result(error, out_result);

    if (!turbowasm_wasi_fs_descriptor_info_get(
            filesystem->filesystem, guest_fd, &child)) {
        (void)turbowasm_wasi_fs_close_fd(
            filesystem->filesystem, guest_fd);
        return TURBOWASM_TRAPPED;
    }

    status = descriptor_resource_create_owned(
        filesystem, child.descriptor, &resource);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi_fs_close_fd(
            filesystem->filesystem, guest_fd);
        return status;
    }

    status = make_wasi02_resource_success(resource, out_result);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi02_filesystem_descriptor_drop(
            filesystem, resource);
        return status;
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

static bool wasi02_fs_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_filesystem *filesystem =
        (turbowasm_wasi02_filesystem *)context;

    if (!component_name_is(
            instance_name,
            "wasi:filesystem/preopens@0.2.8") ||
        !component_name_is(
            function_name,
            "get-directories"))
        return false;

    return bind_preopens_shape(
        filesystem, graph, function_type);
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

    (void)arguments;

    if (filesystem == NULL || !filesystem->initialized ||
        out_result == NULL || trap == NULL ||
        argument_count != 0u ||
        !wasi02_fs_can_bind(
            context, instance_name, function_name,
            graph, function_type))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;
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
