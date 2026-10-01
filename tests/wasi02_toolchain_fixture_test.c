#include <turbowasm/wasi02.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TURBOWASM_WASI02_FIXTURE_DIR
#error "TURBOWASM_WASI02_FIXTURE_DIR must be defined"
#endif

typedef struct fixture_probe {
    uint64_t clock_value;
    uint64_t random_value;

    uint32_t clock_calls;
    uint32_t random_calls;
    uint32_t exit_calls;
    bool exit_success;

    uint32_t stdin_calls;
    uint32_t subscribe_calls;
    uint32_t input_drop_calls;
    uint32_t poll_ready_calls;
    uint32_t poll_drop_calls;

    uint32_t fs_close_calls;
} fixture_probe;

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *out_value) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->clock_calls;
    *out_value = probe->clock_value;
    return TURBOWASM_OK;
}

static turbowasm_status random_u64(
    void *context,
    uint64_t *out_value) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->random_calls;
    *out_value = probe->random_value;
    return TURBOWASM_OK;
}

static turbowasm_status exit_status(
    void *context,
    bool success) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->exit_calls;
    probe->exit_success = success;
    return TURBOWASM_OK;
}

static turbowasm_status get_stdin(
    void *context,
    turbowasm_value *out_stream_rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_stream_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->stdin_calls;
    out_stream_rep->kind = TURBOWASM_VALUE_I64;
    out_stream_rep->as.i64 = 11;
    return TURBOWASM_OK;
}

static turbowasm_status input_subscribe(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_pollable_rep == NULL ||
        stream_rep.kind != TURBOWASM_VALUE_I64 ||
        stream_rep.as.i64 != 11)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->subscribe_calls;
    out_pollable_rep->kind = TURBOWASM_VALUE_I64;
    out_pollable_rep->as.i64 = 77;
    return TURBOWASM_OK;
}

static void input_drop(
    void *context,
    turbowasm_value rep) {
    fixture_probe *probe = (fixture_probe *)context;
    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I64);
    assert(rep.as.i64 == 11);
    ++probe->input_drop_calls;
}

static turbowasm_status poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_ready == NULL ||
        rep.kind != TURBOWASM_VALUE_I64 ||
        rep.as.i64 != 77)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->poll_ready_calls;
    *out_ready = true;
    return TURBOWASM_OK;
}

static turbowasm_status poll_drop(
    void *context,
    turbowasm_value rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL ||
        rep.kind != TURBOWASM_VALUE_I64 ||
        rep.as.i64 != 77)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->poll_drop_calls;
    return TURBOWASM_OK;
}

static uint32_t fs_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    fixture_probe *probe = (fixture_probe *)context;
    (void)file;
    assert(probe != NULL);
    ++probe->fs_close_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fs_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_read != NULL)
        *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fs_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    (void)context;
    (void)file;
    (void)buffers;
    (void)buffer_count;
    if (out_written != NULL)
        *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint8_t *read_fixture(
    const char *name,
    size_t *out_size) {
    char path[1024];
    FILE *file;
    long length;
    uint8_t *bytes;

    assert(name != NULL);
    assert(out_size != NULL);
    assert(snprintf(
               path, sizeof(path),
               "%s/%s",
               TURBOWASM_WASI02_FIXTURE_DIR,
               name) > 0);

    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 8);
    assert(fseek(file, 0, SEEK_SET) == 0);

    bytes = (uint8_t *)malloc((size_t)length);
    assert(bytes != NULL);
    assert(fread(bytes, 1u, (size_t)length, file) ==
           (size_t)length);
    assert(fclose(file) == 0);

    assert(bytes[0] == 0x00u);
    assert(bytes[1] == 0x61u);
    assert(bytes[2] == 0x73u);
    assert(bytes[3] == 0x6du);
    assert(bytes[4] == 0x0du);
    assert(bytes[5] == 0x00u);
    assert(bytes[6] == 0x01u);
    assert(bytes[7] == 0x00u);

    *out_size = (size_t)length;
    return bytes;
}

static turbowasm_name run_name(void) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)"run";
    name.size = 3u;
    return name;
}

static uint64_t run_integer_fixture(
    turbowasm_wasi02 *wasi02,
    const char *filename,
    turbowasm_component_host_value_kind expected_kind) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 0u;
    size_t size = 0u;
    uint8_t *bytes = read_fixture(filename, &size);
    uint64_t value;

    assert(turbowasm_component_load_borrowed(
               &component, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_instance_create(
               &instance, &component, wasi02) == TURBOWASM_OK);
    assert(turbowasm_component_instance_invoke(
               &instance,
               run_name(),
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == expected_kind);

    if (expected_kind == TURBOWASM_COMPONENT_HOST_U64)
        value = result.as.u64;
    else {
        assert(expected_kind == TURBOWASM_COMPONENT_HOST_U32);
        value = result.as.u32;
    }

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
    free(bytes);
    return value;
}

static void run_exit_fixture(
    turbowasm_wasi02 *wasi02) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 99u;
    size_t size = 0u;
    uint8_t *bytes = read_fixture(
        "cli-exit.wasm", &size);

    assert(turbowasm_component_load_borrowed(
               &component, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_instance_create(
               &instance, &component, wasi02) == TURBOWASM_OK);
    assert(turbowasm_component_instance_invoke(
               &instance,
               run_name(),
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_INTERRUPTED);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 0u);

    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
    free(bytes);
}

int main(void) {
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_wasi02_config config = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config fs_config = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    fixture_probe probe = {0};

    probe.clock_value = UINT64_C(0x1122334455667788);
    probe.random_value = UINT64_C(0x8877665544332211);

    fs_config.descriptor_capacity = 8u;
    fs_config.provider.context = &probe;
    fs_config.provider.close = fs_close;
    fs_config.provider.read = fs_read;
    fs_config.provider.write = fs_write;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &fs_config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               3u,
               (turbowasm_wasi_fs_file){UINT64_C(55), 1u},
               true,
               "/",
               &root) == TURBOWASM_OK);

    config.provider.context = &probe;
    config.provider.monotonic_clock_now = monotonic_now;
    config.provider.random_u64 = random_u64;
    config.provider.exit = exit_status;

    config.filesystem = &filesystem;
    config.filesystem_resource_capacity = 8u;

    config.poll.context = &probe;
    config.poll.ready = poll_ready;
    config.poll.drop = poll_drop;
    config.pollable_capacity = 8u;

    config.streams.context = &probe;
    config.streams.get_stdin = get_stdin;
    config.streams.input_subscribe = input_subscribe;
    config.streams.input_drop = input_drop;
    config.stream_resource_capacity = 8u;

    assert(turbowasm_wasi02_init(
               &wasi02, &config, NULL) == TURBOWASM_OK);

    assert(run_integer_fixture(
               &wasi02,
               "monotonic-clock.wasm",
               TURBOWASM_COMPONENT_HOST_U64) ==
           probe.clock_value);
    assert(run_integer_fixture(
               &wasi02,
               "random-u64.wasm",
               TURBOWASM_COMPONENT_HOST_U64) ==
           probe.random_value);
    run_exit_fixture(&wasi02);

    assert(run_integer_fixture(
               &wasi02,
               "filesystem-preopens.wasm",
               TURBOWASM_COMPONENT_HOST_U32) == 1u);
    assert(run_integer_fixture(
               &wasi02,
               "stream-poll-block.wasm",
               TURBOWASM_COMPONENT_HOST_U32) == 1u);

    assert(probe.clock_calls == 1u);
    assert(probe.random_calls == 1u);
    assert(probe.exit_calls == 1u);
    assert(probe.exit_success);

    assert(probe.stdin_calls == 1u);
    assert(probe.subscribe_calls == 1u);
    assert(probe.poll_ready_calls == 1u);
    assert(probe.poll_drop_calls == 1u);
    assert(probe.input_drop_calls == 1u);

    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, root) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
    assert(probe.fs_close_calls == 1u);
    return 0;
}
