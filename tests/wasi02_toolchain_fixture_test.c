#include <turbowasm/wasi02.h>

#include "wasi02_toolchain_fixtures.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    uint32_t socket_network_calls;
    uint32_t socket_network_drop_calls;
    uint32_t socket_tcp_create_calls;
    uint32_t socket_tcp_drop_calls;
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

static turbowasm_status socket_instance_network(
    void *context,
    turbowasm_value *out_rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->socket_network_calls;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 99;
    return TURBOWASM_OK;
}

static turbowasm_status socket_network_drop(
    void *context,
    turbowasm_value rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL ||
        rep.kind != TURBOWASM_VALUE_I32 ||
        rep.as.i32 != 99)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->socket_network_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status socket_tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_rep == NULL || out_error == NULL ||
        (family != TURBOWASM_WASI02_IP_ADDRESS_IPV4 &&
         family != TURBOWASM_WASI02_IP_ADDRESS_IPV6))
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->socket_tcp_create_calls;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 100;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status socket_tcp_drop(
    void *context,
    turbowasm_value rep) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL ||
        rep.kind != TURBOWASM_VALUE_I32 ||
        rep.as.i32 != 100)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->socket_tcp_drop_calls;
    return TURBOWASM_OK;
}

static void output_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
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

static turbowasm_name run_name(void) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)"run";
    name.size = 3u;
    return name;
}

static uint64_t run_integer_fixture(
    turbowasm_wasi02 *wasi02,
    const char *label,
    const uint8_t *bytes,
    size_t size,
    turbowasm_component_host_value_kind expected_kind) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 0u;
    uint64_t value;
    turbowasm_status status;

    assert(label != NULL);
    assert(bytes != NULL && size > 8u);
    status = turbowasm_component_load_borrowed(
        &component, bytes, size);
    if (status != TURBOWASM_OK) {
        fprintf(
            stderr,
            "WASI02 toolchain fixture %s load failed: %d (%s), size=%zu\n",
            label,
            (int)status,
            turbowasm_status_string(status),
            size);
    }
    assert(status == TURBOWASM_OK);
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
    return value;
}

static void run_exit_fixture(
    turbowasm_wasi02 *wasi02) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 99u;

    assert(turbowasm_component_load_borrowed(
               &component,
               turbowasm_wasi02_fixture_cli_exit,
               turbowasm_wasi02_fixture_cli_exit_size) ==
           TURBOWASM_OK);
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
    config.streams.output_drop = output_drop;
    config.stream_resource_capacity = 8u;

    config.sockets.context = &probe;
    config.sockets.instance_network = socket_instance_network;
    config.sockets.network_drop = socket_network_drop;
    config.sockets.tcp_create = socket_tcp_create;
    config.sockets.tcp_drop = socket_tcp_drop;
    config.socket_network_resource_capacity = 4u;
    config.tcp_socket_resource_capacity = 4u;

    assert(turbowasm_wasi02_init(
               &wasi02, &config, NULL) == TURBOWASM_OK);

    assert(run_integer_fixture(
               &wasi02,
               "monotonic-clock",
               turbowasm_wasi02_fixture_monotonic_clock,
               turbowasm_wasi02_fixture_monotonic_clock_size,
               TURBOWASM_COMPONENT_HOST_U64) ==
           probe.clock_value);
    assert(run_integer_fixture(
               &wasi02,
               "random-u64",
               turbowasm_wasi02_fixture_random_u64,
               turbowasm_wasi02_fixture_random_u64_size,
               TURBOWASM_COMPONENT_HOST_U64) ==
           probe.random_value);
    run_exit_fixture(&wasi02);

    assert(run_integer_fixture(
               &wasi02,
               "filesystem-preopens",
               turbowasm_wasi02_fixture_filesystem_preopens,
               turbowasm_wasi02_fixture_filesystem_preopens_size,
               TURBOWASM_COMPONENT_HOST_U32) == 1u);
    assert(run_integer_fixture(
               &wasi02,
               "stream-poll-block",
               turbowasm_wasi02_fixture_stream_poll_block,
               turbowasm_wasi02_fixture_stream_poll_block_size,
               TURBOWASM_COMPONENT_HOST_U32) == 1u);
    assert(run_integer_fixture(
               &wasi02,
               "socket-instance-network",
               turbowasm_wasi02_fixture_socket_instance_network,
               turbowasm_wasi02_fixture_socket_instance_network_size,
               TURBOWASM_COMPONENT_HOST_U32) == 1u);
    assert(run_integer_fixture(
               &wasi02,
               "socket-create-tcp",
               turbowasm_wasi02_fixture_socket_create_tcp,
               turbowasm_wasi02_fixture_socket_create_tcp_size,
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

    assert(probe.socket_network_calls == 1u);
    assert(probe.socket_network_drop_calls == 1u);
    assert(probe.socket_tcp_create_calls == 1u);
    assert(probe.socket_tcp_drop_calls == 1u);

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
