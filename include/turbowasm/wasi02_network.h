#ifndef TURBOWASM_WASI02_NETWORK_H
#define TURBOWASM_WASI02_NETWORK_H
#include <turbowasm/wasi02.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Additive versioned UDP/name-lookup contract; existing TCP provider/config
 * layouts are unchanged. All callbacks belong to one serialized host owner. */
typedef struct turbowasm_wasi02_ip_address {
    turbowasm_wasi02_ip_address_family family;
    union { uint8_t ipv4[4]; uint16_t ipv6[8]; } as;
} turbowasm_wasi02_ip_address;
typedef struct turbowasm_wasi02_incoming_datagram {
    uint8_t *data; size_t capacity, size;
    turbowasm_wasi02_ip_socket_address remote_address;
} turbowasm_wasi02_incoming_datagram;
typedef struct turbowasm_wasi02_outgoing_datagram {
    const uint8_t *data; size_t size;
    bool has_remote_address;
    turbowasm_wasi02_ip_socket_address remote_address;
} turbowasm_wasi02_outgoing_datagram;
typedef enum turbowasm_wasi02_udp_option {
    TURBOWASM_WASI02_UDP_HOP_LIMIT = 0,
    TURBOWASM_WASI02_UDP_RECEIVE_BUFFER,
    TURBOWASM_WASI02_UDP_SEND_BUFFER
} turbowasm_wasi02_udp_option;
typedef struct turbowasm_wasi02_network_provider {
    size_t size; uint32_t api_version;
    void *context;
    /* Hard publication bounds, nonzero with UDP methods. Receive copies into
     * preallocated caller buffers and consumes only on OK with error NONE.
     * No retained borrow survives callback return. Zero-sized messages count. */
    size_t max_datagram_bytes, max_datagram_batch;
    turbowasm_status (*udp_create)(void *, turbowasm_wasi02_ip_address_family, turbowasm_value *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_drop)(void *, turbowasm_value);
    turbowasm_status (*udp_start_bind)(void *, turbowasm_value, turbowasm_value, const turbowasm_wasi02_ip_socket_address *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_finish_bind)(void *, turbowasm_value, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_stream)(void *, turbowasm_value, const turbowasm_wasi02_ip_socket_address *, turbowasm_value *, turbowasm_value *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_local_address)(void *, turbowasm_value, turbowasm_wasi02_ip_socket_address *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_remote_address)(void *, turbowasm_value, turbowasm_wasi02_ip_socket_address *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_option_get)(void *, turbowasm_value, turbowasm_wasi02_udp_option, uint64_t *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_option_set)(void *, turbowasm_value, turbowasm_wasi02_udp_option, uint64_t, turbowasm_wasi02_socket_error *);
    turbowasm_status (*udp_subscribe)(void *, turbowasm_value, turbowasm_value *);
    turbowasm_status (*incoming_receive)(void *, turbowasm_value, turbowasm_wasi02_incoming_datagram *, size_t, size_t *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*incoming_subscribe)(void *, turbowasm_value, turbowasm_value *);
    turbowasm_status (*incoming_drop)(void *, turbowasm_value);
    turbowasm_status (*outgoing_check_send)(void *, turbowasm_value, uint64_t *, turbowasm_wasi02_socket_error *);
    /* Takes no payload ownership. Consumes one check-send grant; exact admitted
     * prefix count on success. A later error must surface on the next operation. */
    turbowasm_status (*outgoing_send)(void *, turbowasm_value, const turbowasm_wasi02_outgoing_datagram *, size_t, size_t *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*outgoing_subscribe)(void *, turbowasm_value, turbowasm_value *);
    turbowasm_status (*outgoing_drop)(void *, turbowasm_value);
    turbowasm_status (*resolve_addresses)(void *, turbowasm_value, turbowasm_wasi02_string_view, turbowasm_value *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*resolve_next_address)(void *, turbowasm_value, bool *, turbowasm_wasi02_ip_address *, turbowasm_wasi02_socket_error *);
    turbowasm_status (*resolve_subscribe)(void *, turbowasm_value, turbowasm_value *);
    turbowasm_status (*resolve_drop)(void *, turbowasm_value);
} turbowasm_wasi02_network_provider;
typedef struct turbowasm_wasi02_config_v2 {
    size_t size; uint32_t api_version;
    turbowasm_wasi02_config base;
    turbowasm_wasi02_network_provider network;
    uint32_t udp_socket_capacity, datagram_stream_capacity, resolve_stream_capacity;
} turbowasm_wasi02_config_v2;
void turbowasm_wasi02_config_v2_init(turbowasm_wasi02_config_v2 *config);
/* Copies the complete versioned config. Capacity zero omits its capability;
 * otherwise require every method for that capability. INVALID_ARGUMENT for
 * incomplete bundles/bounds. OOM leaves the facade uninitialized. Reps transfer
 * only on successful publication and are dropped exactly once. Destroy keeps
 * the existing suspended-instance retention contract. See README for examples. */
turbowasm_status turbowasm_wasi02_init_v2(turbowasm_wasi02 *wasi02,
    const turbowasm_wasi02_config_v2 *config, const turbowasm_runtime_config *runtime_config);
#ifdef __cplusplus
}
#endif
#endif
