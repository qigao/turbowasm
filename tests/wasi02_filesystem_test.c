#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_fs {
    uint32_t close_calls;
    uint32_t open_calls;
    uint32_t create_calls;
    uint32_t remove_calls;
    uint32_t unlink_calls;
    uint64_t next_file;
} fake_fs;

static uint32_t fake_close(void *context, turbowasm_wasi_fs_file file) {
    fake_fs *fs = (fake_fs *)context;
    (void)file;
    ++fs->close_calls;
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
    fake_fs *fs = (fake_fs *)context;
    (void)directory;
    (void)dirflags;
    (void)oflags;
    (void)rights_base;
    (void)rights_inheriting;
    (void)fdflags;

    if (fs == NULL || out_file == NULL ||
        (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;
    ++fs->open_calls;
    ++fs->next_file;
    out_file->value = fs->next_file;
    out_file->generation = 1u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_fs *fs = (fake_fs *)context;
    (void)directory;
    if (fs == NULL || (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;
    ++fs->create_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_fs *fs = (fake_fs *)context;
    (void)directory;
    if (fs == NULL || (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;
    ++fs->remove_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_fs *fs = (fake_fs *)context;
    (void)directory;
    if (fs == NULL || (path_length != 0u && path == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;
    ++fs->unlink_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void test_preopen_projection_and_generation(void) {
    fake_fs fake = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi_fs_descriptor d0 = {0};
    turbowasm_wasi_fs_descriptor d1 = {0};
    turbowasm_wasi_fs_descriptor replacement = {0};
    turbowasm_wasi02_value result = {0};
    turbowasm_wasi_fs_descriptor_info info = {0};
    uint32_t r0;
    uint32_t r1;

    config.descriptor_capacity = 4u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;
    assert(turbowasm_wasi_fs_init(&filesystem, &config) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem, 7u,
               (turbowasm_wasi_fs_file){UINT64_C(11), 1u},
               true, "/sandbox", &d0) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem, 42u,
               (turbowasm_wasi_fs_file){UINT64_C(22), 1u},
               true, "/data", &d1) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_preopen_count(&filesystem) == 2u);

    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_get_directories(
               &bridge, &result) == TURBOWASM_OK);

    assert(result.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.list.count == 2u);
    assert(result.as.list.items[0].kind == TURBOWASM_WASI02_VALUE_TUPLE);
    assert(result.as.list.items[0].as.tuple.count == 2u);
    assert(result.as.list.items[0].as.tuple.items[0].kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    assert(result.as.list.items[0].as.tuple.items[1].kind ==
           TURBOWASM_WASI02_VALUE_STRING);
    assert(result.as.list.items[0].as.tuple.items[1].as.string.size == 8u);
    assert(memcmp(
               result.as.list.items[0].as.tuple.items[1].as.string.data,
               "/sandbox", 8u) == 0);

    r0 = result.as.list.items[0].as.tuple.items[0].as.resource;
    r1 = result.as.list.items[1].as.tuple.items[0].as.resource;
    assert(r0 != 0u && r1 != 0u && r0 != r1);

    assert(turbowasm_wasi02_filesystem_descriptor_resolve(
               &bridge, r0, &info) == TURBOWASM_OK);
    assert(info.preopen);
    assert(info.descriptor.slot == d0.slot);
    assert(info.descriptor.generation == d0.generation);
    assert(strcmp(info.guest_path, "/sandbox") == 0);

    /* Logical WIT drop does not close the host-owned preopen backing. */
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, r1) == TURBOWASM_OK);
    assert(fake.close_calls == 0u);
    assert(turbowasm_wasi_fs_descriptor_info_get(
               &filesystem, 42u, &info));

    /* Stale resource cannot follow a reused filesystem slot. */
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, d0) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(fake.close_calls == 1u);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem, 7u,
               (turbowasm_wasi_fs_file){UINT64_C(33), 1u},
               true, "/replacement", &replacement) == TURBOWASM_OK);
    assert(replacement.slot == d0.slot);
    assert(replacement.generation != d0.generation);
    assert(turbowasm_wasi02_filesystem_descriptor_resolve(
               &bridge, r0, &info) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, r0) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);

    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, replacement) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, d1) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(&filesystem) == TURBOWASM_OK);
    assert(fake.close_calls == 3u);
}

static uint32_t take_success_resource(
    turbowasm_wasi02_value *result) {
    uint32_t resource;

    assert(result != NULL);
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    resource = result->as.result.value->as.resource;
    turbowasm_wasi02_value_destroy(result);
    return resource;
}

static void assert_unit_success(
    turbowasm_wasi02_value *result) {
    assert(result != NULL);
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value == NULL);
    turbowasm_wasi02_value_destroy(result);
}

static void test_child_descriptor_ownership_and_path_ops(void) {
    static const uint8_t child_path[] = "child";
    static const uint8_t grandchild_path[] = "grand";
    static const uint8_t create_path[] = "newdir";
    static const uint8_t remove_path[] = "olddir";
    static const uint8_t unlink_path[] = "file";
    fake_fs fake = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_descriptor preopen = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi02_value directories = {0};
    turbowasm_wasi02_value result = {0};
    uint32_t preopen_resource;
    uint32_t child_resource;
    uint32_t grandchild_resource;

    fake.next_file = UINT64_C(100);
    config.descriptor_capacity = 8u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;
    config.provider.path_open = fake_path_open;
    config.provider.path_create_directory = fake_create_directory;
    config.provider.path_remove_directory = fake_remove_directory;
    config.provider.path_unlink_file = fake_unlink_file;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem, 3u,
               (turbowasm_wasi_fs_file){UINT64_C(1), 1u},
               true, "/", &preopen) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_get_directories(
               &bridge, &directories) == TURBOWASM_OK);
    assert(directories.as.list.count == 1u);
    preopen_resource =
        directories.as.list.items[0]
            .as.tuple.items[0].as.resource;

    assert(turbowasm_wasi02_filesystem_open_at(
               &bridge,
               preopen_resource,
               0u,
               (turbowasm_wasi02_string_view){
                   child_path, sizeof(child_path) - 1u},
               UINT32_C(0x2),
               UINT32_C(0x21),
               &result) == TURBOWASM_OK);
    child_resource = take_success_resource(&result);
    assert(fake.open_calls == 1u);

    assert(turbowasm_wasi02_filesystem_create_directory_at(
               &bridge,
               child_resource,
               (turbowasm_wasi02_string_view){
                   create_path, sizeof(create_path) - 1u},
               &result) == TURBOWASM_OK);
    assert_unit_success(&result);
    assert(fake.create_calls == 1u);

    assert(turbowasm_wasi02_filesystem_remove_directory_at(
               &bridge,
               child_resource,
               (turbowasm_wasi02_string_view){
                   remove_path, sizeof(remove_path) - 1u},
               &result) == TURBOWASM_OK);
    assert_unit_success(&result);
    assert(fake.remove_calls == 1u);

    assert(turbowasm_wasi02_filesystem_unlink_file_at(
               &bridge,
               child_resource,
               (turbowasm_wasi02_string_view){
                   unlink_path, sizeof(unlink_path) - 1u},
               &result) == TURBOWASM_OK);
    assert_unit_success(&result);
    assert(fake.unlink_calls == 1u);

    /* Child directories retain PATH_OPEN rights and are valid open-at bases. */
    assert(turbowasm_wasi02_filesystem_open_at(
               &bridge,
               child_resource,
               0u,
               (turbowasm_wasi02_string_view){
                   grandchild_path, sizeof(grandchild_path) - 1u},
               UINT32_C(0x2),
               UINT32_C(0x21),
               &result) == TURBOWASM_OK);
    grandchild_resource = take_success_resource(&result);
    assert(fake.open_calls == 2u);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, grandchild_resource) == TURBOWASM_OK);
    assert(fake.close_calls == 1u);
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, child_resource) == TURBOWASM_OK);
    assert(fake.close_calls == 2u);

    /* Preopen remains non-owning at the bridge boundary. */
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, preopen_resource) == TURBOWASM_OK);
    assert(fake.close_calls == 2u);

    turbowasm_wasi02_value_destroy(&directories);
    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, preopen) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(fake.close_calls == 3u);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_preopen_projection_and_generation();
    test_child_descriptor_ownership_and_path_ops();
    return 0;
}
