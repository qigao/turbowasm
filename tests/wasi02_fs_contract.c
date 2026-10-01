#include "wasi02_fs_contract.h"

#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

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

typedef struct wasi02_stream_factory_probe {
    uint32_t read_factory_calls;
    uint32_t write_factory_calls;
    uint32_t append_factory_calls;
    uint32_t input_read_calls;
    uint32_t output_check_calls;
    uint32_t output_write_calls;
    uint32_t input_drop_calls;
    uint32_t output_drop_calls;
    turbowasm_wasi_fs_file last_file;
    uint64_t last_offset;
    int64_t next_rep;
} wasi02_stream_factory_probe;

static uint32_t fs_read_stream_factory(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t offset,
    turbowasm_value *out_stream_rep) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;
    assert(probe != NULL && out_stream_rep != NULL);
    ++probe->read_factory_calls;
    probe->last_file = file;
    probe->last_offset = offset;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = ++probe->next_rep;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fs_write_stream_factory(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t offset,
    turbowasm_value *out_stream_rep) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;
    assert(probe != NULL && out_stream_rep != NULL);
    ++probe->write_factory_calls;
    probe->last_file = file;
    probe->last_offset = offset;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = ++probe->next_rep;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fs_append_stream_factory(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_value *out_stream_rep) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;
    assert(probe != NULL && out_stream_rep != NULL);
    ++probe->append_factory_calls;
    probe->last_file = file;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = ++probe->next_rep;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_status produced_input_read(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error) {
    static const uint8_t payload[] = {'f','s'};
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(out_data != NULL && out_size != NULL &&
           out_error != NULL);
    ++probe->input_read_calls;
    memset(out_error, 0, sizeof(*out_error));
    if (max_bytes == 0u) {
        *out_data = NULL;
        *out_size = 0u;
    } else {
        *out_data = payload;
        *out_size = max_bytes < 2u ? (size_t)max_bytes : 2u;
    }
    return TURBOWASM_OK;
}

static turbowasm_status produced_output_check_write(
    void *context,
    turbowasm_value stream_rep,
    uint64_t *out_permit,
    turbowasm_wasi02_stream_error *out_error) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(out_permit != NULL && out_error != NULL);
    ++probe->output_check_calls;
    *out_permit = 64u;
    memset(out_error, 0, sizeof(*out_error));
    return TURBOWASM_OK;
}

static turbowasm_status produced_output_write(
    void *context,
    turbowasm_value stream_rep,
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_stream_error *out_error) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;

    assert(probe != NULL);
    assert(stream_rep.kind == TURBOWASM_VALUE_I64);
    assert(size == 0u || data != NULL);
    assert(out_error != NULL);
    ++probe->output_write_calls;
    memset(out_error, 0, sizeof(*out_error));
    return TURBOWASM_OK;
}

static void produced_input_drop(
    void *context,
    turbowasm_value rep) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    ++probe->input_drop_calls;
}

static void produced_output_drop(
    void *context,
    turbowasm_value rep) {
    wasi02_stream_factory_probe *probe =
        (wasi02_stream_factory_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    ++probe->output_drop_calls;
}

static void set_u64(
    turbowasm_wasi02_value *value,
    uint64_t number) {
    memset(value, 0, sizeof(*value));
    value->kind = TURBOWASM_WASI02_VALUE_U64;
    value->as.u64 = number;
}

static uint32_t get_first_preopen(
    turbowasm_wasi02_filesystem *bridge) {
    turbowasm_wasi02_value dirs = {0};
    uint32_t resource;

    assert(turbowasm_wasi02_filesystem_get_directories(
               bridge, &dirs) == TURBOWASM_OK);
    assert(dirs.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(dirs.as.list.count == 1u);
    assert(dirs.as.list.items[0].kind ==
           TURBOWASM_WASI02_VALUE_TUPLE);
    assert(dirs.as.list.items[0].as.tuple.count == 2u);
    resource =
        dirs.as.list.items[0].as.tuple.items[0].as.resource;
    assert(resource != 0u);
    turbowasm_wasi02_value_destroy(&dirs);
    return resource;
}

static uint32_t expect_resource_ok(
    turbowasm_wasi02_value *result) {
    uint32_t resource;

    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    assert(result->as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_RESOURCE);
    resource = result->as.result.value->as.resource;
    assert(resource != 0u);
    return resource;
}

static void expect_unit_ok(
    turbowasm_wasi02_value *result) {
    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value == NULL);
}

static void expect_stat_ok(
    turbowasm_wasi02_value *result,
    uint32_t expected_type) {
    turbowasm_wasi02_value *record;

    assert(result->kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result->as.result.is_error);
    assert(result->as.result.value != NULL);
    record = result->as.result.value;
    assert(record->kind == TURBOWASM_WASI02_VALUE_RECORD);
    assert(record->as.record.count == 6u);
    assert(record->as.record.items[0].kind ==
           TURBOWASM_WASI02_VALUE_ENUM);
    assert(record->as.record.items[0].as.enum_index ==
           expected_type);
    assert(record->as.record.items[1].kind ==
           TURBOWASM_WASI02_VALUE_U64);
    assert(record->as.record.items[2].kind ==
           TURBOWASM_WASI02_VALUE_U64);
    assert(record->as.record.items[3].kind ==
           TURBOWASM_WASI02_VALUE_OPTION);
    assert(record->as.record.items[4].kind ==
           TURBOWASM_WASI02_VALUE_OPTION);
    assert(record->as.record.items[5].kind ==
           TURBOWASM_WASI02_VALUE_OPTION);
}

int turbowasm_wasi02_fs_contract_run(
    const turbowasm_wasi_fs_contract_fixture *fixture) {
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root_file = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_filesystem_stream_provider
        stream_factories = {0};
    wasi02_stream_factory_probe stream_probe = {0};
    turbowasm_wasi02_value args[5] = {{0}};
    turbowasm_wasi02_value stream_args[2] = {{0}};
    turbowasm_wasi02_value stream_byte = {0};
    turbowasm_wasi02_value result = {0};
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
    uint32_t dir;
    uint32_t file;
    uint32_t input_stream;
    uint32_t output_stream;
    uint32_t append_stream;
    int result_code = 1;

    if (fixture == NULL || fixture->setup == NULL ||
        fixture->teardown == NULL)
        return 1;

    assert(fixture->setup(
               fixture->context,
               &provider,
               &root_file) == TURBOWASM_OK);

    config.descriptor_capacity = 16u;
    config.provider = provider;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem,
               3u,
               root_file,
               true,
               "/",
               all_rights,
               all_rights,
               &root) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 32u) == TURBOWASM_OK);

    stream_probe.next_rep = 100;
    stream_provider.context = &stream_probe;
    stream_provider.input_read = produced_input_read;
    stream_provider.output_check_write =
        produced_output_check_write;
    stream_provider.output_write = produced_output_write;
    stream_provider.input_drop = produced_input_drop;
    stream_provider.output_drop = produced_output_drop;
    assert(turbowasm_wasi02_streams_init(
               &streams, &stream_provider, 16u) == TURBOWASM_OK);

    stream_factories.context = &stream_probe;
    stream_factories.read_via_stream = fs_read_stream_factory;
    stream_factories.write_via_stream = fs_write_stream_factory;
    stream_factories.append_via_stream = fs_append_stream_factory;
    assert(turbowasm_wasi02_filesystem_attach_streams(
               &bridge, &streams, &stream_factories) == TURBOWASM_OK);

    preopen = get_first_preopen(&bridge);

    set_resource(&args[0], preopen);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.stat",
               args, 1u, &result) == TURBOWASM_OK);
    expect_stat_ok(&result, 3u);
    turbowasm_wasi02_value_destroy(&result);

    /* directory lifecycle */
    set_string(&args[1], "dir");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.create-directory-at",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    /* same path-stat contract on both providers */
    set_resource(&args[0], preopen);
    set_flags(&args[1], 0u);
    set_string(&args[2], "dir");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.stat-at",
               args, 3u, &result) == TURBOWASM_OK);
    expect_stat_ok(&result, 3u);
    turbowasm_wasi02_value_destroy(&result);

    set_flags(&args[1], 0u);
    set_string(&args[2], "dir");
    set_flags(&args[3], 2u);  /* open-flags::directory */
    set_flags(&args[4], 32u); /* descriptor-flags::mutate-directory */
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    dir = expect_resource_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    set_resource(&args[0], dir);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.stat",
               args, 1u, &result) == TURBOWASM_OK);
    expect_stat_ok(&result, 3u);
    turbowasm_wasi02_value_destroy(&result);

    /* create a regular file inside the child directory. */
    set_flags(&args[1], 0u);
    set_string(&args[2], "file");
    set_flags(&args[3], 1u); /* open-flags::create */
    set_flags(&args[4], 3u); /* read|write */
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.open-at",
               args, 5u, &result) == TURBOWASM_OK);
    file = expect_resource_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    set_resource(&args[0], file);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.stat",
               args, 1u, &result) == TURBOWASM_OK);
    expect_stat_ok(&result, 6u);
    turbowasm_wasi02_value_destroy(&result);

    /*
     * Produce all three 0.2 stream forms from the live regular-file
     * descriptor. The factory receives the backend's retained file identity,
     * but the returned stream rep is an independent provider capability.
     */
    set_resource(&args[0], file);
    set_u64(&args[1], 5u);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.read-via-stream",
               args, 2u, &result) == TURBOWASM_OK);
    input_stream = expect_resource_ok(&result);
    turbowasm_wasi02_value_destroy(&result);
    assert(stream_probe.read_factory_calls == 1u);
    assert(stream_probe.last_offset == 5u);

    set_u64(&args[1], 7u);
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.write-via-stream",
               args, 2u, &result) == TURBOWASM_OK);
    output_stream = expect_resource_ok(&result);
    turbowasm_wasi02_value_destroy(&result);
    assert(stream_probe.write_factory_calls == 1u);
    assert(stream_probe.last_offset == 7u);

    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.append-via-stream",
               args, 1u, &result) == TURBOWASM_OK);
    append_stream = expect_resource_ok(&result);
    turbowasm_wasi02_value_destroy(&result);
    assert(stream_probe.append_factory_calls == 1u);

    /*
     * Descriptor and stream lifetimes are independent. Closing the descriptor
     * does not consume any factory-produced stream resource.
     */
    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, file) == TURBOWASM_OK);

    set_resource(&stream_args[0], input_stream);
    set_u64(&stream_args[1], 2u);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]input-stream.read",
               stream_args, 2u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.result.value->as.list.count == 2u);
    assert(result.as.result.value->as.list.items[0].as.u8 ==
           (uint8_t)'f');
    assert(result.as.result.value->as.list.items[1].as.u8 ==
           (uint8_t)'s');
    turbowasm_wasi02_value_destroy(&result);

    set_resource(&stream_args[0], output_stream);
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.check-write",
               stream_args, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RESULT);
    assert(!result.as.result.is_error);
    assert(result.as.result.value != NULL);
    assert(result.as.result.value->kind ==
           TURBOWASM_WASI02_VALUE_U64);
    assert(result.as.result.value->as.u64 == 64u);
    turbowasm_wasi02_value_destroy(&result);

    stream_byte.kind = TURBOWASM_WASI02_VALUE_U8;
    stream_byte.as.u8 = (uint8_t)'x';
    stream_args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
    stream_args[1].as.list.items = &stream_byte;
    stream_args[1].as.list.count = 1u;
    assert(turbowasm_wasi02_streams_call(
               &streams,
               "streams",
               "[method]output-stream.write",
               stream_args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    assert(stream_probe.input_read_calls == 1u);
    assert(stream_probe.output_check_calls == 1u);
    assert(stream_probe.output_write_calls == 1u);

    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, input_stream) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, output_stream) == TURBOWASM_OK);
    assert(turbowasm_wasi02_stream_resource_drop(
               &streams, append_stream) == TURBOWASM_OK);
    assert(stream_probe.input_drop_calls == 1u);
    assert(stream_probe.output_drop_calls == 2u);

    set_resource(&args[0], dir);
    set_string(&args[1], "file");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.unlink-file-at",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, dir) == TURBOWASM_OK);

    set_resource(&args[0], preopen);
    set_string(&args[1], "dir");
    assert(turbowasm_wasi02_filesystem_call(
               &bridge,
               "[method]descriptor.remove-directory-at",
               args, 2u, &result) == TURBOWASM_OK);
    expect_unit_ok(&result);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, preopen) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, root) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);

    fixture->teardown(fixture->context);
    result_code = 0;
    return result_code;
}
