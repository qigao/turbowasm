#include <turbowasm/wasi_fs.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_wasi_fs_slot {
    bool active;
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

    if (turbowasm_wasi_fs_find_fd(impl, guest_fd) != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->active_count >= impl->capacity)
        return TURBOWASM_OUT_OF_MEMORY;

    for (index = 0u; index < impl->capacity; ++index) {
        if (!impl->slots[index].active) {
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
    if (generation == 0u)
        generation = 1u;

    slot->active = true;
    slot->preopen = preopen;
    slot->generation = generation;
    slot->guest_fd = guest_fd;
    slot->file = file;
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

    error = impl->provider.close(
        impl->provider.context, slot->file);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    free(slot->guest_path);
    slot->guest_path = NULL;
    slot->active = false;
    slot->preopen = false;
    slot->guest_fd = 0u;
    slot->file = (turbowasm_wasi_fs_file){0};
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
        if (turbowasm_wasi_fs_find_fd(impl, candidate) == NULL) {
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
    if (!directory->preopen ||
        (directory->rights_base &
         TURBOWASM_WASI_RIGHT_PATH_OPEN) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    if ((rights_base & ~directory->rights_inheriting) != 0u ||
        (rights_inheriting & ~directory->rights_inheriting) != 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    if (impl->provider.path_open == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    error = impl->provider.path_open(
        impl->provider.context,
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
        uint32_t close_error = impl->provider.close(
            impl->provider.context, opened);
        (void)close_error;
        *out_guest_fd = 0u;
        return bind_status == TURBOWASM_OUT_OF_MEMORY
            ? TURBOWASM_WASI_ERRNO_MFILE
            : TURBOWASM_WASI_ERRNO_IO;
    }

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

    return impl->provider.read(
        impl->provider.context,
        slot->file,
        buffers,
        buffer_count,
        out_read);
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

    return impl->provider.write(
        impl->provider.context,
        slot->file,
        buffers,
        buffer_count,
        out_written);
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

    if (impl == NULL || out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (whence > TURBOWASM_WASI_WHENCE_END)
        return TURBOWASM_WASI_ERRNO_INVAL;

    /*
     * Preview1 permits fd_seek(fd, 0, CUR) with FD_TELL alone. Prefer the
     * provider's explicit tell callback for that restricted operation.
     */
    if (offset == 0 &&
        whence == TURBOWASM_WASI_WHENCE_CUR &&
        (slot->rights_base & TURBOWASM_WASI_RIGHT_FD_SEEK) == 0u) {
        if ((slot->rights_base & TURBOWASM_WASI_RIGHT_FD_TELL) == 0u)
            return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
        if (impl->provider.tell != NULL) {
            return impl->provider.tell(
                impl->provider.context,
                slot->file,
                out_offset);
        }
        if (impl->provider.seek != NULL) {
            return impl->provider.seek(
                impl->provider.context,
                slot->file,
                0,
                TURBOWASM_WASI_WHENCE_CUR,
                out_offset);
        }
        return TURBOWASM_WASI_ERRNO_NOSYS;
    }

    if ((slot->rights_base & TURBOWASM_WASI_RIGHT_FD_SEEK) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (impl->provider.seek == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    return impl->provider.seek(
        impl->provider.context,
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

    if (impl == NULL || out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base & TURBOWASM_WASI_RIGHT_FD_TELL) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (impl->provider.tell != NULL) {
        return impl->provider.tell(
            impl->provider.context,
            slot->file,
            out_offset);
    }
    if (impl->provider.seek != NULL) {
        return impl->provider.seek(
            impl->provider.context,
            slot->file,
            0,
            TURBOWASM_WASI_WHENCE_CUR,
            out_offset);
    }
    return TURBOWASM_WASI_ERRNO_NOSYS;
}

uint32_t turbowasm_wasi_fs_fd_filestat_get(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_fs_impl *impl =
        turbowasm_wasi_fs_impl_mut(filesystem);
    turbowasm_wasi_fs_slot *slot =
        turbowasm_wasi_fs_find_fd(impl, fd);

    if (impl == NULL || out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if ((slot->rights_base &
         TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET) == 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    if (impl->provider.stat == NULL)
        return TURBOWASM_WASI_ERRNO_NOSYS;

    *out_stat = (turbowasm_wasi_fs_stat){0};
    return impl->provider.stat(
        impl->provider.context,
        slot->file,
        out_stat);
}

