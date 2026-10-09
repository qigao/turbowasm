#include <turbowasm/wasi_host_fs.h>

#include <cmeta_fs.h>
#include <salts/thread.h>
#include <tstr.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EOVERFLOW
#define EOVERFLOW ERANGE
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG ERANGE
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY EEXIST
#endif

enum {
    TURBOWASM_HOST_FS_OFLAGS_CREAT = 1u,
    TURBOWASM_HOST_FS_OFLAGS_DIRECTORY = 2u,
    TURBOWASM_HOST_FS_OFLAGS_EXCL = 4u,
    TURBOWASM_HOST_FS_OFLAGS_TRUNC = 8u,
    TURBOWASM_HOST_FS_FDFLAGS_APPEND = 1u,
    TURBOWASM_HOST_FS_LOOKUPFLAGS_SYMLINK_FOLLOW = 1u
};

typedef struct turbowasm_wasi_host_fs_slot {
    bool used;
    bool reserved, closing;
    uint32_t pins;
    cmeta_mutex_t io_mutex;
    bool directory;
    uint32_t generation;
    cmeta_fs_root_file_t *file;
    cmeta_fs_root_dir_t *dir;
    cmeta_fs_stat_t stat_snapshot;
    char *path;
} turbowasm_wasi_host_fs_slot;

typedef struct turbowasm_wasi_host_fs_impl {
    cmeta_fs_root_t *root;
    cmeta_fs_root_dir_t *root_dir;
    turbowasm_wasi_host_fs_slot *slots;
    char *path_storage;
    size_t slot_capacity;
    size_t path_capacity;
    bool root_open;
    bool root_closing, renaming;
    uint32_t root_pins, opening;
    cmeta_mutex_t mutex, root_mutex;
} turbowasm_wasi_host_fs_impl;

typedef struct host_pin {
    turbowasm_wasi_host_fs_slot *slot;
    bool held, serialized;
} host_pin;

static bool host_is_root(turbowasm_wasi_fs_file file) {
    return file.object == UINT64_C(1) && file.generation == 1u;
}

static uint32_t host_errno(int result) {
    int error;

    if (result >= 0)
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    error = -result;

    if (error == EAGAIN) return TURBOWASM_WASI_ERRNO_AGAIN;
    if (error == EBADF) return TURBOWASM_WASI_ERRNO_BADF;
    if (error == EEXIST) return TURBOWASM_WASI_ERRNO_EXIST;
    if (error == EXDEV) return TURBOWASM_WASI_ERRNO_XDEV;
    if (error == EFBIG || error == EOVERFLOW)
        return TURBOWASM_WASI_ERRNO_FBIG;
    if (error == EINTR) return TURBOWASM_WASI_ERRNO_INTR;
    if (error == EINVAL) return TURBOWASM_WASI_ERRNO_INVAL;
    if (error == EISDIR) return TURBOWASM_WASI_ERRNO_ISDIR;
    if (error == EMFILE || error == ENFILE)
        return TURBOWASM_WASI_ERRNO_MFILE;
    if (error == ENAMETOOLONG) return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    if (error == ENOENT) return TURBOWASM_WASI_ERRNO_NOENT;
    if (error == ENOMEM) return TURBOWASM_WASI_ERRNO_NOMEM;
    if (error == ENOSPC) return TURBOWASM_WASI_ERRNO_NOSPC;
    if (error == ENOTDIR) return TURBOWASM_WASI_ERRNO_NOTDIR;
    if (error == ENOTEMPTY) return TURBOWASM_WASI_ERRNO_NOTEMPTY;
    if (error == EACCES || error == EPERM)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
#ifdef ELOOP
    if (error == ELOOP) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
#endif
#ifdef ENOSYS
    if (error == ENOSYS) return TURBOWASM_WASI_ERRNO_NOSYS;
#endif
    return TURBOWASM_WASI_ERRNO_IO;
}

static uint64_t host_us_to_ns(uint64_t value) {
    return value > UINT64_MAX / UINT64_C(1000)
        ? UINT64_MAX
        : value * UINT64_C(1000);
}

static void host_fill_stat(
    const cmeta_fs_stat_t *source,
    turbowasm_wasi_fs_stat *out_stat) {
    *out_stat = (turbowasm_wasi_fs_stat){0};
    out_stat->size = source->size;
    out_stat->accessed_ns = host_us_to_ns(source->atime);
    out_stat->modified_ns = host_us_to_ns(source->mtime);
    out_stat->changed_ns = host_us_to_ns(source->ctime);
    out_stat->timestamp_valid =
        TURBOWASM_WASI_FS_TIME_ACCESSED_VALID |
        TURBOWASM_WASI_FS_TIME_MODIFIED_VALID |
        TURBOWASM_WASI_FS_TIME_CHANGED_VALID;
    if (source->is_symlink)
        out_stat->file_type = TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK;
    else if (source->is_directory)
        out_stat->file_type = TURBOWASM_WASI_FILETYPE_DIRECTORY;
    else if (source->is_file)
        out_stat->file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
    else
        out_stat->file_type = TURBOWASM_WASI_FILETYPE_UNKNOWN;
}

static turbowasm_wasi_host_fs_slot *host_slot_from_file(
    turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file file) {
    size_t index;
    turbowasm_wasi_host_fs_slot *slot;

    if (impl == NULL || file.object < UINT64_C(2))
        return NULL;
    if (file.object - UINT64_C(2) >= impl->slot_capacity)
        return NULL;
    index = (size_t)(file.object - UINT64_C(2));
    slot = &impl->slots[index];
    if (!slot->used || slot->generation != file.generation)
        return NULL;
    return slot;
}

/* Table admission protects native identities even for direct provider callers.
 * Never wait on an I/O mutex while holding the identity table mutex. */
static uint32_t host_pin_file(turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file file, bool serialize, host_pin *pin) {
    if (!impl) return TURBOWASM_WASI_ERRNO_INVAL;
    cmeta_mutex_lock(&impl->mutex);
    turbowasm_wasi_host_fs_slot *slot = host_slot_from_file(impl, file);
    bool root = host_is_root(file);
    uint32_t error = 0;
    if (root ? !impl->root_open : slot == NULL) error = TURBOWASM_WASI_ERRNO_BADF;
    else if (root ? (impl->root_closing || impl->root_pins == UINT32_MAX) :
        (slot->closing || slot->pins == UINT32_MAX)) error = TURBOWASM_WASI_ERRNO_BUSY;
    else {
        if (root) ++impl->root_pins; else ++slot->pins;
        *pin = (host_pin){.slot = slot, .held = true, .serialized = serialize};
    }
    cmeta_mutex_unlock(&impl->mutex);
    if (!error && serialize) cmeta_mutex_lock(slot ? &slot->io_mutex : &impl->root_mutex);
    return error;
}
static uint32_t host_unpin(turbowasm_wasi_host_fs_impl *impl, host_pin *pin, uint32_t error) {
    if (!pin->held) return error;
    if (pin->serialized) cmeta_mutex_unlock(pin->slot ? &pin->slot->io_mutex : &impl->root_mutex);
    cmeta_mutex_lock(&impl->mutex);
    if (pin->slot) --pin->slot->pins; else --impl->root_pins;
    cmeta_mutex_unlock(&impl->mutex);
    pin->held = false;
    return error;
}

static turbowasm_wasi_fs_file host_file_from_slot(
    const turbowasm_wasi_host_fs_impl *impl,
    const turbowasm_wasi_host_fs_slot *slot) {
    turbowasm_wasi_fs_file file = {0};
    size_t index = (size_t)(slot - impl->slots);

    file.object = UINT64_C(2) + (uint64_t)index;
    file.generation = slot->generation;
    return file;
}

static turbowasm_wasi_host_fs_slot *host_reserve_slot(
    turbowasm_wasi_host_fs_impl *impl) {
    size_t index;

    for (index = 0u; index < impl->slot_capacity; ++index) {
        turbowasm_wasi_host_fs_slot *slot = &impl->slots[index];
        uint32_t generation;

        if (slot->used || slot->reserved || slot->generation == UINT32_MAX)
            continue;
        generation = slot->generation + 1u;
        slot->reserved = true;
        slot->directory = false;
        slot->generation = generation;
        slot->file = NULL;
        slot->dir = NULL;
        slot->stat_snapshot = (cmeta_fs_stat_t){0};
        slot->path[0] = '\0';
        return slot;
    }
    return NULL;
}

static void host_release_slot(turbowasm_wasi_host_fs_slot *slot) {
    if (slot == NULL)
        return;
    slot->used = false;
    slot->reserved = slot->closing = false;
    slot->directory = false;
    slot->file = NULL;
    slot->dir = NULL;
    slot->stat_snapshot = (cmeta_fs_stat_t){0};
    slot->path[0] = '\0';
}

static bool host_is_separator(uint8_t byte) {
    return byte == (uint8_t)'/' || byte == (uint8_t)'\\';
}

static uint32_t host_validate_guest_path(
    const uint8_t *path,
    size_t path_length) {
    size_t component_start = 0u;
    size_t index;

    if (path == NULL || path_length == 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (host_is_separator(path[0]))
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    for (index = 0u; index <= path_length; ++index) {
        bool end = index == path_length;
        uint8_t byte = end ? (uint8_t)'/' : path[index];
        size_t component_length;
        size_t component_index;

        if (!end && byte == 0u)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (byte != (uint8_t)'/' && byte != (uint8_t)'\\')
            continue;

        component_length = index - component_start;
        if (component_length == 0u)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (component_length >= SALTS_FS_ROOT_COMPONENT_MAX)
            return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
        if (component_length == 1u &&
            path[component_start] == (uint8_t)'.')
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (component_length == 2u &&
            path[component_start] == (uint8_t)'.' &&
            path[component_start + 1u] == (uint8_t)'.')
            return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
#ifdef _WIN32
        for (component_index = component_start;
             component_index < index;
             ++component_index) {
            if (path[component_index] == (uint8_t)':')
                return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
        }
#else
        (void)component_index;
#endif
        component_start = index + 1u;
    }

    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_directory_base(
    turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const char **out_path,
    size_t *out_length) {
    turbowasm_wasi_host_fs_slot *slot;

    if (impl == NULL || out_path == NULL || out_length == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    if (host_is_root(directory)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        *out_path = "";
        *out_length = 0u;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = host_slot_from_file(impl, directory);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (!slot->directory)
        return TURBOWASM_WASI_ERRNO_NOTDIR;
    *out_path = slot->path;
    *out_length = strlen(slot->path);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_build_path(
    turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length,
    char *out_path) {
    const char *base;
    size_t base_length;
    size_t total;
    uint32_t error;

    if (path_length > impl->path_capacity)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    error = host_validate_guest_path(path, path_length);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;
    error = host_directory_base(
        impl, directory, &base, &base_length);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    total = base_length + (base_length != 0u ? 1u : 0u)
        + path_length;
    if (total > impl->path_capacity)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;

    if (base_length != 0u) {
        memcpy(out_path, base, base_length);
        out_path[base_length] = '/';
        memcpy(out_path + base_length + 1u, path, path_length);
    } else {
        memcpy(out_path, path, path_length);
    }
    out_path[total] = '\0';
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot;

    if (impl == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    cmeta_mutex_lock(&impl->mutex);
    if (host_is_root(file)) {
        if (!impl->root_open) {
            cmeta_mutex_unlock(&impl->mutex);
            return TURBOWASM_WASI_ERRNO_BADF;
        }
        if (impl->root_closing || impl->root_pins) {
            cmeta_mutex_unlock(&impl->mutex);
            return TURBOWASM_WASI_ERRNO_BUSY;
        }
        impl->root_closing = true;
        cmeta_mutex_unlock(&impl->mutex);
        if (impl->root_dir != NULL) {
            /*
             * Salts root-directory close consumes the opaque identity even
             * when the native close reports an error. Propagating that error
             * would make turbowasm_wasi_fs retain a descriptor whose provider
             * identity no longer exists, so HostFS close is ownership-
             * consuming and reports success once the close was attempted.
             */
            (void)cmeta_fs_root_closedir(impl->root_dir);
            impl->root_dir = NULL;
        }
        cmeta_mutex_lock(&impl->mutex);
        impl->root_open = impl->root_closing = false;
        cmeta_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = host_slot_from_file(impl, file);
    if (slot == NULL) {
        cmeta_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_ERRNO_BADF;
    }
    if (slot->closing || slot->pins) {
        cmeta_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_ERRNO_BUSY;
    }
    slot->closing = true;
    cmeta_mutex_unlock(&impl->mutex);

    /*
     * cmeta_fs_root_file_close()/closedir() always consume their opaque
     * object, including native-close failure paths. Release the HostFS slot in
     * the same operation and report success so the upper descriptor table
     * cannot retain a dangling provider identity and retry an unsafe close.
     */
    if (slot->directory)
        (void)cmeta_fs_root_closedir(slot->dir);
    else
        (void)cmeta_fs_root_file_close(slot->file);
    cmeta_mutex_lock(&impl->mutex);
    host_release_slot(slot);
    cmeta_mutex_unlock(&impl->mutex);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_read_admitted(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot =
        host_slot_from_file(impl, file);
    size_t index;
    uint32_t total = 0u;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_read == NULL ||
        (buffer_count != 0u && buffers == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_read = 0u;
    for (index = 0u; index < buffer_count; ++index) {
        int result;

        if ((buffers[index].size != 0u && buffers[index].data == NULL) ||
            buffers[index].size > (size_t)INT_MAX ||
            buffers[index].size > (size_t)(UINT32_MAX - total))
            return TURBOWASM_WASI_ERRNO_INVAL;
        result = cmeta_fs_root_file_read(
            slot->file,
            (char *)buffers[index].data,
            buffers[index].size);
        if (result < 0)
            return host_errno(result);
        total += (uint32_t)result;
        if ((size_t)result != buffers[index].size)
            break;
    }
    *out_read = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_write_admitted(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot =
        host_slot_from_file(impl, file);
    size_t index;
    uint32_t total = 0u;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_written == NULL ||
        (buffer_count != 0u && buffers == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_written = 0u;
    for (index = 0u; index < buffer_count; ++index) {
        int result;

        if ((buffers[index].size != 0u && buffers[index].data == NULL) ||
            buffers[index].size > (size_t)INT_MAX ||
            buffers[index].size > (size_t)(UINT32_MAX - total))
            return TURBOWASM_WASI_ERRNO_INVAL;
        result = cmeta_fs_root_file_write(
            slot->file,
            (const char *)buffers[index].data,
            buffers[index].size);
        if (result < 0)
            return host_errno(result);
        total += (uint32_t)result;
        if ((size_t)result != buffers[index].size)
            break;
    }
    *out_written = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_seek_admitted(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot =
        host_slot_from_file(impl, file);
    int native_whence;
    int64_t result;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    switch (whence) {
        case TURBOWASM_WASI_WHENCE_SET:
            native_whence = SEEK_SET;
            break;
        case TURBOWASM_WASI_WHENCE_CUR:
            native_whence = SEEK_CUR;
            break;
        case TURBOWASM_WASI_WHENCE_END:
            native_whence = SEEK_END;
            break;
        default:
            return TURBOWASM_WASI_ERRNO_INVAL;
    }

    result = cmeta_fs_root_file_seek(slot->file, offset, native_whence);
    if (result < 0)
        return host_errno((int)result);
    *out_offset = (uint64_t)result;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_tell_admitted(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot =
        host_slot_from_file(impl, file);
    int64_t result;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    result = cmeta_fs_root_file_tell(slot->file);
    if (result < 0)
        return host_errno((int)result);
    *out_offset = (uint64_t)result;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_stat_admitted(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot;
    cmeta_fs_stat_t stat_value;
    int result;

    if (impl == NULL || out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (host_is_root(file)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        result = cmeta_fs_root_fstat(impl->root, &stat_value);
        if (result < 0)
            return host_errno(result);
        host_fill_stat(&stat_value, out_stat);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = host_slot_from_file(impl, file);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory) {
        host_fill_stat(&slot->stat_snapshot, out_stat);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    result = cmeta_fs_root_file_stat(slot->file, &stat_value);
    if (result < 0)
        return host_errno(result);
    host_fill_stat(&stat_value, out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static int host_file_flags(
    uint64_t rights_base,
    uint32_t oflags,
    uint32_t fdflags,
    int *out_flags) {
    bool can_read;
    bool can_write;
    int flags;

    if (out_flags == NULL)
        return -EINVAL;
    can_read =
        (rights_base & TURBOWASM_WASI_RIGHT_FD_READ) != 0u;
    can_write =
        (rights_base & TURBOWASM_WASI_RIGHT_FD_WRITE) != 0u;
    if ((oflags & (TURBOWASM_HOST_FS_OFLAGS_CREAT |
                   TURBOWASM_HOST_FS_OFLAGS_TRUNC)) != 0u &&
        !can_write)
        return -EACCES;
    if ((fdflags & TURBOWASM_HOST_FS_FDFLAGS_APPEND) != 0u &&
        !can_write)
        return -EACCES;

    if (can_write)
        flags = can_read ? SALTS_FS_O_RDWR : SALTS_FS_O_WRONLY;
    else
        flags = SALTS_FS_O_RDONLY;
    if ((oflags & TURBOWASM_HOST_FS_OFLAGS_CREAT) != 0u)
        flags |= SALTS_FS_O_CREAT;
    if ((oflags & TURBOWASM_HOST_FS_OFLAGS_TRUNC) != 0u)
        flags |= SALTS_FS_O_TRUNC;
    if ((fdflags & TURBOWASM_HOST_FS_FDFLAGS_APPEND) != 0u)
        flags |= SALTS_FS_O_APPEND;
#ifdef SALTS_FS_ROOT_MUTATION_VERSION
    if ((oflags & TURBOWASM_HOST_FS_OFLAGS_EXCL) != 0u)
        flags |= SALTS_FS_ROOT_O_EXCL;
#endif
    *out_flags = flags;
    return 0;
}

static uint32_t host_path_open_admitted(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    turbowasm_wasi_fs_file *out_file) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    turbowasm_wasi_host_fs_slot *slot;
    char full_path[TURBOWASM_WASI_HOST_FS_PATH_MAX + 1u];
    uint32_t error;
    int result;
    int file_flags = 0;

    (void)rights_inheriting;

    if (impl == NULL || out_file == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_file = (turbowasm_wasi_fs_file){0};
    if ((dirflags & ~TURBOWASM_HOST_FS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u ||
        (oflags & ~(TURBOWASM_HOST_FS_OFLAGS_CREAT |
                    TURBOWASM_HOST_FS_OFLAGS_DIRECTORY |
                    TURBOWASM_HOST_FS_OFLAGS_EXCL |
                    TURBOWASM_HOST_FS_OFLAGS_TRUNC)) != 0u ||
        (fdflags & ~TURBOWASM_HOST_FS_FDFLAGS_APPEND) != 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if ((dirflags & TURBOWASM_HOST_FS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
#ifndef SALTS_FS_ROOT_MUTATION_VERSION
    if ((oflags & TURBOWASM_HOST_FS_OFLAGS_EXCL) != 0u)
        return TURBOWASM_WASI_ERRNO_NOSYS;
#endif

    error = host_build_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    cmeta_mutex_lock(&impl->mutex);
    slot = host_reserve_slot(impl);
    cmeta_mutex_unlock(&impl->mutex);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_MFILE;
    memcpy(slot->path, full_path, strlen(full_path) + 1u);

    if ((oflags & TURBOWASM_HOST_FS_OFLAGS_DIRECTORY) != 0u) {
        if ((oflags & (TURBOWASM_HOST_FS_OFLAGS_CREAT |
                       TURBOWASM_HOST_FS_OFLAGS_TRUNC)) != 0u) {
            error = TURBOWASM_WASI_ERRNO_INVAL;
            goto abort;
        }
        result = cmeta_fs_root_lstat(
            impl->root, slot->path, &slot->stat_snapshot);
        if (result < 0) {
            error = host_errno(result);
            goto abort;
        }
        if (slot->stat_snapshot.is_symlink) {
            error = TURBOWASM_WASI_ERRNO_NOTCAPABLE;
            goto abort;
        }
        if (!slot->stat_snapshot.is_directory) {
            error = TURBOWASM_WASI_ERRNO_NOTDIR;
            goto abort;
        }
        result = cmeta_fs_root_opendir(
            impl->root, slot->path, &slot->dir);
        if (result < 0) {
            error = host_errno(result);
            goto abort;
        }
        slot->directory = true;
    } else {
        result = host_file_flags(
            rights_base, oflags, fdflags, &file_flags);
        if (result < 0) {
            error = host_errno(result);
            goto abort;
        }
        result = cmeta_fs_root_file_open(
            impl->root,
            slot->path,
            file_flags,
            0644,
            &slot->file);
        if (result < 0) {
            error = host_errno(result);
            goto abort;
        }
    }

    cmeta_mutex_lock(&impl->mutex);
    slot->reserved = false;
    slot->used = true;
    *out_file = host_file_from_slot(impl, slot);
    cmeta_mutex_unlock(&impl->mutex);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
abort:
    cmeta_mutex_lock(&impl->mutex);
    host_release_slot(slot);
    cmeta_mutex_unlock(&impl->mutex);
    return error;
}

static uint32_t host_path_stat_admitted(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    char full_path[TURBOWASM_WASI_HOST_FS_PATH_MAX + 1u];
    cmeta_fs_stat_t stat_value;
    uint32_t error;
    int result;

    if (impl == NULL || out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if ((lookup_flags &
         ~TURBOWASM_HOST_FS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if ((lookup_flags &
         TURBOWASM_HOST_FS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    error = host_build_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    result = cmeta_fs_root_lstat(
        impl->root, full_path, &stat_value);
    if (result < 0)
        return host_errno(result);
    host_fill_stat(&stat_value, out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t host_path_mutation_admitted(
    turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length,
    int operation) {
    char full_path[TURBOWASM_WASI_HOST_FS_PATH_MAX + 1u];
    uint32_t error;
    int result;

    if (impl == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    error = host_build_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    if (operation == 0)
        result = cmeta_fs_root_mkdir(impl->root, full_path, 0755);
    else if (operation == 1)
        result = cmeta_fs_root_rmdir(impl->root, full_path);
    else
        result = cmeta_fs_root_unlink(impl->root, full_path);
    return host_errno(result);
}

static uint32_t host_path_mutation(turbowasm_wasi_host_fs_impl *impl,
    turbowasm_wasi_fs_file directory, const uint8_t *path, size_t length, int operation) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(impl, directory, false, &pin);
    if (!error) error = host_path_mutation_admitted(impl, directory, path, length, operation);
    return host_unpin(impl, &pin, error);
}

static uint32_t host_path_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    return host_path_mutation(
        (turbowasm_wasi_host_fs_impl *)context,
        directory, path, path_length, 0);
}

static uint32_t host_path_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    return host_path_mutation(
        (turbowasm_wasi_host_fs_impl *)context,
        directory, path, path_length, 1);
}

static uint32_t host_path_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    return host_path_mutation(
        (turbowasm_wasi_host_fs_impl *)context,
        directory, path, path_length, 2);
}

#ifdef SALTS_FS_ROOT_MUTATION_VERSION
static uint32_t host_set_flags_admitted(void *context, turbowasm_wasi_fs_file file, uint16_t flags) {
    turbowasm_wasi_host_fs_slot *slot = host_slot_from_file(context, file);
    if (!slot) return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory) return TURBOWASM_WASI_ERRNO_ISDIR;
    if (flags & ~TURBOWASM_HOST_FS_FDFLAGS_APPEND) return TURBOWASM_WASI_ERRNO_NOTSUP;
    return host_errno(cmeta_fs_root_file_set_append(slot->file,
        (flags & TURBOWASM_HOST_FS_FDFLAGS_APPEND) != 0));
}

static uint32_t host_path_rename_admitted(void *context,
    turbowasm_wasi_fs_file from, const uint8_t *source, size_t source_length,
    turbowasm_wasi_fs_file to, const uint8_t *target, size_t target_length) {
    turbowasm_wasi_host_fs_impl *impl = context;
    tstr a = tstr_new_len(NULL, impl->path_capacity);
    tstr b = tstr_new_len(NULL, impl->path_capacity);
    uint32_t error = TURBOWASM_WASI_ERRNO_NOMEM;
    if (!a || !b) goto done;
    error = host_build_path(impl, from, source, source_length, a);
    if (!error) error = host_build_path(impl, to, target, target_length, b);
    if (error) goto done;
    cmeta_mutex_lock(&impl->mutex);
    if (impl->renaming || impl->opening) error = TURBOWASM_WASI_ERRNO_BUSY;
    for (size_t i = 0; !error && i < impl->slot_capacity; ++i) {
        turbowasm_wasi_host_fs_slot *slot = &impl->slots[i];
        /* Directory operations currently resolve stored root-relative paths.
         * Refuse mutation while any child directory is open rather than try
         * to infer native path identity from spelling, case or separators. */
        if (slot->used && slot->directory) {
            error = TURBOWASM_WASI_ERRNO_BUSY;
        }
    }
    if (!error) impl->renaming = true;
    cmeta_mutex_unlock(&impl->mutex);
    if (error) goto done;
    error = host_errno(cmeta_fs_root_rename(impl->root, a, impl->root, b));
    cmeta_mutex_lock(&impl->mutex);
    impl->renaming = false;
    cmeta_mutex_unlock(&impl->mutex);
done:
    tstr_free(a);
    tstr_free(b);
    return error;
}
#endif

static uint32_t host_readdir_admitted(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    turbowasm_wasi_host_fs_impl *impl =
        (turbowasm_wasi_host_fs_impl *)context;
    cmeta_fs_root_dir_t *dir = NULL;
    cmeta_fs_root_dirent_t entry = {0};
    char name[TURBOWASM_WASI_FS_DIRENT_NAME_MAX + 1u];
    turbowasm_wasi_host_fs_slot *slot = NULL;
    int result;

    if (impl == NULL || out_entry == NULL || out_has_entry == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (host_is_root(directory)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        if (impl->root_dir == NULL) {
            result = cmeta_fs_root_opendir_self(
                impl->root, &impl->root_dir);
            if (result < 0)
                return host_errno(result);
        }
        dir = impl->root_dir;
    } else {
        slot = host_slot_from_file(impl, directory);
        if (slot == NULL)
            return TURBOWASM_WASI_ERRNO_BADF;
        if (!slot->directory)
            return TURBOWASM_WASI_ERRNO_NOTDIR;
        dir = slot->dir;
    }

    *out_entry = (turbowasm_wasi_fs_dirent){0};
    *out_has_entry = false;
    result = cmeta_fs_root_readdir(
        dir,
        cookie,
        name,
        sizeof(name),
        &entry);
    if (result < 0)
        return host_errno(result);
    if (result == 0)
        return TURBOWASM_WASI_ERRNO_SUCCESS;

    if (entry.name_length > TURBOWASM_WASI_FS_DIRENT_NAME_MAX)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    out_entry->next_cookie = entry.next_cookie;
    out_entry->name_length = (uint32_t)entry.name_length;
    if (entry.type == SALTS_FS_DIRENT_DIRECTORY)
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_DIRECTORY;
    else if (entry.type == SALTS_FS_DIRENT_FILE)
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
    else if (entry.type == SALTS_FS_DIRENT_SYMLINK)
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK;
    else
        out_entry->file_type = TURBOWASM_WASI_FILETYPE_UNKNOWN;
    memcpy(out_entry->name, name, entry.name_length);
    *out_has_entry = true;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

/* Keep admission and release in one place per entry point, so native error
 * returns cannot bypass the pin or I/O lock cleanup. Path operations only pin
 * immutable directory identity; they do not lock its readdir cursor. */
static uint32_t host_read(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_read_admitted(context, file, buffers, count, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_write(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_write_admitted(context, file, buffers, count, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_seek(void *context, turbowasm_wasi_fs_file file,
    int64_t offset, uint8_t whence, uint64_t *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_seek_admitted(context, file, offset, whence, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_tell(void *context, turbowasm_wasi_fs_file file, uint64_t *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_tell_admitted(context, file, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_stat(void *context, turbowasm_wasi_fs_file file, turbowasm_wasi_fs_stat *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_stat_admitted(context, file, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_readdir(void *context, turbowasm_wasi_fs_file file,
    uint64_t cookie, turbowasm_wasi_fs_dirent *out, bool *has_entry) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_readdir_admitted(context, file, cookie, out, has_entry);
    return host_unpin(context, &pin, error);
}
static uint32_t host_path_stat(void *context, turbowasm_wasi_fs_file directory,
    uint32_t flags, const uint8_t *path, size_t length, turbowasm_wasi_fs_stat *out) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, directory, false, &pin);
    if (!error) error = host_path_stat_admitted(context, directory, flags, path, length, out);
    return host_unpin(context, &pin, error);
}
static uint32_t host_path_open(void *context, turbowasm_wasi_fs_file directory,
    uint32_t dirflags, const uint8_t *path, size_t length, uint32_t oflags,
    uint64_t base, uint64_t inheriting, uint32_t flags, turbowasm_wasi_fs_file *out) {
    turbowasm_wasi_host_fs_impl *impl = context;
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_fs_file){0};
    host_pin pin = {0};
    uint32_t error = host_pin_file(impl, directory, false, &pin);
    if (error) return error;
    cmeta_mutex_lock(&impl->mutex);
    if (impl->renaming || impl->opening == UINT32_MAX) error = TURBOWASM_WASI_ERRNO_BUSY;
    else ++impl->opening;
    cmeta_mutex_unlock(&impl->mutex);
    if (!error) {
        error = host_path_open_admitted(impl, directory, dirflags, path, length,
            oflags, base, inheriting, flags, out);
        cmeta_mutex_lock(&impl->mutex);
        --impl->opening;
        cmeta_mutex_unlock(&impl->mutex);
    }
    return host_unpin(impl, &pin, error);
}
#ifdef SALTS_FS_ROOT_MUTATION_VERSION
static uint32_t host_set_flags(void *context, turbowasm_wasi_fs_file file, uint16_t flags) {
    host_pin pin = {0};
    uint32_t error = host_pin_file(context, file, true, &pin);
    if (!error) error = host_set_flags_admitted(context, file, flags);
    return host_unpin(context, &pin, error);
}
static uint32_t host_path_rename(void *context,
    turbowasm_wasi_fs_file from, const uint8_t *source, size_t source_length,
    turbowasm_wasi_fs_file to, const uint8_t *target, size_t target_length) {
    host_pin a = {0}, b = {0};
    uint32_t error = host_pin_file(context, from, false, &a);
    if (!error) error = host_pin_file(context, to, false, &b);
    if (!error) error = host_path_rename_admitted(context, from, source, source_length, to, target, target_length);
    host_unpin(context, &b, error);
    return host_unpin(context, &a, error);
}
#endif

static void host_destroy_mutexes(turbowasm_wasi_host_fs_impl *impl) {
    for (size_t i = 0; i < impl->slot_capacity; ++i)
        if (impl->slots[i].io_mutex) cmeta_mutex_destroy(&impl->slots[i].io_mutex);
    if (impl->root_mutex) cmeta_mutex_destroy(&impl->root_mutex);
    if (impl->mutex) cmeta_mutex_destroy(&impl->mutex);
}

turbowasm_status turbowasm_wasi_host_fs_init(
    turbowasm_wasi_host_fs *adapter,
    const turbowasm_wasi_host_fs_config *config) {
    turbowasm_wasi_host_fs_impl *impl;
    size_t path_stride;
    size_t path_bytes;
    size_t index;
    int result;

    if (adapter == NULL || config == NULL ||
        config->host_root == NULL ||
        config->file_capacity == 0u ||
        config->path_capacity == 0u ||
        config->path_capacity > TURBOWASM_WASI_HOST_FS_PATH_MAX)
        return TURBOWASM_INVALID_ARGUMENT;
    if (adapter->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (config->path_capacity == SIZE_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    path_stride = config->path_capacity + 1u;
    if (config->file_capacity >
        SIZE_MAX / sizeof(turbowasm_wasi_host_fs_slot) ||
        config->file_capacity > SIZE_MAX / path_stride)
        return TURBOWASM_OUT_OF_MEMORY;
    path_bytes = config->file_capacity * path_stride;

    impl = (turbowasm_wasi_host_fs_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    impl->slots = (turbowasm_wasi_host_fs_slot *)calloc(
        config->file_capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    impl->path_storage = (char *)calloc(path_bytes, 1u);
    if (impl->path_storage == NULL) {
        free(impl->slots);
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    for (index = 0u; index < config->file_capacity; ++index)
        impl->slots[index].path =
            impl->path_storage + index * path_stride;

    impl->slot_capacity = config->file_capacity;
    impl->path_capacity = config->path_capacity;
    cmeta_mutex_init(&impl->mutex);
    cmeta_mutex_init(&impl->root_mutex);
    result = impl->mutex && impl->root_mutex ? 0 : -ENOMEM;
    for (index = 0; result == 0 && index < impl->slot_capacity; ++index) {
        cmeta_mutex_init(&impl->slots[index].io_mutex);
        if (!impl->slots[index].io_mutex) result = -ENOMEM;
    }
    if (!result) result = cmeta_fs_root_open(config->host_root, &impl->root);
    if (result < 0) {
        host_destroy_mutexes(impl);
        free(impl->path_storage);
        free(impl->slots);
        free(impl);
        return result == -ENOMEM
            ? TURBOWASM_OUT_OF_MEMORY
            : TURBOWASM_INVALID_ARGUMENT;
    }

    impl->slot_capacity = config->file_capacity;
    impl->path_capacity = config->path_capacity;
    impl->root_open = true;
    adapter->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi_host_fs_destroy(
    turbowasm_wasi_host_fs *adapter) {
    turbowasm_wasi_host_fs_impl *impl;
    size_t index;

    if (adapter == NULL || adapter->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    impl = (turbowasm_wasi_host_fs_impl *)adapter->impl;
    cmeta_mutex_lock(&impl->mutex);
    bool busy = impl->root_open || impl->root_closing || impl->root_pins || impl->opening || impl->renaming;
    for (index = 0u; index < impl->slot_capacity; ++index) {
        busy |= impl->slots[index].used || impl->slots[index].reserved || impl->slots[index].pins;
    }
    cmeta_mutex_unlock(&impl->mutex);
    if (busy) return TURBOWASM_INVALID_ARGUMENT;

    if (impl->root_dir != NULL) {
        (void)cmeta_fs_root_closedir(impl->root_dir);
        impl->root_dir = NULL;
    }

    /*
     * cmeta_fs_root_close() consumes the root identity even when the native
     * close reports an error. Once all public provider identities are already
     * closed, destroy must mirror that ownership transfer and clear adapter
     * state rather than retaining a dangling root pointer.
     */
    (void)cmeta_fs_root_close(impl->root);
    impl->root = NULL;

    host_destroy_mutexes(impl);
    free(impl->path_storage);
    free(impl->slots);
    free(impl);
    adapter->impl = NULL;
    return TURBOWASM_OK;
}

bool turbowasm_wasi_host_fs_provider(
    turbowasm_wasi_host_fs *adapter,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    turbowasm_wasi_host_fs_impl *impl;

    if (adapter == NULL || adapter->impl == NULL ||
        out_provider == NULL || out_root == NULL)
        return false;
    impl = (turbowasm_wasi_host_fs_impl *)adapter->impl;

    *out_provider = (turbowasm_wasi_fs_provider){0};
    out_provider->context = impl;
    out_provider->close = host_close;
    out_provider->read = host_read;
    out_provider->write = host_write;
    out_provider->seek = host_seek;
    out_provider->tell = host_tell;
    out_provider->stat = host_stat;
    out_provider->path_open = host_path_open;
    out_provider->path_stat = host_path_stat;
    out_provider->path_create_directory =
        host_path_create_directory;
    out_provider->path_remove_directory =
        host_path_remove_directory;
    out_provider->path_unlink_file =
        host_path_unlink_file;
    out_provider->readdir = host_readdir;
#ifdef SALTS_FS_ROOT_MUTATION_VERSION
    out_provider->path_rename = host_path_rename;
    out_provider->set_flags = host_set_flags;
#endif
    *out_root = (turbowasm_wasi_fs_file){
        UINT64_C(1), 1u
    };
    return true;
}
