#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_fs {
    uint32_t close_calls;
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

int main(void) {
    test_preopen_projection_and_generation();
    return 0;
}
