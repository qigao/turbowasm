#ifndef TURBOWASM_WASI_CNET_H
#define TURBOWASM_WASI_CNET_H
#include <turbowasm/wasi_sockets.h>
#include <turbowasm/wasi02_cnet.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional TurboWasm::WASICNet. Factory operations borrow adapter context;
 * its identities and leases keep native slots alive through actual completion.
 * Move only host-owned carriers before publishing any WASI02 facade. No stream
 * poll/error aliases or active waits may exist. Success zeros all input carriers
 * and transfers close ownership to *out_file; failure preserves every carrier.
 * Bind that file with fs_bind_socket_move; if binding fails, close it through
 * the factory's file.close. It must not be used through both WASI frontends.
 * TCP admission reserves another receive_bytes of bounded payload capacity for
 * PEEK|WAITALL. The maximum such peek is receive_bytes (MSGSIZE otherwise).
 */
turbowasm_status turbowasm_wasi_cnet_descriptor_ops(turbowasm_wasi02_cnet *,
    turbowasm_wasi_descriptor_ops *);
turbowasm_status turbowasm_wasi_cnet_listener_move(turbowasm_wasi02_cnet *,
    turbowasm_value *socket, turbowasm_wasi_fs_file *out_file);
turbowasm_status turbowasm_wasi_cnet_tcp_move(turbowasm_wasi02_cnet *,
    turbowasm_value *socket, turbowasm_value *input, turbowasm_value *output,
    turbowasm_wasi_fs_file *out_file);
turbowasm_status turbowasm_wasi_cnet_udp_move(turbowasm_wasi02_cnet *,
    turbowasm_value *socket, turbowasm_value *input, turbowasm_value *output,
    turbowasm_wasi_fs_file *out_file);

#ifdef __cplusplus
}
#endif
#endif
