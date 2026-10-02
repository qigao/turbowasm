#include <turbowasm/wasi02.h>

#include "wasi02_toolchain_fixtures.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void qualify_component(
    const uint8_t *bytes,
    size_t size) {
    turbowasm_component component = {0};

    assert(bytes != NULL);
    assert(size > 8u);
    assert(turbowasm_component_load_borrowed(
               &component, bytes, size) == TURBOWASM_OK);
    turbowasm_component_destroy(&component);
}

int main(void) {
    qualify_component(
        turbowasm_wasi02_fixture_monotonic_clock,
        turbowasm_wasi02_fixture_monotonic_clock_size);
    qualify_component(
        turbowasm_wasi02_fixture_random_u64,
        turbowasm_wasi02_fixture_random_u64_size);
    qualify_component(
        turbowasm_wasi02_fixture_cli_exit,
        turbowasm_wasi02_fixture_cli_exit_size);
    qualify_component(
        turbowasm_wasi02_fixture_filesystem_preopens,
        turbowasm_wasi02_fixture_filesystem_preopens_size);
    qualify_component(
        turbowasm_wasi02_fixture_stream_poll_block,
        turbowasm_wasi02_fixture_stream_poll_block_size);
    qualify_component(
        turbowasm_wasi02_fixture_socket_create_tcp,
        turbowasm_wasi02_fixture_socket_create_tcp_size);
    qualify_component(
        turbowasm_wasi02_fixture_socket_instance_network,
        turbowasm_wasi02_fixture_socket_instance_network_size);
    return 0;
}
