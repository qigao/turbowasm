#include "wasi_fs_contract.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    FAKE_NODE_CAPACITY = 8,
    FAKE_NAME_CAPACITY = 32,
    FAKE_DATA_CAPACITY = 128,
    FAKE_OFLAGS_CREAT = 1u
};

typedef struct fake_node {
    bool used;
    bool directory;
    uint32_t generation;
    uint8_t name[FAKE_NAME_CAPACITY];
    size_t name_length;
    uint8_t data[FAKE_DATA_CAPACITY];
    size_t size;
    uint64_t offset;
} fake_node;

typedef struct fake_backend {
    fake_node nodes[FAKE_NODE_CAPACITY];
} fake_backend;

static bool fake_is_root(turbowasm_wasi_fs_file file) {
    return file.object == UINT64_C(1) && file.generation == 1u;
}

static fake_node *fake_node_from_file(
    fake_backend *backend,
    turbowasm_wasi_fs_file file) {
    size_t index;

    if (backend == NULL || file.object < UINT64_C(2))
        return NULL;
    index = (size_t)(file.object - UINT64_C(2));
    if (index >= FAKE_NODE_CAPACITY)
        return NULL;
    if (!backend->nodes[index].used ||
        backend->nodes[index].generation != file.generation)
        return NULL;
    return &backend->nodes[index];
}

static turbowasm_wasi_fs_file fake_file_from_index(
    const fake_backend *backend,
    size_t index) {
    turbowasm_wasi_fs_file file = {0};
    assert(backend != NULL);
    assert(index < FAKE_NODE_CAPACITY);
    file.object = UINT64_C(2) + (uint64_t)index;
    file.generation = backend->nodes[index].generation;
    return file;
}

static fake_node *fake_find_path(
    fake_backend *backend,
    const uint8_t *path,
    size_t path_length,
    size_t *out_index) {
    size_t index;

    for (index = 0u; index < FAKE_NODE_CAPACITY; ++index) {
        fake_node *node = &backend->nodes[index];
        if (!node->used || node->name_length != path_length)
            continue;
        if (path_length == 0u ||
            memcmp(node->name, path, path_length) == 0) {
            if (out_index != NULL)
                *out_index = index;
            return node;
        }
    }
    return NULL;
}

static fake_node *fake_create_node(
    fake_backend *backend,
    const uint8_t *path,
    size_t path_length,
    bool directory,
    size_t *out_index) {
    size_t index;

    if (backend == NULL ||
        path_length == 0u ||
        path_length > FAKE_NAME_CAPACITY ||
        (path_length != 0u && path == NULL))
        return NULL;
    if (fake_find_path(backend, path, path_length, NULL) != NULL)
        return NULL;

    for (index = 0u; index < FAKE_NODE_CAPACITY; ++index) {
        fake_node *node = &backend->nodes[index];
        uint32_t generation;
        if (node->used)
            continue;
        generation = node->generation + 1u;
        if (generation == 0u)
            generation = 1u;
        *node = (fake_node){0};
        node->used = true;
        node->directory = directory;
        node->generation = generation;
        node->name_length = path_length;
        memcpy(node->name, path, path_length);
        if (out_index != NULL)
            *out_index = index;
        return node;
    }
    return NULL;
}

static uint32_t fake_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_backend *backend = (fake_backend *)context;

    if (fake_is_root(file))
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    return fake_node_from_file(backend, file) != NULL
        ? TURBOWASM_WASI_ERRNO_SUCCESS
        : TURBOWASM_WASI_ERRNO_BADF;
}

static uint32_t fake_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node = fake_node_from_file(backend, file);
    size_t index;
    uint32_t total = 0u;

    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_read == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    for (index = 0u; index < buffer_count; ++index) {
        size_t available;
        size_t amount;

        if (buffers[index].size != 0u &&
            buffers[index].data == NULL)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (node->offset >= node->size)
            break;
        available = node->size - (size_t)node->offset;
        amount = buffers[index].size < available
            ? buffers[index].size
            : available;
        if (amount != 0u) {
            memcpy(
                buffers[index].data,
                node->data + (size_t)node->offset,
                amount);
            node->offset += amount;
            total += (uint32_t)amount;
        }
        if (amount != buffers[index].size)
            break;
    }
    *out_read = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node = fake_node_from_file(backend, file);
    size_t index;
    uint32_t total = 0u;

    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_written == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    for (index = 0u; index < buffer_count; ++index) {
        size_t end;
        if (buffers[index].size != 0u &&
            buffers[index].data == NULL)
            return TURBOWASM_WASI_ERRNO_INVAL;
        if (node->offset > FAKE_DATA_CAPACITY ||
            buffers[index].size >
                FAKE_DATA_CAPACITY - (size_t)node->offset)
            return TURBOWASM_WASI_ERRNO_IO;
        end = (size_t)node->offset + buffers[index].size;
        if (buffers[index].size != 0u) {
            memcpy(
                node->data + (size_t)node->offset,
                buffers[index].data,
                buffers[index].size);
        }
        node->offset = end;
        if (end > node->size)
            node->size = end;
        total += (uint32_t)buffers[index].size;
    }
    *out_written = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_seek(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node = fake_node_from_file(backend, file);
    int64_t base;
    int64_t next;

    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    switch (whence) {
        case TURBOWASM_WASI_WHENCE_SET:
            base = 0;
            break;
        case TURBOWASM_WASI_WHENCE_CUR:
            base = (int64_t)node->offset;
            break;
        case TURBOWASM_WASI_WHENCE_END:
            base = (int64_t)node->size;
            break;
        default:
            return TURBOWASM_WASI_ERRNO_INVAL;
    }

    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return TURBOWASM_WASI_ERRNO_INVAL;
    next = base + offset;
    if (next < 0 || (uint64_t)next > FAKE_DATA_CAPACITY)
        return TURBOWASM_WASI_ERRNO_INVAL;

    node->offset = (uint64_t)next;
    *out_offset = node->offset;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_tell(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node = fake_node_from_file(backend, file);

    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_offset == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    *out_offset = node->offset;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void fake_fill_stat(
    turbowasm_wasi_fs_file file,
    const fake_node *node,
    bool root,
    turbowasm_wasi_fs_stat *out_stat) {
    *out_stat = (turbowasm_wasi_fs_stat){0};
    out_stat->file_type = root || node->directory
        ? TURBOWASM_WASI_FILETYPE_DIRECTORY
        : TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
    out_stat->inode = file.object;
    out_stat->link_count = 1u;
    if (!root && !node->directory)
        out_stat->size = node->size;
}

static uint32_t fake_stat(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node;

    if (out_stat == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (fake_is_root(file)) {
        fake_fill_stat(file, NULL, true, out_stat);
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    node = fake_node_from_file(backend, file);
    if (node == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    fake_fill_stat(file, node, false, out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_path_open(
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
    fake_backend *backend = (fake_backend *)context;
    fake_node *node;
    size_t index = 0u;

    (void)dirflags;
    (void)rights_base;
    (void)rights_inheriting;
    (void)fdflags;

    if (!fake_is_root(directory) || out_file == NULL)
        return TURBOWASM_WASI_ERRNO_BADF;
    if (path_length == 0u || path == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    node = fake_find_path(backend, path, path_length, &index);
    if (node == NULL && (oflags & FAKE_OFLAGS_CREAT) != 0u)
        node = fake_create_node(
            backend, path, path_length, false, &index);
    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_INVAL;

    node->offset = 0u;
    *out_file = fake_file_from_index(backend, index);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_path_stat(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node;
    size_t index = 0u;

    (void)lookup_flags;
    if (!fake_is_root(directory))
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_stat == NULL ||
        path_length == 0u || path == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    node = fake_find_path(backend, path, path_length, &index);
    if (node == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    fake_fill_stat(
        fake_file_from_index(backend, index),
        node,
        false,
        out_stat);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_path_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_backend *backend = (fake_backend *)context;

    if (!fake_is_root(directory))
        return TURBOWASM_WASI_ERRNO_BADF;
    return fake_create_node(
        backend, path, path_length, true, NULL) != NULL
        ? TURBOWASM_WASI_ERRNO_SUCCESS
        : TURBOWASM_WASI_ERRNO_INVAL;
}

static uint32_t fake_path_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node;

    if (!fake_is_root(directory))
        return TURBOWASM_WASI_ERRNO_BADF;
    node = fake_find_path(backend, path, path_length, NULL);
    if (node == NULL || !node->directory)
        return TURBOWASM_WASI_ERRNO_INVAL;
    node->used = false;
    node->size = 0u;
    node->offset = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_path_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_backend *backend = (fake_backend *)context;
    fake_node *node;

    if (!fake_is_root(directory))
        return TURBOWASM_WASI_ERRNO_BADF;
    node = fake_find_path(backend, path, path_length, NULL);
    if (node == NULL || node->directory)
        return TURBOWASM_WASI_ERRNO_INVAL;
    node->used = false;
    node->size = 0u;
    node->offset = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_readdir(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    fake_backend *backend = (fake_backend *)context;
    size_t index;

    if (!fake_is_root(directory))
        return TURBOWASM_WASI_ERRNO_BADF;
    if (out_entry == NULL || out_has_entry == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (cookie > FAKE_NODE_CAPACITY)
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_entry = (turbowasm_wasi_fs_dirent){0};
    *out_has_entry = false;

    for (index = (size_t)cookie;
         index < FAKE_NODE_CAPACITY;
         ++index) {
        fake_node *node = &backend->nodes[index];
        if (!node->used)
            continue;

        out_entry->next_cookie = (uint64_t)index + 1u;
        out_entry->inode = UINT64_C(2) + (uint64_t)index;
        out_entry->name_length = (uint32_t)node->name_length;
        out_entry->file_type = node->directory
            ? TURBOWASM_WASI_FILETYPE_DIRECTORY
            : TURBOWASM_WASI_FILETYPE_REGULAR_FILE;
        memcpy(
            out_entry->name,
            node->name,
            node->name_length);
        *out_has_entry = true;
        return TURBOWASM_WASI_ERRNO_SUCCESS;
    }
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_status fake_setup(
    void *context,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    fake_backend *backend = (fake_backend *)context;

    if (backend == NULL ||
        out_provider == NULL ||
        out_root == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(backend, 0, sizeof(*backend));
    *out_provider = (turbowasm_wasi_fs_provider){0};
    out_provider->context = backend;
    out_provider->close = fake_close;
    out_provider->read = fake_read;
    out_provider->write = fake_write;
    out_provider->seek = fake_seek;
    out_provider->tell = fake_tell;
    out_provider->stat = fake_stat;
    out_provider->path_open = fake_path_open;
    out_provider->path_stat = fake_path_stat;
    out_provider->path_create_directory =
        fake_path_create_directory;
    out_provider->path_remove_directory =
        fake_path_remove_directory;
    out_provider->path_unlink_file =
        fake_path_unlink_file;
    out_provider->readdir = fake_readdir;
    *out_root = (turbowasm_wasi_fs_file){
        UINT64_C(1), 1u
    };
    return TURBOWASM_OK;
}

static void fake_teardown(void *context) {
    fake_backend *backend = (fake_backend *)context;
    assert(backend != NULL);
    memset(backend, 0, sizeof(*backend));
}

int main(void) {
    fake_backend backend = {0};
    const turbowasm_wasi_fs_contract_fixture fixture = {
        &backend,
        fake_setup,
        fake_teardown
    };
    return turbowasm_wasi_fs_contract_run(&fixture);
}
