#include <turbowasm/wasi02_cnet.h>

int main() {
    turbowasm_wasi02_cnet adapter{};
    turbowasm_wasi02_cnet_config config{};
    turbowasm_wasi02_cnet_config_v2 extended{};
    turbowasm_wasi02_config_v2 facade_config{};
    turbowasm_wasi02_cnet_config_v2_init(&extended);
    turbowasm_wasi02_config_v2_init(&facade_config);
    if (extended.size != sizeof(extended) || extended.api_version != 2 || !extended.udp_capacity || !extended.lookup_capacity) return 6;
    if (extended.allow_udp_bind || extended.allow_udp_send || extended.allow_udp_receive || extended.allow_name_lookup) return 7;
    if (facade_config.size != sizeof(facade_config) || facade_config.api_version != 2) return 8;
    size_t events = 0;
    turbowasm_wasi02_cnet_config_init(&config);
    if (config.size != sizeof(config) || config.api_version != 1) return 1;
    if (config.allow_bind || config.allow_connect || config.allow_accept) return 2;
    if (!config.socket_capacity || !config.receive_bytes || !config.send_bytes || !config.payload_bytes) return 3;
    if (turbowasm_wasi02_cnet_advance(&adapter, &events) != TURBOWASM_INVALID_ARGUMENT) return 4;
    return turbowasm_wasi02_cnet_destroy(&adapter) == TURBOWASM_OK ? 0 : 5;
}
