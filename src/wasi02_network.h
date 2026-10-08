#ifndef TURBOWASM_WASI02_NETWORK_INTERNAL_H
#define TURBOWASM_WASI02_NETWORK_INTERNAL_H
#include <turbowasm/wasi02_network.h>
#include "component_resource.h"
#include "wasi02_provider.h"
struct turbowasm_wasi02_sockets;
enum { TW_NETWORK_UDP, TW_NETWORK_INCOMING, TW_NETWORK_OUTGOING, TW_NETWORK_RESOLVE, TW_NETWORK_KINDS };
typedef struct tw_network_slot {
    bool active, checked;
    turbowasm_value provider_rep;
    turbowasm_wasi02_ip_address_family family;
    unsigned state;
    uint32_t children[2];
    uint64_t permit;
} tw_network_slot;
typedef struct turbowasm_wasi02_network {
    turbowasm_wasi02_network_provider provider;
    turbowasm_component_resource_table tables[TW_NETWORK_KINDS];
    tw_network_slot *slots[TW_NETWORK_KINDS];
    uint32_t capacities[TW_NETWORK_KINDS];
    uint64_t identities[TW_NETWORK_KINDS];
    bool identity_bound[TW_NETWORK_KINDS];
} turbowasm_wasi02_network;
uint64_t tw_network_resource_id(unsigned kind);
int tw_network_resource_kind(const char *interface_name, const char *resource_name);
turbowasm_status tw_network_init(struct turbowasm_wasi02_sockets *, const turbowasm_wasi02_config_v2 *);
turbowasm_status tw_network_destroy(struct turbowasm_wasi02_sockets *);
turbowasm_status tw_network_rep(turbowasm_wasi02_network *, unsigned kind, uint32_t handle, turbowasm_value *);
turbowasm_status tw_network_drop(turbowasm_wasi02_network *, unsigned kind, uint32_t handle);
turbowasm_status tw_network_call(struct turbowasm_wasi02_sockets *, const char *, const char *,
    const turbowasm_wasi02_value *, size_t, turbowasm_wasi02_value *);
turbowasm_status turbowasm_wasi02_socket_address_from_value(const turbowasm_wasi02_value *, turbowasm_wasi02_ip_socket_address *);
turbowasm_status turbowasm_wasi02_socket_address_to_value(const turbowasm_wasi02_ip_socket_address *, turbowasm_wasi02_value *);
#endif
