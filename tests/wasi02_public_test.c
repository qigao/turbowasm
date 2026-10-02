#include <turbowasm/wasi02.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct clock_probe {
    uint32_t calls;
    uint64_t value;
} clock_probe;

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *out_value) {
    clock_probe *probe = (clock_probe *)context;

    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->calls;
    *out_value = probe->value;
    return TURBOWASM_OK;
}

static turbowasm_status socket_poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    (void)context;
    (void)rep;
    if (out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_ready = true;
    return TURBOWASM_OK;
}

static turbowasm_status socket_poll_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
    return TURBOWASM_OK;
}

static void socket_stream_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
}

static turbowasm_status socket_instance_network(
    void *context,
    turbowasm_value *out_rep) {
    (void)context;
    if (out_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 1;
    return TURBOWASM_OK;
}

static turbowasm_status socket_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
    return TURBOWASM_OK;
}

static turbowasm_status socket_tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    (void)context;
    if (out_rep == NULL || out_error == NULL ||
        (family != TURBOWASM_WASI02_IP_ADDRESS_IPV4 &&
         family != TURBOWASM_WASI02_IP_ADDRESS_IPV6))
        return TURBOWASM_INVALID_ARGUMENT;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 2;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static const uint8_t monotonic_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x23,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7e,
      0x02,0x09,0x01,
        0x01,'p',
        0x03,'n','o','w',
        0x00,0x00,
      0x07,0x07,0x01,
        0x03,'r','u','n',
        0x00,0x00,
    0x07,0x14,0x02,
      0x42,0x02,
        0x01,0x40,0x00,0x00,0x77,
        0x04,0x00,0x03,'n','o','w',0x01,0x00,
      0x40,0x00,0x00,0x77,
    0x0a,0x26,0x01,
      0x00,0x21,
      'w','a','s','i',':','c','l','o','c','k','s','/',
      'm','o','n','o','t','o','n','i','c','-',
      'c','l','o','c','k','@','0','.','2','.','8',
      0x05,0x00,
    0x06,0x08,0x01,
      0x01,0x00,0x00,0x03,'n','o','w',
    0x08,0x05,0x01,
      0x01,0x00,0x00,0x00,
    0x02,0x10,0x02,
      0x01,0x01,
        0x03,'n','o','w',0x00,0x00,
      0x00,0x00,0x01,
        0x01,'p',0x12,0x00,
    0x06,0x09,0x01,
      0x00,0x00,0x01,0x01,0x03,'r','u','n',
    0x08,0x06,0x01,
      0x00,0x00,0x01,0x00,0x01,
    0x0b,0x09,0x01,
      0x00,0x03,'r','u','n',
      0x01,0x01,0x00
};

static turbowasm_name run_name(void) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)"run";
    name.size = 3u;
    return name;
}

int main(void) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_wasi02_config config = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 0u;
    clock_probe probe = {0};

    probe.value = UINT64_C(987654321);
    config.provider.context = &probe;
    config.provider.monotonic_clock_now = monotonic_now;

    config.poll.ready = socket_poll_ready;
    config.poll.drop = socket_poll_drop;
    config.pollable_capacity = 4u;
    config.streams.input_drop = socket_stream_drop;
    config.streams.output_drop = socket_stream_drop;
    config.stream_resource_capacity = 4u;
    config.sockets.instance_network = socket_instance_network;
    config.sockets.network_drop = socket_drop;
    config.sockets.tcp_create = socket_tcp_create;
    config.sockets.tcp_drop = socket_drop;
    config.socket_network_resource_capacity = 2u;
    config.tcp_socket_resource_capacity = 4u;

    {
        turbowasm_wasi02 invalid = {0};
        turbowasm_wasi02_config half = config;
        half.tcp_socket_resource_capacity = 0u;
        assert(turbowasm_wasi02_init(
                   &invalid, &half, NULL) ==
               TURBOWASM_INVALID_ARGUMENT);
        assert(invalid.impl == NULL);
    }

    assert(turbowasm_wasi02_init(
               &wasi02, &config, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_load_borrowed(
               &component,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_instance_create(
               &instance,
               &component,
               &wasi02) == TURBOWASM_OK);

    /* Public WASI02 ownership is strict while a bound instance is live. */
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_INVALID_ARGUMENT);

    /*
     * Instance retains the decoded Component state. Dropping the public
     * Component handle does not invalidate the bound instance.
     */
    turbowasm_component_destroy(&component);

    assert(turbowasm_component_instance_invoke(
               &instance,
               run_name(),
               NULL,
               0u,
               &result,
               1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U64);
    assert(result.as.u64 == probe.value);
    assert(probe.calls == 1u);

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);

    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    assert(wasi02.impl == NULL);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    return 0;
}
