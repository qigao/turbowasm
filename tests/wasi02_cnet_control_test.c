#include "../src/wasi02_cnet.h"

#include <turbowasm/wasi02_sockets.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static native_io_backend_kind test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static turbowasm_wasi02_ip_socket_address loopback_v4(void) {
    turbowasm_wasi02_ip_socket_address address;
    memset(&address, 0, sizeof(address));
    address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    address.as.ipv4.port = 0u;
    address.as.ipv4.address[0] = 127u;
    address.as.ipv4.address[3] = 1u;
    return address;
}

static void test_cnet_control_plane(void) {
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_cnet_config config = {0};
    turbowasm_wasi02_socket_provider provider = {0};
    turbowasm_value network = {0};
    turbowasm_value socket = {0};
    turbowasm_wasi02_ip_socket_address local = {0};
    turbowasm_wasi02_ip_socket_address bind = loopback_v4();
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    uint8_t hop = 0u;
    uint64_t receive_buffer = 0u;
    bool keepalive = false;

    config.backend = test_backend();
    config.socket_capacity = 2u;
    config.default_listen_backlog = 8u;

    assert(turbowasm_wasi02_cnet_init(
               &adapter, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter, &provider) == TURBOWASM_OK);

    assert(provider.tcp_start_connect == NULL);
    assert(provider.tcp_finish_connect == NULL);
    assert(provider.tcp_accept == NULL);
    assert(provider.tcp_remote_address == NULL);
    assert(provider.tcp_subscribe == NULL);
    assert(provider.tcp_shutdown == NULL);

    assert(provider.instance_network(
               provider.context, &network) == TURBOWASM_OK);
    assert(network.kind == TURBOWASM_VALUE_I64);

    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &socket, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(socket.kind == TURBOWASM_VALUE_I64);

    assert(provider.tcp_keep_alive_enabled(
               provider.context, socket,
               &keepalive, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_set_keep_alive_enabled(
               provider.context, socket, true,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_keep_alive_enabled(
               provider.context, socket,
               &keepalive, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(keepalive);

    assert(provider.tcp_set_hop_limit(
               provider.context, socket, UINT8_C(64),
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_hop_limit(
               provider.context, socket, &hop,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(hop == UINT8_C(64));

    assert(provider.tcp_receive_buffer_size(
               provider.context, socket,
               &receive_buffer, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(receive_buffer != 0u);

    assert(provider.tcp_start_bind(
               provider.context, socket, network,
               &bind, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_finish_bind(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(provider.tcp_local_address(
               provider.context, socket,
               &local, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(local.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4);
    assert(local.as.ipv4.port != 0u);
    assert(local.as.ipv4.address[0] == 127u);
    assert(local.as.ipv4.address[1] == 0u);
    assert(local.as.ipv4.address[2] == 0u);
    assert(local.as.ipv4.address[3] == 1u);

    assert(provider.tcp_set_listen_backlog_size(
               provider.context, socket, UINT64_C(4),
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_start_listen(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(provider.tcp_finish_listen(
               provider.context, socket,
               &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(provider.tcp_drop(
               provider.context, socket) == TURBOWASM_OK);
    assert(provider.tcp_drop(
               provider.context, socket) == TURBOWASM_TRAPPED);

    assert(provider.network_drop(
               provider.context, network) == TURBOWASM_OK);
    assert(provider.network_drop(
               provider.context, network) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);
}

static void test_cnet_capacity_is_bounded(void) {
    turbowasm_wasi02_cnet adapter = {0};
    turbowasm_wasi02_cnet_config config = {0};
    turbowasm_wasi02_socket_provider provider = {0};
    turbowasm_value first = {0};
    turbowasm_value second = {0};
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    config.backend = test_backend();
    config.socket_capacity = 1u;

    assert(turbowasm_wasi02_cnet_init(
               &adapter, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi02_cnet_socket_provider(
               &adapter, &provider) == TURBOWASM_OK);

    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &first, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);

    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &second, &error) == TURBOWASM_OK);
    assert(error ==
           TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_INVALID_ARGUMENT);
    assert(provider.tcp_drop(
               provider.context, first) == TURBOWASM_OK);

    /*
     * Reuse the sole bounded slot and prove the generation changed. The old
     * provider rep must remain stale even though the physical slot is active
     * again.
     */
    second = (turbowasm_value){0};
    assert(provider.tcp_create(
               provider.context,
               TURBOWASM_WASI02_IP_ADDRESS_IPV4,
               &second, &error) == TURBOWASM_OK);
    assert(error == TURBOWASM_WASI02_SOCKET_ERROR_NONE);
    assert(second.kind == TURBOWASM_VALUE_I64);
    assert(second.as.i64 != first.as.i64);
    assert(provider.tcp_drop(
               provider.context, first) == TURBOWASM_TRAPPED);
    assert(provider.tcp_drop(
               provider.context, second) == TURBOWASM_OK);

    assert(turbowasm_wasi02_cnet_destroy(
               &adapter) == TURBOWASM_OK);
}

int main(void) {
    test_cnet_control_plane();
    test_cnet_capacity_is_bounded();
    return 0;
}
