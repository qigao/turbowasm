#include <turbowasm/wasi02.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_wasi02_config config = {0};
    turbowasm_wasi02_socket_provider sockets = {0};
    turbowasm_wasi02_ip_socket_address address = {0};

    config.sockets = sockets;
    config.socket_network_resource_capacity = 0u;
    config.tcp_socket_resource_capacity = 0u;

    address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV6;
    address.as.ipv6.port = UINT16_C(443);
    address.as.ipv6.flow_info = UINT32_C(7);
    address.as.ipv6.scope_id = UINT32_C(3);
    assert(address.as.ipv6.port == UINT16_C(443));
    assert(TURBOWASM_WASI02_SOCKET_ERROR_UNKNOWN == 1);
    assert(TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH == 2);

    assert(turbowasm_wasi02_init(
               &wasi02,
               &config,
               NULL) == TURBOWASM_OK);
    assert(wasi02.impl != NULL);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    assert(wasi02.impl == NULL);
    return 0;
}
