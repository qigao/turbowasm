#ifndef TURBOWASM_WASI02_SOCKETS_H
#define TURBOWASM_WASI02_SOCKETS_H

#include "component_resource.h"
#include "wasi02_poll.h"
#include "wasi02_provider.h"
#include "wasi02_streams.h"

#include <turbowasm/wasi02_sockets.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_wasi02_tcp_state {
    TURBOWASM_WASI02_TCP_UNBOUND = 0,
    TURBOWASM_WASI02_TCP_BIND_IN_PROGRESS,
    TURBOWASM_WASI02_TCP_BOUND,
    TURBOWASM_WASI02_TCP_LISTEN_IN_PROGRESS,
    TURBOWASM_WASI02_TCP_LISTENING,
    TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS,
    TURBOWASM_WASI02_TCP_CONNECTED,
    TURBOWASM_WASI02_TCP_CLOSED
} turbowasm_wasi02_tcp_state;

typedef struct turbowasm_wasi02_tcp_slot {
    bool active;
    turbowasm_value provider_rep;
    turbowasm_wasi02_ip_address_family family;
    turbowasm_wasi02_tcp_state state;
} turbowasm_wasi02_tcp_slot;

typedef struct turbowasm_wasi02_sockets {
    turbowasm_wasi02_socket_provider provider;

    turbowasm_component_resource_table networks;
    turbowasm_component_resource_table tcp_resources;

    turbowasm_wasi02_tcp_slot *tcp_slots;
    uint32_t *tcp_free_indices;
    uint32_t tcp_capacity;
    uint32_t tcp_free_count;

    turbowasm_wasi02_streams *streams;
    turbowasm_wasi02_poll *poll;

    bool initialized;
} turbowasm_wasi02_sockets;

turbowasm_status turbowasm_wasi02_sockets_init(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_wasi02_socket_provider *provider,
    uint32_t max_networks,
    uint32_t max_tcp_sockets);

turbowasm_status turbowasm_wasi02_sockets_destroy(
    turbowasm_wasi02_sockets *sockets);

turbowasm_status turbowasm_wasi02_sockets_attach_io(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_poll *poll);

turbowasm_status turbowasm_wasi02_tcp_state_get(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource,
    turbowasm_wasi02_tcp_state *out_state);

/*
 * Provider-side terminal event for the sole asynchronous state transition in
 * the upstream TCP state machine: connected -> closed.
 */
turbowasm_status turbowasm_wasi02_tcp_mark_closed(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource);

turbowasm_status turbowasm_wasi02_network_drop(
    turbowasm_wasi02_sockets *sockets,
    uint32_t network_resource);

turbowasm_status turbowasm_wasi02_tcp_drop(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource);

/*
 * Direct typed S2 contract used before the Component import bridge is wired.
 * interface_name is one of: instance-network, tcp-create-socket, tcp.
 */
turbowasm_status turbowasm_wasi02_sockets_call(
    turbowasm_wasi02_sockets *sockets,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result);

#endif /* TURBOWASM_WASI02_SOCKETS_H */
