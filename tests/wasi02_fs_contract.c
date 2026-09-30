#include "wasi02_fs_contract.h"

#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
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
    turbowasm_wasi02_value args[5] = {{0}};
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

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, file) == TURBOWASM_OK);

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
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, root) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);

    fixture->teardown(fixture->context);
    result_code = 0;
    return result_code;
}
