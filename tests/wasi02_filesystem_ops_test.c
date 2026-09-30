#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_ops {
    uint64_t next_object;
    uint32_t close_calls;
    uint32_t open_calls;
    uint32_t stat_calls;
    uint32_t path_stat_calls;
    uint32_t create_calls;
    uint32_t remove_calls;
    uint32_t unlink_calls;
    bool fail_next_close;
} fake_ops;

static uint32_t fake_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_ops *ops = (fake_ops *)context;
    (void)file;
    ++ops->close_calls;
    if (ops->fail_next_close) {
        ops->fail_next_close = false;
        return TURBOWASM_WASI_ERRNO_IO;
    }
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    if (out_read != NULL) *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    if (out_written != NULL) *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void fill_stat(
    turbowasm_wasi_fs_stat *out,
    uint8_t file_type) {
    *out = (turbowasm_wasi_fs_stat){0};
    out->file_type = file_type;
    out->link_count = 1u;
    out->size = UINT64_C(1234);
    out->accessed_ns = UINT64_C(1000000001);
    out->modified_ns = UINT64_C(2000000002);
    out->changed_ns = UINT64_C(3000000003);
    out->timestamp_valid =
        TURBOWASM_WASI_FS_TIME_ACCESSED_VALID |
        TURBOWASM_WASI_FS_TIME_MODIFIED_VALID;
}

static uint32_t fake_stat(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out) {
    fake_ops *ops = (fake_ops *)context;
    ++ops->stat_calls;
    fill_stat(
        out,
        file.object == UINT64_C(1)
            ? TURBOWASM_WASI_FILETYPE_DIRECTORY
            : TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_path_stat(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out) {
    fake_ops *ops = (fake_ops *)context;
    (void)directory;
    assert(lookup_flags == 0u);
    assert(path_length == 4u);
    assert(memcmp(path, "file", 4u) == 0);
    ++ops->path_stat_calls;
    fill_stat(out, TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
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
    fake_ops *ops = (fake_ops *)context;
    (void)directory;
    (void)dirflags;
    (void)path;
    (void)path_length;
    (void)oflags;
    (void)rights_base;
    (void)rights_inheriting;
    assert(fdflags == 0u);
    ++ops->open_calls;
    out_file->object = ++ops->next_object;
    out_file->generation = 1u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_create(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_ops *ops = (fake_ops *)context;
    (void)directory; (void)path; (void)path_length;
    ++ops->create_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_remove(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_ops *ops = (fake_ops *)context;
    (void)directory; (void)path; (void)path_length;
    ++ops->remove_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_unlink(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_ops *ops = (fake_ops *)context;
    (void)directory; (void)path; (void)path_length;
    ++ops->unlink_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t first_preopen_resource(
    turbowasm_wasi02_filesystem *bridge) {
    turbowasm_wasi02_value dirs = {0};
    uint32_t resource;

    assert(turbowasm_wasi02_filesystem_get_directories(
               bridge, &dirs) == TURBOWASM_OK);
    assert(dirs.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(dirs.as.list.count == 1u);
    resource =
        dirs.as.list.items[0].as.tuple.items[0].as.resource;
    turbowasm_wasi02_value_destroy(&dirs);
    return resource;
}

static void set_resource(
    turbowasm_wasi02_value *value,
    uint32_t resource) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    value->as.resource = resource;
}

static void set_string(
    turbowasm_wasi02_value *value,
    const char *text) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_STRING;
    value->as.string.data = (uint8_t *)text;
    value->as.string.size = strlen(text);
}

static void set_flags(
    turbowasm_wasi02_value *value,
    uint32_t flags) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_FLAGS;
    value->as.flags = flags;
}

static uint32_t success_resource(
    turbowasm_wasi02_value *result) {
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    return result->as.result.value->as.resource;
}

static void test_descriptor_operations(void) {
    fake_ops ops = {UINT64_C(100), 0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi02_value args[5] = {{0}};
    turbowasm_wasi02_value result = {0};
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint64_t all_rights =
        TURBOWASM_WASI_RIGHT_FD_READ |
        TURBOWASM_WASI_RIGHT_FD_WRITE |
        TURBOWASM_WASI_RIGHT_FD_READDIR |
        TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_OPEN |
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
    uint32_t preopen;
    uint32_t child;
    uint32_t directory_child;
    uint32_t nested;

    config.descriptor_capacity = 12u;
    config.provider.context = &ops;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;
    config.provider.stat = fake_stat;
    config.provider.path_stat = fake_path_stat;
    config.provider.path_open = fake_path_open;
    config.provider.path_create_directory = fake_create;
    config.provider.path_remove_directory = fake_remove;
    config.provider.path_unlink_file = fake_unlink;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem, 3u,
               (turbowasm_wasi_fs_file){UINT64_C(1), 1u},
               true, "/",
               all_rights, all_rights,
               &root) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 16u) == TURBOWASM_OK);

    preopen = first_preopen_resource(&bridge);

    set_resource(&args[0], preopen);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.stat",
               args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RECORD);
    assert(result.as.result.value->as.record.count == 6u);
    assert(result.as.result.value->as.record.items[0]
               .as.enum_index == 3u);
    assert(result.as.result.value->as.record.items[3]
               .as.option.has_value);
    assert(result.as.result.value->as.record.items[4]
               .as.option.has_value);
    assert(!result.as.result.value->as.record.items[5]
                .as.option.has_value);
    turbowasm_wasi02_value_destroy(&result);

    set_flags(&args[1], 0u);
    set_string(&args[2], "file");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.stat-at",
               args, 3u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    assert(result.as.result.value->as.record.items[0]
               .as.enum_index == 6u);
    turbowasm_wasi02_value_destroy(&result);

    set_string(&args[1], "newdir");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.create-directory-at",
               args, 2u, &result) == TURBOWASM_OK);
    assert(!result.as.result.is_error);
    turbowasm_wasi02_value_destroy(&result);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.remove-directory-at",
               args, 2u, &result) == TURBOWASM_OK);
    turbowasm_wasi02_value_destroy(&result);
    set_string(&args[1], "file");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.unlink-file-at",
               args, 2u, &result) == TURBOWASM_OK);
    turbowasm_wasi02_value_destroy(&result);
    assert(ops.create_calls == 1u);
    assert(ops.remove_calls == 1u);
    assert(ops.unlink_calls == 1u);

    set_resource(&args[0], preopen);
    set_flags(&args[1], 0u);
    set_string(&args[2], "file");
    set_flags(&args[3], 0u);
    set_flags(&args[4], 1u); /* descriptor-flags::read */
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    child = success_resource(&result);
    turbowasm_wasi02_value_destroy(&result);
    assert(turbowasm_wasi02_filesystem_descriptor_resolve(
               &bridge, child, &info) == TURBOWASM_OK);
    assert(!info.preopen);

    ops.fail_next_close = true;
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, child) == TURBOWASM_TRAPPED);
    assert(turbowasm_wasi02_filesystem_descriptor_resolve(
               &bridge, child, &info) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, child) == TURBOWASM_OK);

    /* Open a directory child, then open again relative to that non-preopen. */
    set_resource(&args[0], preopen);
    set_flags(&args[1], 0u);
    set_string(&args[2], "subdir");
    set_flags(&args[3], 2u); /* open-flags::directory */
    set_flags(&args[4], 32u); /* descriptor-flags::mutate-directory */
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    directory_child = success_resource(&result);
    turbowasm_wasi02_value_destroy(&result);

    set_resource(&args[0], directory_child);
    set_string(&args[2], "nested");
    set_flags(&args[3], 0u);
    set_flags(&args[4], 1u);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    nested = success_resource(&result);
    turbowasm_wasi02_value_destroy(&result);
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, nested) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, directory_child) == TURBOWASM_OK);

    /* Sync request bits cannot be implemented losslessly by current providers. */
    set_resource(&args[0], preopen);
    set_string(&args[2], "file");
    set_flags(&args[3], 0u);
    set_flags(&args[4], 4u); /* file-integrity-sync */
    assert(turbowasm_wasi02_filesystem_call(
               &bridge, "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    assert(result.as.result.is_error);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_ENUM);
    assert(result.as.result.value->as.enum_index == 27u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, preopen) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, root) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);

    assert(ops.open_calls == 3u);
    assert(ops.close_calls == 5u);
    assert(ops.stat_calls == 1u);
    assert(ops.path_stat_calls == 1u);
}

int main(void) {
    test_descriptor_operations();
    return 0;
}
