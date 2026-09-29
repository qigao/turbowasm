#include <turbowasm/wasi_littlefs.h>

#include "lfs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_LITTLEFS_OFLAGS_CREAT = 1u,
    TURBOWASM_LITTLEFS_OFLAGS_DIRECTORY = 2u,
    TURBOWASM_LITTLEFS_OFLAGS_EXCL = 4u,
    TURBOWASM_LITTLEFS_OFLAGS_TRUNC = 8u,
    TURBOWASM_LITTLEFS_FDFLAGS_APPEND = 1u,
    TURBOWASM_LITTLEFS_LOOKUPFLAGS_SYMLINK_FOLLOW = 1u
};

typedef struct turbowasm_wasi_littlefs_slot {
    bool used;
    bool directory;
    uint32_t generation;
    lfs_file_t file;
    struct lfs_file_config file_config;
    uint8_t *cache;
    char path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
} turbowasm_wasi_littlefs_slot;

typedef struct turbowasm_wasi_littlefs_impl {
    lfs_t *filesystem;
    turbowasm_wasi_littlefs_slot *slots;
    uint8_t *file_caches;
    size_t slot_capacity;
    size_t cache_size;
    bool root_open;
    char root_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
    size_t root_path_length;
} turbowasm_wasi_littlefs_impl;

static bool littlefs_is_root(turbowasm_wasi_fs_file file) {
    return file.object == UINT64_C(1) && file.generation == 1u;
}

static uint32_t littlefs_errno(int error) {
    switch (error) {
        case LFS_ERR_OK:
            return TURBOWASM_WASI_ERRNO_SUCCESS;
        case LFS_ERR_NOENT:
            return TURBOWASM_WASI_ERRNO_NOENT;
        case LFS_ERR_EXIST:
            return TURBOWASM_WASI_ERRNO_EXIST;
        case LFS_ERR_NOTDIR:
            return TURBOWASM_WASI_ERRNO_NOTDIR;
        case LFS_ERR_ISDIR:
            return TURBOWASM_WASI_ERRNO_ISDIR;
        case LFS_ERR_NOTEMPTY:
            return TURBOWASM_WASI_ERRNO_NOTEMPTY;
        case LFS_ERR_BADF:
            return TURBOWASM_WASI_ERRNO_BADF;
        case LFS_ERR_FBIG:
            return TURBOWASM_WASI_ERRNO_FBIG;
        case LFS_ERR_INVAL:
            return TURBOWASM_WASI_ERRNO_INVAL;
        case LFS_ERR_NOSPC:
            return TURBOWASM_WASI_ERRNO_NOSPC;
        case LFS_ERR_NOMEM:
            return TURBOWASM_WASI_ERRNO_NOMEM;
        case LFS_ERR_NAMETOOLONG:
            return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
        case LFS_ERR_IO:
        case LFS_ERR_CORRUPT:
        case LFS_ERR_NOATTR:
        default:
            return TURBOWASM_WASI_ERRNO_IO;
    }
}

static uint32_t littlefs_validate_relative_path(
    const uint8_t *path,
    size_t path_length,
    bool allow_empty) {
    size_t component_start = 0u;
    size_t index;

    if (path_length == 0u)
        return allow_empty
            ? TURBOWASM_WASI_ERRNO_SUCCESS
            : TURBOWASM_WASI_ERRNO_INVAL;
    if (path == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (path[0] == (uint8_t)'/')
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;

    for (index = 0u; index <= path_length; ++index) {
        bool end = index == path_length;
        uint8_t byte = end ? (uint8_t)'/' : path[index];
        size_t component_length;

        if (!end && byte == 0u)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (byte != (uint8_t)'/')
            continue;

        component_length = index - component_start;
        if (component_length == 0u)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (component_length > LFS_NAME_MAX)
            return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
        if (component_length == 2u &&
            path[component_start] == (uint8_t)'.' &&
            path[component_start + 1u] == (uint8_t)'.')
            return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
        component_start = index + 1u;
    }

    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_normalize_root(
    turbowasm_wasi_littlefs_impl *impl,
    const uint8_t *path,
    size_t path_length) {
    size_t start = 0u;
    size_t length;
    uint32_t error;

    if (path_length == 0u) {
        impl->root_path[0] = '\0';
        impl->root_path_length = 0u;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (path == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    while (start < path_length && path[start] == (uint8_t)'/')
        ++start;
    while (path_length > start &&
           path[path_length - 1u] == (uint8_t)'/')
        --path_length;
    length = path_length - start;

    if (length == 0u) {
        impl->root_path[0] = '\0';
        impl->root_path_length = 0u;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    if (length > TURBOWASM_WASI_LITTLEFS_PATH_MAX)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;

    error = littlefs_validate_relative_path(
        path + start, length, false);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    memcpy(impl->root_path, path + start, length);
    impl->root_path[length] = '\0';
    impl->root_path_length = length;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_wasi_littlefs_slot *littlefs_slot_from_file(
    turbowasm_wasi_littlefs_impl *impl,
    turbowasm_wasi_fs_file file) {
    size_t index;
    turbowasm_wasi_littlefs_slot *slot;

    if (impl == NULL || file.object < UINT64_C(2))
        return NULL;
    index = (size_t)(file.object - UINT64_C(2));
    if (index >= impl->slot_capacity)
        return NULL;
    slot = &impl->slots[index];
    if (!slot->used || slot->generation != file.generation)
        return NULL;
    return slot;
}

static turbowasm_wasi_fs_file littlefs_file_from_slot(
    const turbowasm_wasi_littlefs_impl *impl,
    const turbowasm_wasi_littlefs_slot *slot) {
    turbowasm_wasi_fs_file file = {0};
    size_t index = (size_t)(slot - impl->slots);

    file.object = UINT64_C(2) + (uint64_t)index;
    file.generation = slot->generation;
    return file;
}

static turbowasm_wasi_littlefs_slot *littlefs_reserve_slot(
    turbowasm_wasi_littlefs_impl *impl) {
    size_t index;

    for (index = 0u; index < impl->slot_capacity; ++index) {
        turbowasm_wasi_littlefs_slot *slot = &impl->slots[index];
        uint32_t generation;

        if (slot->used)
            continue;
        generation = slot->generation + 1u;
        if (generation == 0u)
            generation = 1u;

        memset(&slot->file, 0, sizeof(slot->file));
        memset(&slot->file_config, 0, sizeof(slot->file_config));
        memset(slot->path, 0, sizeof(slot->path));
        slot->used = true;
        slot->directory = false;
        slot->generation = generation;
        slot->cache =
            impl->file_caches + index * impl->cache_size;
        slot->file_config.buffer = slot->cache;
        return slot;
    }
    return NULL;
}

static void littlefs_release_slot(
    turbowasm_wasi_littlefs_slot *slot) {
    if (slot == NULL)
        return;
    memset(&slot->file, 0, sizeof(slot->file));
    memset(&slot->file_config, 0, sizeof(slot->file_config));
    memset(slot->path, 0, sizeof(slot->path));
    slot->used = false;
    slot->directory = false;
}

static uint32_t littlefs_directory_base(
    turbowasm_wasi_littlefs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const char **out_path,
    size_t *out_path_length) {
    turbowasm_wasi_littlefs_slot *slot;

    if (impl == NULL || out_path == NULL ||
        out_path_length == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    if (littlefs_is_root(directory)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        *out_path = impl->root_path;
        *out_path_length = impl->root_path_length;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = littlefs_slot_from_file(impl, directory);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (!slot->directory)
        return TURBOWASM_WASI_ERRNO_NOTDIR;
    *out_path = slot->path;
    *out_path_length = strlen(slot->path);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_join_path(
    turbowasm_wasi_littlefs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length,
    char out_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u]) {
    const char *base;
    size_t base_length;
    size_t total;
    uint32_t error;

    error = littlefs_validate_relative_path(
        path, path_length, false);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    error = littlefs_directory_base(
        impl, directory, &base, &base_length);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    total = base_length + (base_length != 0u ? 1u : 0u)
        + path_length;
    if (total > TURBOWASM_WASI_LITTLEFS_PATH_MAX)
        return TURBOWASM_WASI_ERRNO_NAMETOOLONG;

    if (base_length != 0u) {
        memcpy(out_path, base, base_length);
        out_path[base_length] = '/';
        memcpy(
            out_path + base_length + 1u,
            path,
            path_length);
    } else {
        memcpy(out_path, path, path_length);
    }
    out_path[total] = '\0';
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void littlefs_fill_stat(
    turbowasm_wasi_fs_file file,
    bool directory,
    uint64_t size,
    turbowasm_wasi_fs_stat *out_stat) {
    *out_stat = (turbowasm_wasi_fs_stat){0};
    out_stat->file_type = directory
        ? TURBOWASM_WASI_FILETYPE_DIRECTORY
        : TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
    out_stat->size = directory ? 0u : size;
    out_stat->inode = file.object;
    out_stat->link_count = 1u;
}

static uint32_t littlefs_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot;
    int result = 0;

    if (impl == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (littlefs_is_root(file)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        impl->root_open = false;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = littlefs_slot_from_file(impl, file);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (!slot->directory)
        result = lfs_file_close(impl->filesystem, &slot->file);

    littlefs_release_slot(slot);
    return result < 0
        ? littlefs_errno(result)
        : TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot =
        littlefs_slot_from_file(impl, file);
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
        lfs_ssize_t result;

        if (buffers[index].size != 0u &&
            buffers[index].data == NULL)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (buffers[index].size >
            (size_t)(UINT32_MAX - total))
            return TURBOWASM_WASI_ERRNO_INVAL;

        result = lfs_file_read(
            impl->filesystem,
            &slot->file,
            buffers[index].data,
            (lfs_size_t)buffers[index].size);
        if (result < 0)
            return littlefs_errno((int)result);
        total += (uint32_t)result;
        if ((size_t)result != buffers[index].size)
            break;
    }
    *out_read = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot =
        littlefs_slot_from_file(impl, file);
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
        lfs_ssize_t result;

        if (buffers[index].size != 0u &&
            buffers[index].data == NULL)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (buffers[index].size >
            (size_t)(UINT32_MAX - total))
            return TURBOWASM_WASI_ERRNO_INVAL;

        result = lfs_file_write(
            impl->filesystem,
            &slot->file,
            buffers[index].data,
            (lfs_size_t)buffers[index].size);
        if (result < 0)
            return littlefs_errno((int)result);
        total += (uint32_t)result;
        if ((size_t)result != buffers[index].size)
            break;
    }
    *out_written = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_seek(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot =
        littlefs_slot_from_file(impl, file);
    int lfs_whence;
    lfs_soff_t result;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_offset == NULL ||
        offset < INT32_MIN || offset > INT32_MAX)
        return TURBOWASM_WASI_ERRNO_INVAL;

    switch (whence) {
        case TURBOWASM_WASI_WHENCE_SET:
            lfs_whence = LFS_SEEK_SET;
            break;
        case TURBOWASM_WASI_WHENCE_CUR:
            lfs_whence = LFS_SEEK_CUR;
            break;
        case TURBOWASM_WASI_WHENCE_END:
            lfs_whence = LFS_SEEK_END;
            break;
        default:
            return TURBOWASM_WASI_ERRNO_INVAL;
    }

    result = lfs_file_seek(
        impl->filesystem,
        &slot->file,
        (lfs_soff_t)offset,
        lfs_whence);
    if (result < 0)
        return littlefs_errno((int)result);
    *out_offset = (uint64_t)result;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_tell(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot =
        littlefs_slot_from_file(impl, file);
    lfs_soff_t result;

    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory)
        return TURBOWASM_WASI_ERRNO_ISDIR;
    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    result = lfs_file_tell(impl->filesystem, &slot->file);
    if (result < 0)
        return littlefs_errno((int)result);
    *out_offset = (uint64_t)result;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_stat(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot;
    lfs_soff_t size;

    if (impl == NULL || out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (littlefs_is_root(file)) {
        if (!impl->root_open)
            return TURBOWASM_WASI_ERRNO_BADF;
        littlefs_fill_stat(file, true, 0u, out_stat);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    slot = littlefs_slot_from_file(impl, file);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (slot->directory) {
        littlefs_fill_stat(file, true, 0u, out_stat);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }

    size = lfs_file_size(impl->filesystem, &slot->file);
    if (size < 0)
        return littlefs_errno((int)size);
    littlefs_fill_stat(file, false, (uint64_t)size, out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_path_open(
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
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    turbowasm_wasi_littlefs_slot *slot;
    char full_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
    uint32_t error;
    bool directory_open;
    int flags = 0;
    int result;

    (void)rights_inheriting;

    if (impl == NULL || out_file == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if ((dirflags & ~TURBOWASM_LITTLEFS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u ||
        (oflags & ~(TURBOWASM_LITTLEFS_OFLAGS_CREAT |
                    TURBOWASM_LITTLEFS_OFLAGS_DIRECTORY |
                    TURBOWASM_LITTLEFS_OFLAGS_EXCL |
                    TURBOWASM_LITTLEFS_OFLAGS_TRUNC)) != 0u ||
        (fdflags & ~TURBOWASM_LITTLEFS_FDFLAGS_APPEND) != 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;

    error = littlefs_join_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    directory_open =
        (oflags & TURBOWASM_LITTLEFS_OFLAGS_DIRECTORY) != 0u;
    if (directory_open &&
        (oflags & (TURBOWASM_LITTLEFS_OFLAGS_CREAT |
                   TURBOWASM_LITTLEFS_OFLAGS_TRUNC)) != 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;

    slot = littlefs_reserve_slot(impl);
    if (slot == NULL)
        return TURBOWASM_WASI_ERRNO_MFILE;

    if (directory_open) {
        struct lfs_info info;
        result = lfs_stat(impl->filesystem, full_path, &info);
        if (result < 0) {
            littlefs_release_slot(slot);
            return littlefs_errno(result);
        }
        if (info.type != LFS_TYPE_DIR) {
            littlefs_release_slot(slot);
            return TURBOWASM_WASI_ERRNO_NOTDIR;
        }
        slot->directory = true;
    } else {
        bool can_read =
            (rights_base & TURBOWASM_WASI_RIGHT_FD_READ) != 0u;
        bool can_write =
            (rights_base & TURBOWASM_WASI_RIGHT_FD_WRITE) != 0u;

        if (can_write)
            flags = can_read ? LFS_O_RDWR : LFS_O_WRONLY;
        else
            flags = LFS_O_RDONLY;
        if ((oflags & TURBOWASM_LITTLEFS_OFLAGS_CREAT) != 0u)
            flags |= LFS_O_CREAT;
        if ((oflags & TURBOWASM_LITTLEFS_OFLAGS_EXCL) != 0u)
            flags |= LFS_O_EXCL;
        if ((oflags & TURBOWASM_LITTLEFS_OFLAGS_TRUNC) != 0u)
            flags |= LFS_O_TRUNC;
        if ((fdflags & TURBOWASM_LITTLEFS_FDFLAGS_APPEND) != 0u)
            flags |= LFS_O_APPEND;

        result = lfs_file_opencfg(
            impl->filesystem,
            &slot->file,
            full_path,
            flags,
            &slot->file_config);
        if (result < 0) {
            littlefs_release_slot(slot);
            return littlefs_errno(result);
        }
    }

    memcpy(slot->path, full_path, strlen(full_path) + 1u);
    *out_file = littlefs_file_from_slot(impl, slot);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_path_stat(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    char full_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
    struct lfs_info info;
    turbowasm_wasi_fs_file anonymous = {0};
    uint32_t error;
    int result;

    if (impl == NULL || out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if ((lookup_flags &
         ~TURBOWASM_LITTLEFS_LOOKUPFLAGS_SYMLINK_FOLLOW) != 0u)
        return TURBOWASM_WASI_ERRNO_INVAL;

    error = littlefs_join_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    result = lfs_stat(impl->filesystem, full_path, &info);
    if (result < 0)
        return littlefs_errno(result);

    littlefs_fill_stat(
        anonymous,
        info.type == LFS_TYPE_DIR,
        info.size,
        out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_path_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    char full_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
    uint32_t error;
    int result;

    error = littlefs_join_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;
    result = lfs_mkdir(impl->filesystem, full_path);
    return result < 0
        ? littlefs_errno(result)
        : TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_remove_path(
    turbowasm_wasi_littlefs_impl *impl,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length,
    bool directory_expected) {
    char full_path[TURBOWASM_WASI_LITTLEFS_PATH_MAX + 1u];
    struct lfs_info info;
    uint32_t error;
    int result;

    error = littlefs_join_path(
        impl, directory, path, path_length, full_path);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    result = lfs_stat(impl->filesystem, full_path, &info);
    if (result < 0)
        return littlefs_errno(result);
    if (directory_expected && info.type != LFS_TYPE_DIR)
        return TURBOWASM_WASI_ERRNO_NOTDIR;
    if (!directory_expected && info.type == LFS_TYPE_DIR)
        return TURBOWASM_WASI_ERRNO_ISDIR;

    result = lfs_remove(impl->filesystem, full_path);
    return result < 0
        ? littlefs_errno(result)
        : TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t littlefs_path_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    return littlefs_remove_path(
        (turbowasm_wasi_littlefs_impl *)context,
        directory,
        path,
        path_length,
        true);
}

static uint32_t littlefs_path_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    return littlefs_remove_path(
        (turbowasm_wasi_littlefs_impl *)context,
        directory,
        path,
        path_length,
        false);
}

static uint32_t littlefs_readdir(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    turbowasm_wasi_littlefs_impl *impl =
        (turbowasm_wasi_littlefs_impl *)context;
    const char *base;
    size_t base_length;
    const char *open_path;
    lfs_dir_t dir;
    uint32_t error;
    int result;

    if (impl == NULL ||
        out_entry == NULL || out_has_entry == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (cookie > UINT32_MAX)
        return TURBOWASM_WASI_ERRNO_INVAL;

    error = littlefs_directory_base(
        impl, directory, &base, &base_length);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    open_path = base_length == 0u ? "/" : base;
    memset(&dir, 0, sizeof(dir));
    result = lfs_dir_open(impl->filesystem, &dir, open_path);
    if (result < 0)
        return littlefs_errno(result);

    if (cookie != 0u) {
        result = lfs_dir_seek(
            impl->filesystem, &dir, (lfs_off_t)cookie);
        if (result < 0) {
            (void)lfs_dir_close(impl->filesystem, &dir);
            return littlefs_errno(result);
        }
    }

    *out_entry = (turbowasm_wasi_fs_dirent){0};
    *out_has_entry = false;
    for (;;) {
        struct lfs_info info;
        lfs_soff_t next_cookie;
        size_t name_length;

        result = lfs_dir_read(impl->filesystem, &dir, &info);
        if (result < 0) {
            (void)lfs_dir_close(impl->filesystem, &dir);
            return littlefs_errno(result);
        }
        if (result == 0)
            break;
        if ((strcmp(info.name, ".") == 0) ||
            (strcmp(info.name, "..") == 0))
            continue;

        name_length = strlen(info.name);
        if (name_length > TURBOWASM_WASI_FS_DIRENT_NAME_MAX) {
            (void)lfs_dir_close(impl->filesystem, &dir);
            return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
        }

        next_cookie = lfs_dir_tell(impl->filesystem, &dir);
        if (next_cookie < 0) {
            (void)lfs_dir_close(impl->filesystem, &dir);
            return littlefs_errno((int)next_cookie);
        }

        out_entry->next_cookie = (uint64_t)next_cookie;
        out_entry->name_length = (uint32_t)name_length;
        out_entry->file_type = info.type == LFS_TYPE_DIR
            ? TURBOWASM_WASI_FILETYPE_DIRECTORY
            : TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
        memcpy(out_entry->name, info.name, name_length);
        *out_has_entry = true;
        break;
    }

    result = lfs_dir_close(impl->filesystem, &dir);
    if (result < 0)
        return littlefs_errno(result);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

turbowasm_status turbowasm_wasi_littlefs_init(
    turbowasm_wasi_littlefs *adapter,
    const turbowasm_wasi_littlefs_config *config) {
    turbowasm_wasi_littlefs_impl *impl;
    lfs_t *filesystem;
    size_t cache_bytes;
    uint32_t path_error;

    if (adapter == NULL || config == NULL ||
        config->filesystem == NULL ||
        config->file_capacity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (adapter->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    filesystem = (lfs_t *)config->filesystem;
    if (filesystem->cfg == NULL ||
        filesystem->cfg->cache_size == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (config->file_capacity >
        SIZE_MAX / sizeof(turbowasm_wasi_littlefs_slot))
        return TURBOWASM_OUT_OF_MEMORY;
    if (config->file_capacity >
        SIZE_MAX / (size_t)filesystem->cfg->cache_size)
        return TURBOWASM_OUT_OF_MEMORY;

    impl = (turbowasm_wasi_littlefs_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->slots = (turbowasm_wasi_littlefs_slot *)calloc(
        config->file_capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    cache_bytes =
        config->file_capacity * (size_t)filesystem->cfg->cache_size;
    impl->file_caches = (uint8_t *)malloc(cache_bytes);
    if (impl->file_caches == NULL) {
        free(impl->slots);
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->filesystem = filesystem;
    impl->slot_capacity = config->file_capacity;
    impl->cache_size = (size_t)filesystem->cfg->cache_size;
    impl->root_open = true;

    path_error = littlefs_normalize_root(
        impl, config->root_path, config->root_path_length);
    if (path_error != TURBOWASM_WASI_ERRNO_SUCCESS) {
        free(impl->file_caches);
        free(impl->slots);
        free(impl);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    adapter->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi_littlefs_destroy(
    turbowasm_wasi_littlefs *adapter) {
    turbowasm_wasi_littlefs_impl *impl;
    size_t index;

    if (adapter == NULL || adapter->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    impl = (turbowasm_wasi_littlefs_impl *)adapter->impl;

    if (impl->root_open)
        return TURBOWASM_INVALID_ARGUMENT;
    for (index = 0u; index < impl->slot_capacity; ++index) {
        if (impl->slots[index].used)
            return TURBOWASM_INVALID_ARGUMENT;
    }

    free(impl->file_caches);
    free(impl->slots);
    free(impl);
    adapter->impl = NULL;
    return TURBOWASM_OK;
}

bool turbowasm_wasi_littlefs_provider(
    turbowasm_wasi_littlefs *adapter,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    turbowasm_wasi_littlefs_impl *impl;

    if (adapter == NULL || adapter->impl == NULL ||
        out_provider == NULL || out_root == NULL)
        return false;
    impl = (turbowasm_wasi_littlefs_impl *)adapter->impl;

    *out_provider = (turbowasm_wasi_fs_provider){0};
    out_provider->context = impl;
    out_provider->close = littlefs_close;
    out_provider->read = littlefs_read;
    out_provider->write = littlefs_write;
    out_provider->seek = littlefs_seek;
    out_provider->tell = littlefs_tell;
    out_provider->stat = littlefs_stat;
    out_provider->path_open = littlefs_path_open;
    out_provider->path_stat = littlefs_path_stat;
    out_provider->path_create_directory =
        littlefs_path_create_directory;
    out_provider->path_remove_directory =
        littlefs_path_remove_directory;
    out_provider->path_unlink_file =
        littlefs_path_unlink_file;
    out_provider->readdir = littlefs_readdir;
    *out_root = (turbowasm_wasi_fs_file){
        UINT64_C(1), 1u
    };
    return true;
}
