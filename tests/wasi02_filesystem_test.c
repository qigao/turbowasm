#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct fake_fs {
    uint32_t close_calls;
} fake_fs;

typedef struct fake_streams {
    uint32_t factory_calls;
    uint32_t read_calls;
    uint32_t drop_calls;
    turbowasm_wasi_fs_file file;
    uint64_t offset;
} fake_streams;

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

static uint32_t fake_read_via_stream(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t offset,
    turbowasm_value *out_stream_rep) {
    fake_streams *state = (fake_streams *)context;
    assert(state != NULL && out_stream_rep != NULL);
    ++state->factory_calls;
    state->file = file;
    state->offset = offset;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = 99;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_status fake_stream_read(
    void *context,
    turbowasm_value rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    static const uint8_t bytes[] = {'r','d'};
    fake_streams *state = (fake_streams *)context;
    assert(state != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64 && rep.as.i64 == 99);
    assert(out_data != NULL && out_size != NULL && out_error != NULL);
    ++state->read_calls;
    memset(out_error, 0, sizeof(*out_error));
    *out_data = max_bytes == 0u ? NULL : bytes;
    *out_size = max_bytes == 0u
        ? 0u
        : (max_bytes < 2u ? (size_t)max_bytes : 2u);
    return TURBOWASM_OK;
}

static void fake_stream_drop(
    void *context,
    turbowasm_value rep) {
    fake_streams *state = (fake_streams *)context;
    assert(state != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64 && rep.as.i64 == 99);
    ++state->drop_calls;
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

static void test_read_via_stream_lifetime(void) {
    fake_fs fake = {0};
    fake_streams stream_state = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_filesystem_stream_provider factories = {0};
    turbowasm_wasi02_value dirs = {0};
    turbowasm_wasi02_value args[2] = {{0}};
    turbowasm_wasi02_value result = {0};
    uint32_t descriptor;
    uint32_t input_stream;

    config.descriptor_capacity = 4u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem, 3u,
               (turbowasm_wasi_fs_file){UINT64_C(55), 4u},
               true, "/", &root) == TURBOWASM_OK);

    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 4u) == TURBOWASM_OK);

    stream_provider.context = &stream_state;
    stream_provider.input_read = fake_stream_read;
    stream_provider.input_drop = fake_stream_drop;
    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider, 4u) == TURBOWASM_OK);

    factories.context = &stream_state;
    factories.read_via_stream = fake_read_via_stream;
    assert(turbowasm_wasi02_filesystem_attach_streams(
               &bridge, &streams, &factories) == TURBOWASM_OK);

    assert(turbowasm_wasi02_filesystem_get_directories(
               &bridge, &dirs) == TURBOWASM_OK);
    descriptor =
        dirs.as.list.items[0].as.tuple.items[0].as.resource;
    turbowasm_wasi02_value_destroy(&dirs);

    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = descriptor;
    args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    args[1].as.u64 = 9u;
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.read-via-stream",
               args, 2u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    input_stream = result.as.result.value->as.resource;
    turbowasm_wasi02_value_destroy(&result);

    assert(stream_state.factory_calls == 1u);
    assert(stream_state.file.object == UINT64_C(55));
    assert(stream_state.file.generation == 4u);
    assert(stream_state.offset == 9u);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, descriptor) == TURBOWASM_OK);

    args[0].as.resource = input_stream;
    args[1].as.u64 = 2u;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.read",
               args, 2u, &result) == TURBOWASM_OK);
    assert(result.as.result.value->as.list.count == 2u);
    assert(result.as.result.value->as.list.items[0].as.u8 ==
           (uint8_t)'r');
    assert(result.as.result.value->as.list.items[1].as.u8 ==
           (uint8_t)'d');
    turbowasm_wasi02_value_destroy(&result);
    assert(stream_state.read_calls == 1u);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input_stream) == TURBOWASM_OK);
    assert(stream_state.drop_calls == 1u);

    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, root) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_preopen_projection_and_generation();
    test_read_via_stream_lifetime();
    return 0;
}
