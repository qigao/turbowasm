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
        config->descriptor_capacity > UINT32_MAX ||
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
    --impl->active_count;
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

    return impl->provider.write(
        impl->provider.context,
        slot->file,
        buffers,
        buffer_count,
        out_written);
}
