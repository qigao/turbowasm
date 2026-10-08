#include "wasi_fs_private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_wasi_fs_slot {
    bool active;
    bool reserved;
    uint32_t leases;
    uint8_t claims;
    uint8_t type;
    uint16_t flags;
    turbowasm_wasi_descriptor_ops ops;
    bool preopen;
    uint32_t generation;
    uint32_t guest_fd;
    turbowasm_wasi_fs_file file;
    char *guest_path;
    uint64_t rights_base;
    uint64_t rights_inheriting;
} turbowasm_wasi_fs_slot;

typedef struct turbowasm_wasi_fs_impl {
    turbowasm_wasi_fs_provider provider;
    turbowasm_wasi_fs_slot *slots;
    uint32_t capacity;
    uint32_t active_count;
} turbowasm_wasi_fs_impl;

static turbowasm_wasi_fs_impl *turbowasm_wasi_fs_impl_mut(
    turbowasm_wasi_fs *filesystem) {
    return filesystem == NULL
        ? NULL
        : (turbowasm_wasi_fs_impl *)filesystem->impl;
}

static const turbowasm_wasi_fs_impl *turbowasm_wasi_fs_impl_get(
    const turbowasm_wasi_fs *filesystem) {
    return filesystem == NULL
        ? NULL
        : (const turbowasm_wasi_fs_impl *)filesystem->impl;
}

static turbowasm_wasi_fs_slot *turbowasm_wasi_fs_find_fd(
    turbowasm_wasi_fs_impl *impl,
    uint32_t guest_fd) {
    uint32_t index;

    if (impl == NULL)
        return NULL;
    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].active &&
            impl->slots[index].guest_fd == guest_fd)
            return &impl->slots[index];
    }
    return NULL;
}

static bool tw_guest_fd_occupied(const turbowasm_wasi_fs_impl *p, uint32_t fd) {
    for (uint32_t i=0; i<p->capacity; ++i)
        if ((p->slots[i].active || p->slots[i].reserved) && p->slots[i].guest_fd == fd) return true;
    return false;
}

static const turbowasm_wasi_fs_slot *
turbowasm_wasi_fs_find_fd_const(
    const turbowasm_wasi_fs_impl *impl,
    uint32_t guest_fd) {
    uint32_t index;

    if (impl == NULL)
        return NULL;
    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].active &&
            impl->slots[index].guest_fd == guest_fd)
            return &impl->slots[index];
    }
    return NULL;
}

static turbowasm_wasi_fs_slot *
turbowasm_wasi_fs_find_descriptor(
    turbowasm_wasi_fs_impl *impl,
    turbowasm_wasi_fs_descriptor descriptor) {
    turbowasm_wasi_fs_slot *slot;

    if (impl == NULL || descriptor.generation == 0u ||
        descriptor.slot >= impl->capacity)
        return NULL;

    slot = &impl->slots[descriptor.slot];
    if (!slot->active ||
        slot->generation != descriptor.generation)
        return NULL;
    return slot;
}

turbowasm_status turbowasm_wasi_fs_init(
    turbowasm_wasi_fs *filesystem,
    const turbowasm_wasi_fs_config *config) {
    turbowasm_wasi_fs_impl *impl;

    if (filesystem == NULL || filesystem->impl != NULL ||
        config == NULL ||
        config->descriptor_capacity == 0u ||
        config->descriptor_capacity > UINT32_MAX - 3u ||
        config->descriptor_capacity >
            SIZE_MAX / sizeof(turbowasm_wasi_fs_slot) ||
        config->provider.close == NULL ||
        config->provider.read == NULL ||
        config->provider.write == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi_fs_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->slots = (turbowasm_wasi_fs_slot *)calloc(
        config->descriptor_capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->provider = config->provider;
    impl->capacity = (uint32_t)config->descriptor_capacity;
    filesystem->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi_fs_destroy(
    turbowasm_wasi_fs *filesystem) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);

    if (filesystem == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl == NULL)
        return TURBOWASM_OK;
    for (uint32_t i = 0; i < impl->capacity; ++i)
        if (impl->slots[i].leases || impl->slots[i].reserved)
            return TURBOWASM_INVALID_ARGUMENT;
    if (impl->active_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    free(impl->slots);
    free(impl);
    filesystem->impl = NULL;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi_fs_bind_descriptor(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_file file,
    bool preopen,
    const char *guest_path,
    turbowasm_wasi_fs_descriptor *out_descriptor) {
    return turbowasm_wasi_fs_bind_descriptor_with_rights(
        filesystem,
        guest_fd,
        file,
        preopen,
        guest_path,
        UINT64_MAX,
        UINT64_MAX,
        out_descriptor);
}

turbowasm_status turbowasm_wasi_fs_bind_descriptor_with_rights(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_file file,
    bool preopen,
    const char *guest_path,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    turbowasm_wasi_fs_descriptor *out_descriptor) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot = NULL;
    char *path_copy = NULL;
    uint32_t index;
    uint32_t generation;

    if (impl == NULL || out_descriptor == NULL ||
        file.generation == 0u ||
        (preopen &&
         (guest_path == NULL || guest_path[0] == '\0')) ||
        (!preopen && guest_path != NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    *out_descriptor = (turbowasm_wasi_fs_descriptor){0};

    if (tw_guest_fd_occupied(impl, guest_fd))
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->active_count >= impl->capacity)
        return TURBOWASM_OUT_OF_MEMORY;

    for (index = 0u; index < impl->capacity; ++index) {
        if (!impl->slots[index].active && !impl->slots[index].reserved &&
            !impl->slots[index].leases && impl->slots[index].generation != UINT32_MAX) {
            slot = &impl->slots[index];
            break;
        }
    }
    if (slot == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if (preopen) {
        size_t length = strlen(guest_path);
        if (length == SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        path_copy = (char *)malloc(length + 1u);
        if (path_copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(path_copy, guest_path, length + 1u);
    }

    generation = slot->generation + 1u;

    slot->active = true;
    slot->preopen = preopen;
    slot->generation = generation;
    slot->guest_fd = guest_fd;
    slot->file = file;
    slot->ops = (turbowasm_wasi_descriptor_ops){0};
    slot->ops.size = sizeof(slot->ops);
    slot->ops.api_version = 1;
    slot->ops.file = impl->provider;
    slot->type = preopen ? TURBOWASM_WASI_FILETYPE_DIRECTORY : TURBOWASM_WASI_FILETYPE_UNKNOWN;
    slot->flags = 0;
    slot->guest_path = path_copy;
    slot->rights_base = rights_base;
    slot->rights_inheriting = rights_inheriting;
    ++impl->active_count;

    out_descriptor->slot = (uint32_t)(slot - impl->slots);
    out_descriptor->generation = generation;
    return TURBOWASM_OK;
}

bool turbowasm_wasi_fs_descriptor_info_get(
    const turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    const turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_get(filesystem);
    const turbowasm_wasi_fs_slot *slot;

    if (impl == NULL || out_info == NULL)
        return false;

    slot = turbowasm_wasi_fs_find_fd_const(impl, guest_fd);
    if (slot == NULL)
        return false;

    out_info->descriptor.slot =
        (uint32_t)(slot - impl->slots);
    out_info->descriptor.generation = slot->generation;
    out_info->guest_fd = slot->guest_fd;
    out_info->file = slot->file;
    out_info->preopen = slot->preopen;
    out_info->guest_path = slot->guest_path;
    out_info->rights_base = slot->rights_base;
    out_info->rights_inheriting = slot->rights_inheriting;
    return true;
}

bool turbowasm_wasi_fs_retained_descriptor_info_get(
    const turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_descriptor descriptor,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    const turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_get(filesystem);
    const turbowasm_wasi_fs_slot *slot;

    if (impl == NULL || out_info == NULL ||
        descriptor.generation == 0u ||
        descriptor.slot >= impl->capacity)
        return false;

    slot = &impl->slots[descriptor.slot];
    if (!slot->active ||
        slot->generation != descriptor.generation)
        return false;

    out_info->descriptor = descriptor;
    out_info->guest_fd = slot->guest_fd;
    out_info->file = slot->file;
    out_info->preopen = slot->preopen;
    out_info->guest_path = slot->guest_path;
    out_info->rights_base = slot->rights_base;
    out_info->rights_inheriting = slot->rights_inheriting;
    return true;
}

size_t turbowasm_wasi_fs_preopen_count(
    const turbowasm_wasi_fs *filesystem) {
    const turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_get(filesystem);
    uint32_t index;
    size_t count = 0u;

    if (impl == NULL)
        return 0u;

    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].active &&
            impl->slots[index].preopen)
            ++count;
    }
    return count;
}

bool turbowasm_wasi_fs_preopen_at(
    const turbowasm_wasi_fs *filesystem,
    size_t preopen_index,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    const turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_get(filesystem);
    uint32_t index;
    size_t ordinal = 0u;

    if (impl == NULL || out_info == NULL)
        return false;

    for (index = 0u; index < impl->capacity; ++index) {
        const turbowasm_wasi_fs_slot *slot =
            &impl->slots[index];

        if (!slot->active || !slot->preopen)
            continue;
        if (ordinal++ != preopen_index)
            continue;

        out_info->descriptor.slot = index;
        out_info->descriptor.generation = slot->generation;
        out_info->guest_fd = slot->guest_fd;
        out_info->file = slot->file;
        out_info->preopen = true;
        out_info->guest_path = slot->guest_path;
        out_info->rights_base = slot->rights_base;
        out_info->rights_inheriting = slot->rights_inheriting;
        return true;
    }

    return false;
}

uint32_t turbowasm_wasi_fs_close_descriptor(
    turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_descriptor descriptor) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_descriptor(impl, descriptor);
    uint32_t error;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;

    error = slot->ops.file.close(
        slot->ops.file.context, slot->file);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    free(slot->guest_path);
    slot->guest_path = NULL;
    slot->active = false;
    slot->preopen = false;
    slot->guest_fd = 0u;
    if (!slot->leases) slot->file = (turbowasm_wasi_fs_file){0};
    slot->rights_base = 0u;
    slot->rights_inheriting = 0u;
    --impl->active_count;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

turbowasm_status turbowasm_wasi_fs_bind_next_descriptor(
    turbowasm_wasi_fs *filesystem,
    turbowasm_wasi_fs_file file,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    turbowasm_wasi_fs_descriptor *out_descriptor,
    uint32_t *out_guest_fd) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    uint32_t offset;

    if (impl == NULL || out_descriptor == NULL ||
        out_guest_fd == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *out_descriptor = (turbowasm_wasi_fs_descriptor){0};
    *out_guest_fd = 0u;

    if (impl->active_count >= impl->capacity)
        return TURBOWASM_OUT_OF_MEMORY;

    for (offset = 0u; offset <= impl->capacity; ++offset) {
        uint32_t candidate = 3u + offset;
        if (!tw_guest_fd_occupied(impl, candidate)) {
            turbowasm_status status =
                turbowasm_wasi_fs_bind_descriptor_with_rights(
                    filesystem,
                    candidate,
                    file,
                    false,
                    NULL,
                    rights_base,
                    rights_inheriting,
                    out_descriptor);
            if (status == TURBOWASM_OK)
                *out_guest_fd = candidate;
            return status;
        }
    }

    return TURBOWASM_OUT_OF_MEMORY;
}

uint32_t turbowasm_wasi_fs_path_open(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    uint32_t *out_guest_fd) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *directory;
    turbowasm_wasi_fs_file opened = {0};
    turbowasm_wasi_fs_descriptor descriptor = {0};
    turbowasm_status bind_status;
    uint32_t error;

    if (impl == NULL || out_guest_fd == NULL ||
        (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_guest_fd = 0u;
    directory = turbowasm_wasi_fs_find_fd(
        impl, directory_fd);
    if (directory == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((directory->rights_base &
         TURBOWASM_WASI_RIGHT_PATH_OPEN) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    if ((rights_base & ~directory->rights_inheriting) != 0u ||
        (rights_inheriting & ~directory->rights_inheriting) != 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    if (directory->ops.file.path_open == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    error = directory->ops.file.path_open(
        directory->ops.file.context,
        directory->file,
        dirflags,
        path,
        path_length,
        oflags,
        rights_base,
        rights_inheriting,
        fdflags,
        &opened);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    bind_status = turbowasm_wasi_fs_bind_next_descriptor(
        filesystem,
        opened,
        rights_base,
        rights_inheriting,
        &descriptor,
        out_guest_fd);
    if (bind_status != TURBOWASM_OK) {
        uint32_t close_error = directory->ops.file.close(
            directory->ops.file.context, opened);
        (void)close_error;
        *out_guest_fd = 0u;
        return bind_status == TURBOWASM_OUT_OF_MEMORY
            ? TURBOWASM_WASI_ERRNO_MFILE
            : TURBOWASM_WASI_ERRNO_IO;
    }

    turbowasm_wasi_fs_slot *child = turbowasm_wasi_fs_find_descriptor(impl, descriptor);
    child->ops = directory->ops;
    child->type = TURBOWASM_WASI_FILETYPE_UNKNOWN;
    child->flags = (uint16_t)fdflags;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

uint32_t turbowasm_wasi_fs_close_fd(
    turbowasm_wasi_fs *filesystem,
    uint32_t guest_fd) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, guest_fd);
    turbowasm_wasi_fs_descriptor descriptor;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;

    descriptor.slot = (uint32_t)(slot - impl->slots);
    descriptor.generation = slot->generation;
    return turbowasm_wasi_fs_close_descriptor(
        filesystem, descriptor);
}

uint32_t turbowasm_wasi_fs_fd_read(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    turbowasm_wasi_fs *filesystem =
        (turbowasm_wasi_fs *)context;
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_READ) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    return slot->ops.file.read(
        slot->ops.file.context,
        slot->file,
        buffers,
        buffer_count,
        out_read);
}


uint32_t turbowasm_wasi_fs_fd_seek(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_offset = 0u;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_SEEK) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (whence != TURBOWASM_WASI_WHENCE_SET &&
        whence != TURBOWASM_WASI_WHENCE_CUR &&
        whence != TURBOWASM_WASI_WHENCE_END)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (slot->ops.file.seek == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return slot->ops.file.seek(
        slot->ops.file.context,
        slot->file,
        offset,
        whence,
        out_offset);
}

uint32_t turbowasm_wasi_fs_fd_tell(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    uint64_t *out_offset) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_offset = 0u;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         (TURBOWASM_WASI_RIGHT_FD_TELL |
          TURBOWASM_WASI_RIGHT_FD_SEEK)) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (slot->ops.file.tell == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return slot->ops.file.tell(
        slot->ops.file.context,
        slot->file,
        out_offset);
}

uint32_t turbowasm_wasi_fs_fd_stat(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_stat = (turbowasm_wasi_fs_stat){0};
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (slot->ops.file.stat == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return slot->ops.file.stat(
        slot->ops.file.context,
        slot->file,
        out_stat);
}

uint32_t turbowasm_wasi_fs_path_stat(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *directory =
        turbowasm_wasi_fs_find_fd(impl, directory_fd);

    if (out_stat == NULL ||
        (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_stat = (turbowasm_wasi_fs_stat){0};
    if (directory == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((directory->rights_base &
         TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (directory->ops.file.path_stat == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return directory->ops.file.path_stat(
        directory->ops.file.context,
        directory->file,
        lookup_flags,
        path,
        path_length,
        out_stat);
}

static uint32_t turbowasm_wasi_fs_path_mutate(
    turbowasm_wasi_fs_impl *impl,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length,
    uint64_t required_right,
    unsigned operation) {
    turbowasm_wasi_fs_path_mutation_fn mutation;
    turbowasm_wasi_fs_slot *directory =
        turbowasm_wasi_fs_find_fd(impl, directory_fd);

    if (path_length != 0u && path == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (directory == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((directory->rights_base & required_right) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    mutation = operation == 0 ? directory->ops.file.path_create_directory :
        operation == 1 ? directory->ops.file.path_remove_directory : directory->ops.file.path_unlink_file;
    if (mutation == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return mutation(
        directory->ops.file.context,
        directory->file,
        path,
        path_length);
}

uint32_t turbowasm_wasi_fs_path_create_directory(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);

    return turbowasm_wasi_fs_path_mutate(
        impl,
        directory_fd,
        path,
        path_length,
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY,
        0);
}

uint32_t turbowasm_wasi_fs_path_remove_directory(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);

    return turbowasm_wasi_fs_path_mutate(
        impl,
        directory_fd,
        path,
        path_length,
        TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY,
        1);
}

uint32_t turbowasm_wasi_fs_path_unlink_file(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);

    return turbowasm_wasi_fs_path_mutate(
        impl,
        directory_fd,
        path,
        path_length,
        TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE,
        2);
}

uint32_t turbowasm_wasi_fs_fd_readdir(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);
    uint32_t error;

    if (out_entry == NULL || out_has_entry == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_entry = (turbowasm_wasi_fs_dirent){0};
    *out_has_entry = false;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_READDIR) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (slot->ops.file.readdir == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    error = slot->ops.file.readdir(
        slot->ops.file.context,
        slot->file,
        cookie,
        out_entry,
        out_has_entry);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS ||
        !*out_has_entry)
        return error;
    if (out_entry->name_length >
        TURBOWASM_WASI_FS_DIRENT_NAME_MAX)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    if (out_entry->file_type >
        TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (out_entry->next_cookie == cookie)
        return TURBOWASM_WASI_ERRNO_INVAL;

    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

uint32_t turbowasm_wasi_fs_fd_write(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    turbowasm_wasi_fs *filesystem =
        (turbowasm_wasi_fs *)context;
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_WRITE) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    return slot->ops.file.write(
        slot->ops.file.context,
        slot->file,
        buffers,
        buffer_count,
        out_written);
}

static turbowasm_wasi_fs_slot *tw_fd_identity(turbowasm_wasi_fs_impl *p,
    turbowasm_wasi_fs_descriptor d) {
    if (!p || !d.generation || d.slot >= p->capacity) return NULL;
    turbowasm_wasi_fs_slot *s = &p->slots[d.slot];
    return s->generation == d.generation ? s : NULL;
}

bool tw_wasi_fd_is_socket(turbowasm_wasi_fs *fs, uint32_t fd) {
    turbowasm_wasi_fs_slot *s=turbowasm_wasi_fs_find_fd(turbowasm_wasi_fs_impl_mut(fs), fd);
    return s && s->ops.recv != NULL;
}

uint32_t tw_wasi_fd_acquire(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t right,
    tw_wasi_fd_lease *out) {
    turbowasm_wasi_fs_impl *p = turbowasm_wasi_fs_impl_mut(fs);
    turbowasm_wasi_fs_slot *s = turbowasm_wasi_fs_find_fd(p, fd);
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (tw_wasi_fd_lease){0};
    if (!s) return TURBOWASM_WASI_ERRNO_BADF;
    if ((s->rights_base & right) != right) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (s->leases == UINT32_MAX) return TURBOWASM_WASI_ERRNO_BUSY;
    uint8_t type = s->type;
    if (!type && s->ops.file.stat) {
        turbowasm_wasi_fs_stat stat = {0};
        uint32_t e = s->ops.file.stat(s->ops.file.context, s->file, &stat);
        if (e) return e;
        type = stat.file_type;
    }
    if (s->ops.retain) {
        uint32_t e = s->ops.retain(s->ops.file.context, s->file);
        if (e) return e;
    }
    ++s->leases;
    out->descriptor = (turbowasm_wasi_fs_descriptor){(uint32_t)(s-p->slots), s->generation};
    out->file = s->file; out->ops = s->ops;
    out->rights_base = s->rights_base; out->rights_inheriting = s->rights_inheriting;
    out->flags = s->flags; out->type = type; out->held = true;
    return 0;
}

void tw_wasi_fd_release(turbowasm_wasi_fs *fs, tw_wasi_fd_lease *lease) {
    if (!lease || !lease->held) return;
    turbowasm_wasi_fs_slot *s = tw_fd_identity(turbowasm_wasi_fs_impl_mut(fs), lease->descriptor);
    if (lease->claimed && lease->ops.finish)
        lease->ops.finish(lease->ops.file.context, lease->file, lease->direction);
    if (lease->ops.release) lease->ops.release(lease->ops.file.context, lease->file);
    if (s) {
        if (lease->claimed) s->claims &= (uint8_t)~(1u << lease->direction);
        --s->leases;
        if (!s->active && !s->leases) s->file = (turbowasm_wasi_fs_file){0};
    }
    *lease = (tw_wasi_fd_lease){0};
}

uint32_t tw_wasi_fd_check(turbowasm_wasi_fs *fs, const tw_wasi_fd_lease *lease, uint64_t rights) {
    turbowasm_wasi_fs_slot *s = tw_fd_identity(turbowasm_wasi_fs_impl_mut(fs), lease->descriptor);
    if (!s || !s->active) return TURBOWASM_WASI_ERRNO_BADF;
    return (s->rights_base & rights) == rights ? 0 : TURBOWASM_WASI_ERRNO_NOTCAPABLE;
}

uint32_t tw_wasi_fd_claim(turbowasm_wasi_fs *fs, tw_wasi_fd_lease *lease, uint8_t direction) {
    turbowasm_wasi_fs_slot *s = tw_fd_identity(turbowasm_wasi_fs_impl_mut(fs), lease->descriptor);
    if (!s || !s->active) return TURBOWASM_WASI_ERRNO_BADF;
    uint8_t bit = (uint8_t)(1u << direction);
    if (s->claims & bit) return TURBOWASM_WASI_ERRNO_BUSY;
    s->claims |= bit; lease->claimed = true; lease->direction = direction;
    return 0;
}

uint32_t tw_wasi_fd_set_flags(turbowasm_wasi_fs *fs, uint32_t fd, uint16_t flags) {
    turbowasm_wasi_fs_slot *s = turbowasm_wasi_fs_find_fd(turbowasm_wasi_fs_impl_mut(fs), fd);
    if (!s) return TURBOWASM_WASI_ERRNO_BADF;
    if (!(s->rights_base & TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS)) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (flags & ~TURBOWASM_WASI_FDFLAG_NONBLOCK) return TURBOWASM_WASI_ERRNO_NOTSUP;
    if (!s->ops.recv && flags) return TURBOWASM_WASI_ERRNO_NOTSUP;
    s->flags = flags; return 0;
}

uint32_t tw_wasi_fd_set_rights(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t base, uint64_t inheriting) {
    turbowasm_wasi_fs_slot *s = turbowasm_wasi_fs_find_fd(turbowasm_wasi_fs_impl_mut(fs), fd);
    if (!s) return TURBOWASM_WASI_ERRNO_BADF;
    if ((base & ~s->rights_base) || (inheriting & ~s->rights_inheriting)) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    s->rights_base = base; s->rights_inheriting = inheriting; return 0;
}

uint32_t tw_wasi_fd_reserve(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor *out, uint32_t *fd) {
    turbowasm_wasi_fs_impl *p = turbowasm_wasi_fs_impl_mut(fs);
    if (!p || !out || !fd) return TURBOWASM_WASI_ERRNO_INVAL;
    turbowasm_wasi_fs_slot *s = NULL;
    for (uint32_t i = 0; i < p->capacity; ++i)
        if (!p->slots[i].active && !p->slots[i].reserved && !p->slots[i].leases &&
            p->slots[i].generation != UINT32_MAX) { s = &p->slots[i]; break; }
    if (!s) return TURBOWASM_WASI_ERRNO_MFILE;
    uint32_t candidate;
    for (candidate = 3; candidate <= p->capacity + 3; ++candidate) {
        bool occupied = false;
        for (uint32_t i = 0; i < p->capacity; ++i)
            if ((p->slots[i].active || p->slots[i].reserved) && p->slots[i].guest_fd == candidate)
                occupied = true;
        if (!occupied) break;
    }
    s->reserved = true; s->guest_fd = candidate; ++s->generation;
    *out = (turbowasm_wasi_fs_descriptor){(uint32_t)(s-p->slots), s->generation}; *fd = candidate;
    return 0;
}

void tw_wasi_fd_abort(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d) {
    turbowasm_wasi_fs_slot *s = tw_fd_identity(turbowasm_wasi_fs_impl_mut(fs), d);
    if (s && s->reserved) { s->reserved = false; s->guest_fd = 0; }
}

void tw_wasi_fd_publish(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d,
    turbowasm_wasi_fs_file file, const tw_wasi_fd_lease *parent, uint16_t flags) {
    turbowasm_wasi_fs_impl *p = turbowasm_wasi_fs_impl_mut(fs);
    turbowasm_wasi_fs_slot *s = tw_fd_identity(p, d);
    s->reserved = false; s->active = true; s->preopen = false; s->guest_path = NULL;
    s->file = file; s->ops = parent->ops; s->type = TURBOWASM_WASI_FILETYPE_SOCKET_STREAM;
    uint64_t stream_rights = TURBOWASM_WASI_RIGHT_FD_READ | TURBOWASM_WASI_RIGHT_FD_WRITE |
        TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS | TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE | TURBOWASM_WASI_RIGHT_SOCK_SHUTDOWN;
    turbowasm_wasi_fs_slot *current_parent = tw_fd_identity(p, parent->descriptor);
    s->flags = flags; s->rights_base = current_parent->rights_inheriting & stream_rights;
    s->rights_inheriting = 0; ++p->active_count;
}

uint32_t tw_wasi_fd_ready(turbowasm_wasi_fs *fs, const tw_wasi_fd_lease *lease, uint8_t direction,
    turbowasm_wasi_readiness *out) {
    *out = (turbowasm_wasi_readiness){0};
    uint32_t e = tw_wasi_fd_check(fs, lease, TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE);
    if (e) return e;
    if (lease->ops.ready) return lease->ops.ready(lease->ops.file.context, lease->file, direction, out);
    if (lease->type == TURBOWASM_WASI_FILETYPE_REGULAR_FILE || lease->type == TURBOWASM_WASI_FILETYPE_DIRECTORY) {
        out->ready = true;
        if (direction == TURBOWASM_WASI_EVENT_FD_READ && lease->ops.file.stat) {
            turbowasm_wasi_fs_stat stat = {0};
            e = lease->ops.file.stat(lease->ops.file.context, lease->file, &stat);
            if (e) return e;
            uint64_t offset = 0;
            if (lease->ops.file.tell) {
                e = lease->ops.file.tell(lease->ops.file.context, lease->file, &offset);
                if (e) return e;
            }
            out->bytes = stat.size > offset ? stat.size-offset : 0;
        }
        return 0;
    }
    return TURBOWASM_WASI_ERRNO_NOTSUP;
}

turbowasm_status turbowasm_wasi_fs_bind_socket_move(turbowasm_wasi_fs *fs, uint32_t fd,
    const turbowasm_wasi_descriptor_ops *ops, turbowasm_wasi_fs_file *file,
    uint8_t type, uint16_t flags, uint64_t base, uint64_t inheriting,
    turbowasm_wasi_fs_descriptor *out) {
    if (!ops || ops->size != sizeof(*ops) || ops->api_version != 1 || !file || !file->generation ||
        !ops->file.close || !ops->file.read || !ops->file.write || !ops->retain || !ops->release ||
        !ops->ready || !ops->recv || !ops->send || !ops->shutdown ||
        (type != TURBOWASM_WASI_FILETYPE_SOCKET_STREAM && type != TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM) ||
        (flags & ~TURBOWASM_WASI_FDFLAG_NONBLOCK)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = turbowasm_wasi_fs_bind_descriptor_with_rights(fs, fd, *file,
        false, NULL, base, inheriting, out);
    if (status != TURBOWASM_OK) return status;
    turbowasm_wasi_fs_slot *s = turbowasm_wasi_fs_find_descriptor(turbowasm_wasi_fs_impl_mut(fs), *out);
    s->ops = *ops; s->type = type; s->flags = flags; *file = (turbowasm_wasi_fs_file){0};
    return TURBOWASM_OK;
}
