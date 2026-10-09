#include <turbowasm/wasi_cnet.h>
#include <type_traits>

static_assert(std::is_standard_layout_v<turbowasm_wasi_preview1_config_v2>);
static_assert(std::is_standard_layout_v<turbowasm_wasi_descriptor_ops>);
int main() {
    if (turbowasm_host_call_check_interrupt(nullptr) != TURBOWASM_INVALID_ARGUMENT) return 6;
    turbowasm_wasi_preview1_config_v2 config{};
    turbowasm_wasi_preview1_config_v2_init(&config);
    if (config.size != sizeof(config) || config.api_version != 2 || config.allow_sockets || config.allow_poll) return 1;
    turbowasm_wasi02_cnet adapter{}; turbowasm_wasi_descriptor_ops ops{};
    if (turbowasm_wasi_cnet_descriptor_ops(&adapter,&ops) != TURBOWASM_INVALID_ARGUMENT) return 2;
    turbowasm_value socket{}, input{}, output{}; turbowasm_wasi_fs_file file{};
    if (turbowasm_wasi_cnet_listener_move(&adapter,&socket,&file) != TURBOWASM_INVALID_ARGUMENT) return 3;
    if (turbowasm_wasi_cnet_tcp_move(&adapter,&socket,&input,&output,&file) != TURBOWASM_INVALID_ARGUMENT) return 4;
    if (turbowasm_wasi_cnet_udp_move(&adapter,&socket,&input,&output,&file) != TURBOWASM_INVALID_ARGUMENT) return 5;
    return 0;
}
