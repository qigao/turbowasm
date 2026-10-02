#include <turbowasm/wasi02.h>

#include <cassert>
#include <type_traits>

static_assert(
    std::is_standard_layout<turbowasm_wasi02_socket_provider>::value,
    "WASI02 socket provider must remain standard-layout");
static_assert(
    std::is_standard_layout<turbowasm_wasi02_ip_socket_address>::value,
    "WASI02 socket address must remain standard-layout");

int main() {
    turbowasm_wasi02 wasi02{};
    turbowasm_wasi02_config config{};
    turbowasm_wasi02_socket_provider sockets{};

    config.sockets = sockets;
    config.socket_network_resource_capacity = 0u;
    config.tcp_socket_resource_capacity = 0u;

    assert(turbowasm_wasi02_init(
               &wasi02,
               &config,
               nullptr) == TURBOWASM_OK);
    assert(wasi02.impl != nullptr);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    assert(wasi02.impl == nullptr);
    return 0;
}
