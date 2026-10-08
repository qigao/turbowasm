#ifndef TURBOWASM_WASI02_CNET_H
#define TURBOWASM_WASI02_CNET_H

#include <turbowasm/wasi02_sockets.h>
#include <turbowasm/wasi02_io.h>
#include <turbowasm/wasi02_network.h>

#include <cnet/cnet.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbowasm_wasi02_cnet {
    void *impl;
} turbowasm_wasi02_cnet;

typedef struct turbowasm_wasi02_cnet_config {
    native_io_backend_kind backend;
    uint32_t socket_capacity;
    size_t default_listen_backlog;
    size_t size;
    uint32_t api_version;
    size_t receive_bytes, send_bytes, payload_bytes;
    uint32_t connect_timeout_ms, read_timeout_ms, write_timeout_ms;
    bool allow_bind, allow_connect, allow_accept;
    /* Optional additional restriction; borrowed immutable context. Called
     * before native admission, and for accepted peers before publication. */
    bool (*authorize)(void *context, unsigned operation,
        const turbowasm_wasi02_ip_socket_address *address);
    void *policy_context;
} turbowasm_wasi02_cnet_config;

enum { TURBOWASM_WASI02_CNET_BIND = 1, TURBOWASM_WASI02_CNET_CONNECT = 2, TURBOWASM_WASI02_CNET_ACCEPT = 3 };
void turbowasm_wasi02_cnet_config_init(turbowasm_wasi02_cnet_config *config);
turbowasm_status turbowasm_wasi02_cnet_init_external(turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_io *io, native_io_backend *backend,
    const turbowasm_wasi02_cnet_config *config, const turbowasm_runtime_config *runtime_config);

/* Retaining facade composition, including transport-terminal observation.
 * Copies config, installs this adapter's socket bundle and its I/O domain's
 * stream/poll bundles. Socket/stream/poll capacities must be nonzero. A socket
 * context belonging to another provider is rejected. Existing provider layouts
 * and synchronous constructors remain unchanged. Destroy the facade before
 * destroying the adapter; suspended instances/calls retain the facade. */
turbowasm_status turbowasm_wasi02_cnet_wasi02_init(turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02 *wasi02, const turbowasm_wasi02_config *config,
    const turbowasm_runtime_config *runtime_config);
/* Versioned additive UDP/DNS configuration. All capacities are hard bounds.
 * Defaults deny every network operation. Endpoint and name authorization are
 * additional restrictions, called before admission and for received/results.
 * Datagram payload storage is bounded by udp_capacity * datagram_bytes *
 * (receive_datagrams + send_datagrams + 1). Pair metadata stays alive while
 * closed poll aliases exist; pair_capacity bounds repeated stream replacement.
 * DNS servers_csv is copied at init; policy_context stays borrowed through
 * adapter destruction. Names reach authorize_name only after IDNA validation. */
typedef struct turbowasm_wasi02_cnet_config_v2 {
    size_t size; uint32_t api_version;
    turbowasm_wasi02_cnet_config base;
    uint32_t udp_capacity, pair_capacity, lookup_capacity;
    size_t datagram_bytes, receive_datagrams, send_datagrams;
    size_t lookup_results, max_name_bytes;
    uint32_t lookup_timeout_ms;
    bool allow_udp_bind, allow_udp_send, allow_udp_receive, allow_name_lookup;
    bool (*authorize_name)(void *, turbowasm_wasi02_string_view normalized_name);
    const char *servers_csv;
} turbowasm_wasi02_cnet_config_v2;
enum { TURBOWASM_WASI02_CNET_UDP_BIND = 4, TURBOWASM_WASI02_CNET_UDP_SEND = 5,
    TURBOWASM_WASI02_CNET_UDP_RECEIVE = 6, TURBOWASM_WASI02_CNET_RESOLVE_RESULT = 7 };
void turbowasm_wasi02_cnet_config_v2_init(turbowasm_wasi02_cnet_config_v2 *);
turbowasm_status turbowasm_wasi02_cnet_init_external_v2(turbowasm_wasi02_cnet *,
    turbowasm_wasi02_io *, native_io_backend *, const turbowasm_wasi02_cnet_config_v2 *,
    const turbowasm_runtime_config *);
turbowasm_status turbowasm_wasi02_cnet_network_provider(turbowasm_wasi02_cnet *,
    turbowasm_wasi02_network_provider *);
turbowasm_status turbowasm_wasi02_cnet_wasi02_init_v2(turbowasm_wasi02_cnet *,
    turbowasm_wasi02 *, const turbowasm_wasi02_config_v2 *, const turbowasm_runtime_config *);

turbowasm_status turbowasm_wasi02_cnet_advance(turbowasm_wasi02_cnet *adapter, size_t *out_events);
turbowasm_status turbowasm_wasi02_cnet_route_completion(turbowasm_wasi02_cnet *adapter,
    const native_io_completion *completion, bool *out_consumed);
turbowasm_status turbowasm_wasi02_cnet_next_timeout(turbowasm_wasi02_cnet *adapter,
    uint32_t max_wait_ms, uint32_t *out_timeout_ms);
turbowasm_status turbowasm_wasi02_cnet_shutdown_request(turbowasm_wasi02_cnet *adapter);
turbowasm_status turbowasm_wasi02_cnet_shutdown_poll(turbowasm_wasi02_cnet *adapter, bool *out_complete);

/* Bounded, single-owner IP adapter. The caller owns backend observation:
 * advance, observe once, route every completion, then advance the shared I/O
 * domain. No worker thread is created. Keep backend and policy_context alive
 * until shutdown_poll reports complete and destroy succeeds. The adapter
 * retains io; socket/stream/poll carriers keep their connection alive.
 * Config defaults deny network admission; explicitly authorize operations.
 * Requires the optional TurboWasm::WASI02CNet target and a Salts SDK exposing
 * directional EOF, CNET_DATAGRAM_SOCKET_CONTROL_VERSION and
 * CNET_NAME_LOOKUP_API_VERSION. Version 1 constructors retain TCP behavior;
 * version 2 opts into UDP and DNS without changing existing layouts. */

turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter);

turbowasm_status turbowasm_wasi02_cnet_socket_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_socket_provider *out_provider);

#ifdef __cplusplus
}
#endif
#endif /* TURBOWASM_WASI02_CNET_H */
