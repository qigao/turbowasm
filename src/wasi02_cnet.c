#include "wasi02_cnet.h"
#include "runtime_alloc.h"

#include <salts/error_codes.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TW_CNET_DEFAULT_BACKLOG = 128u
};

typedef enum tw_cnet_slot_state {
    TW_CNET_SLOT_FREE = 0,
    TW_CNET_SLOT_UNBOUND,
    TW_CNET_SLOT_BOUND,
    TW_CNET_SLOT_LISTENING,
    TW_CNET_SLOT_CONNECTING,
    TW_CNET_SLOT_CONNECTED,
    TW_CNET_SLOT_CLOSED
} tw_cnet_slot_state;

typedef struct tw_cnet_impl tw_cnet_impl;
typedef struct tw_cnet_readiness { struct tw_cnet_slot *slot; unsigned kind; } tw_cnet_readiness;

typedef struct tw_cnet_slot {
    bool active;
    uint32_t generation;
    uint64_t token;
    turbowasm_wasi02_ip_address_family family;
    tw_cnet_slot_state state;
    size_t listen_backlog;
    cnet_listener listener;
    tw_cnet_impl *owner;
    uint32_t refs;
    bool socket_live, connection_live, connected, terminal, closing;
    bool listener_attached, close_requested;
    bool eof_enabled;
    bool rx_pending, rx_closed, tx_busy, tx_closed, flushing;
    bool input_live, output_live, accept_pending, accept_ready;
    unsigned shutdown_pending;
    int terminal_status, accept_status;
    native_io_request accept_request;
    cnet_connection connection;
    uint8_t *rx;
    size_t rx_offset, rx_size, permit, tx_size, payload_charge;
    mem_buffer_t *tx;
    turbowasm_wasi02_io_source sources[3];
    tw_cnet_readiness readiness[3];
    turbowasm_value input_rep, output_rep;
} tw_cnet_slot;

struct tw_cnet_impl {
    native_io_backend_kind backend;
    tw_cnet_slot *slots;
    uint32_t *free_indices;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t active_count;
    uint32_t network_live;
    uint32_t facades;
    uint32_t network_generation;
    uint64_t network_token;
    size_t default_listen_backlog;
    bool external, busy, stopping, stopped;
    turbowasm_wasi02_cnet_config config;
    turbowasm_runtime_config runtime;
    turbowasm_wasi02_io *io;
    native_io_backend *native_backend;
    cnet_client client;
    size_t payload_used;
    int cleanup_error;
    struct tw_cnet_network *network;
};

extern bool turbowasm_wasi02_io_retain_private(turbowasm_wasi02_io *);
extern void turbowasm_wasi02_io_release_private(void *);
extern void *turbowasm_wasi02_io_context_private(turbowasm_wasi02_io *);
extern uint64_t turbowasm_wasi02_io_token_private(void);

static bool permitted(tw_cnet_impl *p, unsigned operation, const turbowasm_wasi02_ip_socket_address *address) {
    bool allowed;
    if (!p->external) return true;
    allowed = operation == TURBOWASM_WASI02_CNET_BIND ? p->config.allow_bind :
        operation == TURBOWASM_WASI02_CNET_CONNECT ? p->config.allow_connect : p->config.allow_accept;
    if (allowed && p->config.authorize) {
        bool busy = p->busy; p->busy = true;
        allowed = p->config.authorize(p->config.policy_context, operation, address);
        p->busy = busy;
    }
    return allowed;
}
#if defined(TURBOWASM_WASI02_NATIVE_TCP)
static void native_drop_pending(tw_cnet_slot *slot);
static turbowasm_status native_source_register(tw_cnet_slot *slot, unsigned kind);
#endif

static tw_cnet_impl *impl_mut(turbowasm_wasi02_cnet *adapter) {
    return adapter == NULL ? NULL : (tw_cnet_impl *)adapter->impl;
}

static turbowasm_wasi02_socket_error map_error(int status) {
    switch (status) {
        case SALTS_OK:
            return TURBOWASM_WASI02_SOCKET_ERROR_NONE;
        case SALTS_EPERM:
            return TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED;
        case SALTS_ENOTSUP:
        case SALTS_ENOSYS:
        case SALTS_EAFNOSUPPORT:
        case SALTS_EPROTONOSUPPORT:
        case SALTS_ENOPROTOOPT:
            return TURBOWASM_WASI02_SOCKET_ERROR_NOT_SUPPORTED;
        case SALTS_EINVAL:
        case SALTS_EFAULT:
        case SALTS_ERANGE:
            return TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
        case SALTS_ENOMEM:
        case SALTS_ENOBUFS:
            return TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY;
        case SALTS_ETIMEDOUT:
            return TURBOWASM_WASI02_SOCKET_ERROR_TIMEOUT;
        case SALTS_EALREADY:
        case SALTS_EBUSY:
            return TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT;
        case SALTS_EBADF:
        case SALTS_ENOENT:
        case SALTS_ENOTCONN:
        case SALTS_EISCONN:
        case SALTS_ESHUTDOWN:
            return TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        case SALTS_EMFILE:
        case SALTS_ENFILE:
            return TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT;
        case SALTS_EADDRNOTAVAIL:
            return TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_NOT_BINDABLE;
        case SALTS_EADDRINUSE:
            return TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_IN_USE;
        case SALTS_ENETDOWN:
        case SALTS_ENETUNREACH:
        case SALTS_EHOSTUNREACH:
        case SALTS_ENONET:
            return TURBOWASM_WASI02_SOCKET_ERROR_REMOTE_UNREACHABLE;
        case SALTS_ECONNREFUSED:
            return TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_REFUSED;
        case SALTS_ECONNRESET:
            return TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_RESET;
        case SALTS_ECONNABORTED:
            return TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_ABORTED;
        case SALTS_EMSGSIZE:
            return TURBOWASM_WASI02_SOCKET_ERROR_DATAGRAM_TOO_LARGE;
        case SALTS_EAI_NONAME:
        case SALTS_EAI_NODATA:
            return TURBOWASM_WASI02_SOCKET_ERROR_NAME_UNRESOLVABLE;
        case SALTS_EAI_AGAIN:
            return TURBOWASM_WASI02_SOCKET_ERROR_TEMPORARY_RESOLVER_FAILURE;
        case SALTS_EAI_FAIL:
            return TURBOWASM_WASI02_SOCKET_ERROR_PERMANENT_RESOLVER_FAILURE;
        default:
            return TURBOWASM_WASI02_SOCKET_ERROR_UNKNOWN;
    }
}

static uint64_t pack_rep(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32u) |
           ((uint64_t)index + UINT64_C(1));
}

static bool unpack_rep(
    turbowasm_value rep,
    uint32_t *out_index,
    uint32_t *out_generation) {
    uint64_t packed;
    uint32_t encoded;

    if (out_index == NULL || out_generation == NULL ||
        rep.kind != TURBOWASM_VALUE_I64)
        return false;

    packed = (uint64_t)rep.as.i64;
    encoded = (uint32_t)packed;
    *out_generation = (uint32_t)(packed >> 32u);
    if (encoded == 0u || *out_generation == 0u)
        return false;
    *out_index = encoded - 1u;
    return true;
}

static tw_cnet_slot *slot_lookup(
    tw_cnet_impl *impl,
    turbowasm_value rep) {
    uint32_t index;
    uint32_t generation;
    tw_cnet_slot *slot;

    if (impl && impl->external) {
        if (rep.kind != TURBOWASM_VALUE_I64 || !rep.as.i64) return NULL;
        for (uint32_t i = 0; i < impl->capacity; ++i)
            if (impl->slots[i].active && impl->slots[i].token == (uint64_t)rep.as.i64) return &impl->slots[i];
        return NULL;
    }
    if (impl == NULL ||
        !unpack_rep(rep, &index, &generation) ||
        index >= impl->capacity)
        return NULL;

    slot = &impl->slots[index];
    if (!slot->active || slot->generation != generation)
        return NULL;
    return slot;
}
static tw_cnet_slot *slot_from_rep(tw_cnet_impl *impl, turbowasm_value rep) {
    return impl && impl->external && impl->busy ? NULL : slot_lookup(impl, rep);
}

static tw_cnet_slot *reserve_slot(
    tw_cnet_impl *impl,
    turbowasm_wasi02_ip_address_family family,
    uint32_t *out_index) {
    uint32_t index;
    uint32_t generation;
    tw_cnet_slot *slot;
    uint64_t token = impl && impl->external ? turbowasm_wasi02_io_token_private() : 0;

    if (impl == NULL || out_index == NULL ||
        impl->free_count == 0u || (impl->external && !token))
        return NULL;

    index = impl->free_indices[--impl->free_count];
    slot = &impl->slots[index];
    generation = slot->generation + 1u;

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = generation;
    slot->token = token;
    slot->family = family;
    slot->state = TW_CNET_SLOT_UNBOUND;
    slot->listen_backlog = impl->default_listen_backlog;
    slot->owner = impl; slot->refs = 1u; slot->socket_live = true;
    ++impl->active_count;
    *out_index = index;
    return slot;
}

static void release_slot(
    tw_cnet_impl *impl,
    tw_cnet_slot *slot) {
    uint32_t index;
    uint32_t generation;

    if (impl == NULL || slot == NULL || !slot->active)
        return;

    index = (uint32_t)(slot - impl->slots);
    generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    --impl->active_count;
    if (generation != UINT32_MAX)
        impl->free_indices[impl->free_count++] = index;
}

static bool network_rep_valid(
    const tw_cnet_impl *impl,
    turbowasm_value rep) {
    return impl != NULL &&
           rep.kind == TURBOWASM_VALUE_I64 &&
           impl->network_generation != 0u &&
           (uint64_t)rep.as.i64 ==
               (impl->external ? impl->network_token : (uint64_t)impl->network_generation);
}

static cnet_datagram_address_family cnet_family(
    turbowasm_wasi02_ip_address_family family) {
    return family == TURBOWASM_WASI02_IP_ADDRESS_IPV6
        ? CNET_DATAGRAM_ADDRESS_IPV6
        : CNET_DATAGRAM_ADDRESS_IPV4;
}

static bool endpoint_from_wasi(
    const turbowasm_wasi02_ip_socket_address *address,
    cnet_stream_endpoint *out) {
    size_t i;

    if (address == NULL || out == NULL)
        return false;

    *out = (cnet_stream_endpoint)CNET_STREAM_ENDPOINT_INIT;
    if (address->family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) {
        out->family = CNET_DATAGRAM_ADDRESS_IPV4;
        out->port = address->as.ipv4.port;
        memcpy(out->address, address->as.ipv4.address, 4u);
        return true;
    }

    if (address->family != TURBOWASM_WASI02_IP_ADDRESS_IPV6)
        return false;

    out->family = CNET_DATAGRAM_ADDRESS_IPV6;
    out->port = address->as.ipv6.port;
    out->flow_info = address->as.ipv6.flow_info;
    out->scope_id = address->as.ipv6.scope_id;
    for (i = 0u; i < 8u; ++i) {
        uint16_t word = address->as.ipv6.address[i];
        out->address[i * 2u] = (uint8_t)(word >> 8u);
        out->address[i * 2u + 1u] = (uint8_t)word;
    }
    return true;
}

static bool endpoint_to_wasi(
    const cnet_stream_endpoint *endpoint,
    turbowasm_wasi02_ip_socket_address *out) {
    size_t i;

    if (endpoint == NULL || out == NULL ||
        endpoint->size < sizeof(*endpoint) ||
        endpoint->version != CNET_STREAM_ENDPOINT_API_VERSION)
        return false;

    memset(out, 0, sizeof(*out));
    if (endpoint->family == CNET_DATAGRAM_ADDRESS_IPV4) {
        out->family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
        out->as.ipv4.port = endpoint->port;
        memcpy(out->as.ipv4.address, endpoint->address, 4u);
        return true;
    }
    if (endpoint->family != CNET_DATAGRAM_ADDRESS_IPV6)
        return false;

    out->family = TURBOWASM_WASI02_IP_ADDRESS_IPV6;
    out->as.ipv6.port = endpoint->port;
    out->as.ipv6.flow_info = endpoint->flow_info;
    out->as.ipv6.scope_id = endpoint->scope_id;
    for (i = 0u; i < 8u; ++i) {
        out->as.ipv6.address[i] =
            (uint16_t)(((uint16_t)endpoint->address[i * 2u] << 8u) |
                       endpoint->address[i * 2u + 1u]);
    }
    return true;
}

static turbowasm_status provider_instance_network(
    void *context,
    turbowasm_value *out_rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;

    if (impl == NULL || out_rep == NULL ||
        impl->network_live == UINT32_MAX || impl->busy || impl->stopping)
        return TURBOWASM_INVALID_ARGUMENT;

    ++impl->network_live;
    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)(impl->external ? impl->network_token : impl->network_generation);
    return TURBOWASM_OK;
}

static turbowasm_status provider_network_drop(
    void *context,
    turbowasm_value rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;

    if (!network_rep_valid(impl, rep) ||
        impl->network_live == 0u || impl->busy)
        return TURBOWASM_TRAPPED;
    --impl->network_live;
    return TURBOWASM_OK;
}

static turbowasm_status provider_tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot;
    uint32_t index;
    int status;

    if (impl == NULL || impl->busy || impl->stopping || out_rep == NULL || out_error == NULL ||
        (family != TURBOWASM_WASI02_IP_ADDRESS_IPV4 &&
         family != TURBOWASM_WASI02_IP_ADDRESS_IPV6))
        return TURBOWASM_INVALID_ARGUMENT;

    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    slot = reserve_slot(impl, family, &index);
    if (slot == NULL) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT;
        return TURBOWASM_OK;
    }

    impl->busy = true;
    status = cnet_listener_open(
        &slot->listener, impl->backend, cnet_family(family));
    impl->busy = false;
    if (status != SALTS_OK) {
        *out_error = map_error(status);
        release_slot(impl, slot);
        return TURBOWASM_OK;
    }
#if defined(TURBOWASM_WASI02_NATIVE_TCP)
    if (impl->external && native_source_register(slot, 0) != TURBOWASM_OK) {
        (void)cnet_listener_close(&slot->listener); (void)cnet_listener_destroy(&slot->listener);
        release_slot(impl, slot); *out_error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK;
    }
#endif

    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)(impl->external ? slot->token : pack_rep(index, slot->generation));
    return TURBOWASM_OK;
}

static turbowasm_status provider_tcp_drop(
    void *context,
    turbowasm_value rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, rep);
    int status;

    if (slot == NULL)
        return TURBOWASM_TRAPPED;

    if (impl->external) {
        if (!slot->socket_live || impl->busy) return TURBOWASM_INVALID_ARGUMENT;
        slot->socket_live = false; --slot->refs;
        return TURBOWASM_OK;
    }

    status = cnet_listener_close(&slot->listener);
    if (status != SALTS_OK && status != SALTS_EALREADY)
        return TURBOWASM_TRAPPED;

    status = cnet_listener_destroy(&slot->listener);
    if (slot->listener.impl != NULL)
        return status == SALTS_OK
            ? TURBOWASM_TRAPPED
            : TURBOWASM_INVALID_ARGUMENT;

    /*
     * destroy consumed the CNet owner before module-shutdown reporting.
     * Never retain a provider slot whose CNet identity no longer exists.
     */
    release_slot(impl, slot);
    return TURBOWASM_OK;
}

static turbowasm_status provider_start_bind(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *local_address,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    cnet_stream_endpoint endpoint = CNET_STREAM_ENDPOINT_INIT;
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }
    if (!network_rep_valid(impl, network_rep) ||
        !endpoint_from_wasi(local_address, &endpoint) ||
        local_address->family != slot->family) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
        return TURBOWASM_OK;
    }
    if (!permitted(impl, TURBOWASM_WASI02_CNET_BIND, local_address)) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK;
    }
    if (slot->state != TW_CNET_SLOT_UNBOUND) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    status = cnet_listener_bind_open_endpoint(
        &slot->listener, &endpoint);
    *out_error = map_error(status);
    if (status == SALTS_OK)
        slot->state = TW_CNET_SLOT_BOUND;
    return TURBOWASM_OK;
}

static turbowasm_status provider_finish_bind(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = slot != NULL && slot->state == TW_CNET_SLOT_BOUND
        ? TURBOWASM_WASI02_SOCKET_ERROR_NONE
        : TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
    return TURBOWASM_OK;
}

static turbowasm_status provider_start_listen(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL || slot->state != TW_CNET_SLOT_BOUND) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    status = cnet_listener_listen(
        &slot->listener, slot->listen_backlog);
    *out_error = map_error(status);
    if (status == SALTS_OK)
        slot->state = TW_CNET_SLOT_LISTENING;
    return TURBOWASM_OK;
}

static turbowasm_status provider_finish_listen(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error =
        slot != NULL && slot->state == TW_CNET_SLOT_LISTENING
            ? TURBOWASM_WASI02_SOCKET_ERROR_NONE
            : TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
    return TURBOWASM_OK;
}

static turbowasm_status provider_local_address(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_ip_socket_address *out_address,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    cnet_stream_endpoint endpoint = CNET_STREAM_ENDPOINT_INIT;
    int status;

    if (out_address == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL || slot->state == TW_CNET_SLOT_UNBOUND) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    status = slot->connection_live && !slot->terminal
        ? cnet_connection_local_endpoint(&impl->client, slot->connection, &endpoint)
        : cnet_listener_local_endpoint(&slot->listener, &endpoint);
    *out_error = map_error(status);
    if (status == SALTS_OK &&
        !endpoint_to_wasi(&endpoint, out_address))
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_backlog(
    void *context,
    turbowasm_value socket_rep,
    uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL || value == 0u || value > SIZE_MAX) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
        return TURBOWASM_OK;
    }

    status = cnet_listener_set_backlog(
        &slot->listener, (size_t)value);
    *out_error = map_error(status);
    if (status == SALTS_OK)
        slot->listen_backlog = (size_t)value;
    return TURBOWASM_OK;
}

static int option_get(
    tw_cnet_impl *impl,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t *out_value) {
    tw_cnet_slot *slot = slot_from_rep(impl, rep);

    if (slot == NULL || out_value == NULL)
        return SALTS_EINVAL;
    if (slot->terminal || slot->state == TW_CNET_SLOT_CLOSED) return SALTS_ESHUTDOWN;
    if (slot->connection_live && !slot->terminal)
        return cnet_connection_tcp_option_get(&impl->client, slot->connection, option, out_value);
    return cnet_listener_tcp_option_get(
        &slot->listener, option, out_value);
}

static int option_set(
    tw_cnet_impl *impl,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t value) {
    tw_cnet_slot *slot = slot_from_rep(impl, rep);

    if (slot == NULL)
        return SALTS_EINVAL;
    if (slot->terminal || slot->state == TW_CNET_SLOT_CLOSED) return SALTS_ESHUTDOWN;
    if (slot->connection_live && !slot->terminal)
        return cnet_connection_tcp_option_set(&impl->client, slot->connection, option, value);
    return cnet_listener_tcp_option_set(
        &slot->listener, option, value);
}

static uint64_t ms_to_ns(uint64_t value) {
    return value > UINT64_MAX / UINT64_C(1000000)
        ? UINT64_MAX
        : value * UINT64_C(1000000);
}

static uint64_t ns_to_ms_ceil(uint64_t value) {
    uint64_t ms = value / UINT64_C(1000000);
    if (value % UINT64_C(1000000) != 0u)
        ++ms;
    return ms;
}

static turbowasm_status provider_get_bool(
    void *context,
    turbowasm_value rep,
    bool *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    uint64_t value = 0u;
    int status;

    if (out_value == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_get(
        impl, rep, CNET_TCP_SOCKET_KEEPALIVE_ENABLED, &value);
    *out_error = map_error(status);
    if (status == SALTS_OK)
        *out_value = value != 0u;
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_bool(
    void *context,
    turbowasm_value rep,
    bool value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_set(
        (tw_cnet_impl *)context, rep,
        CNET_TCP_SOCKET_KEEPALIVE_ENABLED,
        value ? UINT64_C(1) : UINT64_C(0));
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_get_keepalive_time(
    void *context,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    uint64_t value = 0u;
    int status;

    if (out_value == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_get((tw_cnet_impl *)context, rep, option, &value);
    *out_error = map_error(status);
    if (status == SALTS_OK)
        *out_value = ms_to_ns(value);
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_keepalive_time(
    void *context,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_set(
        (tw_cnet_impl *)context, rep, option,
        ns_to_ms_ceil(value));
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_get_idle(
    void *context, turbowasm_value rep, uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_get_keepalive_time(
        context, rep, CNET_TCP_SOCKET_KEEPALIVE_IDLE_MS,
        out_value, out_error);
}

static turbowasm_status provider_set_idle(
    void *context, turbowasm_value rep, uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_set_keepalive_time(
        context, rep, CNET_TCP_SOCKET_KEEPALIVE_IDLE_MS,
        value, out_error);
}

static turbowasm_status provider_get_interval(
    void *context, turbowasm_value rep, uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_get_keepalive_time(
        context, rep, CNET_TCP_SOCKET_KEEPALIVE_INTERVAL_MS,
        out_value, out_error);
}

static turbowasm_status provider_set_interval(
    void *context, turbowasm_value rep, uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_set_keepalive_time(
        context, rep, CNET_TCP_SOCKET_KEEPALIVE_INTERVAL_MS,
        value, out_error);
}

static turbowasm_status provider_get_count(
    void *context,
    turbowasm_value rep,
    uint32_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    uint64_t value = 0u;
    int status;

    if (out_value == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_get(
        (tw_cnet_impl *)context, rep,
        CNET_TCP_SOCKET_KEEPALIVE_COUNT, &value);
    *out_error = map_error(status);
    if (status == SALTS_OK) {
        if (value > UINT32_MAX)
            return TURBOWASM_TRAPPED;
        *out_value = (uint32_t)value;
    }
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_count(
    void *context,
    turbowasm_value rep,
    uint32_t value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_set(
        (tw_cnet_impl *)context, rep,
        CNET_TCP_SOCKET_KEEPALIVE_COUNT, value);
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_get_hop(
    void *context,
    turbowasm_value rep,
    uint8_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    uint64_t value = 0u;
    int status;

    if (out_value == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_get(
        (tw_cnet_impl *)context, rep,
        CNET_TCP_SOCKET_HOP_LIMIT, &value);
    *out_error = map_error(status);
    if (status == SALTS_OK) {
        if (value > UINT8_MAX)
            return TURBOWASM_TRAPPED;
        *out_value = (uint8_t)value;
    }
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_hop(
    void *context,
    turbowasm_value rep,
    uint8_t value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_set(
        (tw_cnet_impl *)context, rep,
        CNET_TCP_SOCKET_HOP_LIMIT, value);
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_get_u64_option(
    void *context,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_value == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_get(
        (tw_cnet_impl *)context, rep, option, out_value);
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_set_u64_option(
    void *context,
    turbowasm_value rep,
    cnet_tcp_socket_option option,
    uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = option_set(
        (tw_cnet_impl *)context, rep, option, value);
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static turbowasm_status provider_get_recv(
    void *context, turbowasm_value rep, uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_get_u64_option(
        context, rep, CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES,
        out_value, out_error);
}

static turbowasm_status provider_set_recv(
    void *context, turbowasm_value rep, uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_set_u64_option(
        context, rep, CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES,
        value, out_error);
}

static turbowasm_status provider_get_send(
    void *context, turbowasm_value rep, uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_get_u64_option(
        context, rep, CNET_TCP_SOCKET_SEND_BUFFER_BYTES,
        out_value, out_error);
}

static turbowasm_status provider_set_send(
    void *context, turbowasm_value rep, uint64_t value,
    turbowasm_wasi02_socket_error *out_error) {
    return provider_set_u64_option(
        context, rep, CNET_TCP_SOCKET_SEND_BUFFER_BYTES,
        value, out_error);
}

#if defined(TURBOWASM_WASI02_NATIVE_TCP)
/* Native data owners are never released from a CNet callback. Callbacks record
 * bounded outcomes; external advance performs retirement after CNet returns. */
static turbowasm_status native_ready(void *context, bool *out) {
    tw_cnet_readiness *r = context; tw_cnet_slot *s = r->slot;
    if (r->kind == 0) *out = s->state == TW_CNET_SLOT_LISTENING ? s->accept_ready :
        s->state == TW_CNET_SLOT_CONNECTING ? s->connected || s->terminal : true;
    else if (r->kind == 1) *out = s->rx_size != 0 || s->rx_closed || s->terminal;
    else *out = s->tx_closed || s->terminal || (!s->tx_busy && !s->flushing);
    return TURBOWASM_OK;
}
static void native_retain(void *context) { ++((tw_cnet_readiness *)context)->slot->refs; }
static void native_release(void *context) { --((tw_cnet_readiness *)context)->slot->refs; }
static turbowasm_status native_source_register(tw_cnet_slot *s, unsigned kind) {
    turbowasm_wasi02_io_source_ops ops;
    s->readiness[kind] = (tw_cnet_readiness){s, kind};
    ops = (turbowasm_wasi02_io_source_ops){&s->readiness[kind], native_ready, native_retain, native_release};
    return turbowasm_wasi02_io_source_register(s->owner->io, &ops, &s->sources[kind]);
}
static void native_changed(tw_cnet_slot *s) {
    for (unsigned i = 0; i < 3; ++i) if (s->sources[i].token)
        (void)turbowasm_wasi02_io_source_changed(s->owner->io, s->sources[i]);
}
static void native_state(void *context, cnet_connection connection, cnet_connection_state state, const cnet_error *error) {
    tw_cnet_slot *s = context; (void)connection;
    if (state == CNET_CONNECTION_CONNECTED) s->connected = true;
    if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
        s->terminal = true; s->rx_pending = false; s->tx_busy = false; s->flushing = false;
        if (s->terminal_status == SALTS_OK) s->terminal_status = error != NULL ? error->status : SALTS_OK;
        if (state == CNET_CONNECTION_FAILED && s->terminal_status == SALTS_OK) s->terminal_status = SALTS_EIO;
    }
    native_changed(s);
}
static void native_receive(void *context, cnet_connection connection, const cnet_receive_view *view) {
    tw_cnet_slot *s = context; (void)connection; s->rx_pending = false;
    if (!view || view->kind != CNET_MESSAGE_BYTES || view->size > s->owner->config.receive_bytes || s->rx_size != 0 ||
        (view->size != 0 && view->data == NULL)) { s->terminal_status = SALTS_EPROTO; s->closing = true; }
    else if (!s->rx_closed && view->size != 0) { memcpy(s->rx, view->data, view->size); s->rx_offset = 0; s->rx_size = view->size; }
    else if (view->size == 0) s->rx_closed = true;
    native_changed(s);
}
static void native_sent(void *context, cnet_connection connection, size_t size) {
    tw_cnet_slot *s = context; (void)connection;
    if (!s->tx_busy || size != s->tx_size) { s->terminal_status = SALTS_EPROTO; s->closing = true; }
    s->tx_busy = false; s->flushing = false; s->tx_size = 0; native_changed(s);
}
static cnet_observer native_observer(tw_cnet_slot *s) {
    return (cnet_observer){native_state, native_receive, s, native_sent};
}
static turbowasm_status native_stream_error(tw_cnet_slot *s, bool input, turbowasm_wasi02_stream_error *error) {
    memset(error, 0, sizeof(*error));
    if ((input ? s->rx_closed : s->tx_closed) || (s->terminal && s->terminal_status == SALTS_OK)) {
        error->kind = TURBOWASM_WASI02_STREAM_ERROR_CLOSED;
    } else if (s->terminal_status != SALTS_OK) {
        error->kind = TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED;
        error->error_rep.kind = TURBOWASM_VALUE_I64; error->error_rep.as.i64 = (int64_t)map_error(s->terminal_status);
    }
    return TURBOWASM_OK;
}
static turbowasm_status native_input_read(void *context, turbowasm_value rep, uint64_t max,
    const uint8_t **data, size_t *size, turbowasm_wasi02_stream_error *error) {
    tw_cnet_slot *s = slot_from_rep(context, rep); size_t n;
    if (!s || !s->input_live || !data || !size || !error) return TURBOWASM_TRAPPED;
    *data = NULL; *size = 0; memset(error, 0, sizeof(*error));
    if (!max) return TURBOWASM_OK;
    if (!s->rx_size) return native_stream_error(s, true, error);
    n = max < s->rx_size ? (size_t)max : s->rx_size;
    *data = s->rx + s->rx_offset; *size = n; s->rx_offset += n; s->rx_size -= n;
    /* No receive is resubmitted here: the returned view remains valid through
     * the facade's allocation-free result copy until external advance. */
    native_changed(s); return TURBOWASM_OK;
}
static turbowasm_status native_input_skip(void *context, turbowasm_value rep, uint64_t max,
    uint64_t *skipped, turbowasm_wasi02_stream_error *error) {
    const uint8_t *data; size_t size = 0; turbowasm_status status;
    if (!skipped) return TURBOWASM_INVALID_ARGUMENT;
    status = native_input_read(context, rep, max, &data, &size, error); *skipped = size; return status;
}
static turbowasm_status native_output_check(void *context, turbowasm_value rep, uint64_t *permit,
    turbowasm_wasi02_stream_error *error) {
    tw_cnet_slot *s = slot_from_rep(context, rep);
    if (!s || !s->output_live || !permit || !error) return TURBOWASM_TRAPPED;
    native_stream_error(s, false, error); *permit = 0;
    if (error->kind != TURBOWASM_WASI02_STREAM_ERROR_NONE) return TURBOWASM_OK;
    s->permit = s->tx_busy || s->flushing ? 0 : s->owner->config.send_bytes; *permit = s->permit;
    return TURBOWASM_OK;
}
static turbowasm_status native_write(tw_cnet_slot *s, const uint8_t *data, size_t size,
    bool zeroes, turbowasm_wasi02_stream_error *error) {
    int status;
    native_stream_error(s, false, error);
    if (error->kind != TURBOWASM_WASI02_STREAM_ERROR_NONE) return TURBOWASM_OK;
    if (size > s->permit || s->tx_busy || s->flushing || (!zeroes && size && !data)) return TURBOWASM_TRAPPED;
    if (zeroes) memset(mem_buffer_data(s->tx), 0, size); else if (size) memcpy(mem_buffer_data(s->tx), data, size);
    s->permit = 0;
    if (!size) return TURBOWASM_OK;
    mem_set_used(s->tx, size); s->tx_size = size; s->tx_busy = true;
    status = cnet_send_buffer(&s->owner->client, s->connection, s->tx);
    if (status != SALTS_OK) { s->tx_busy = false; s->tx_size = 0; s->terminal_status = status; return native_stream_error(s, false, error); }
    native_changed(s); return TURBOWASM_OK;
}
static turbowasm_status native_output_write(void *context, turbowasm_value rep, const uint8_t *data,
    size_t size, turbowasm_wasi02_stream_error *error) {
    tw_cnet_slot *s = slot_from_rep(context, rep);
    if (!s || !s->output_live || !error) return TURBOWASM_TRAPPED;
    return native_write(s, data, size, false, error);
}
static turbowasm_status native_output_zeroes(void *context, turbowasm_value rep, uint64_t size,
    turbowasm_wasi02_stream_error *error) {
    tw_cnet_slot *s = slot_from_rep(context, rep);
    if (!s || !s->output_live || !error || size > SIZE_MAX) return TURBOWASM_TRAPPED;
    return native_write(s, NULL, (size_t)size, true, error);
}
static turbowasm_status native_flush(void *context, turbowasm_value rep, turbowasm_wasi02_stream_error *error) {
    tw_cnet_slot *s = slot_from_rep(context, rep);
    if (!s || !s->output_live || !error) return TURBOWASM_TRAPPED;
    native_stream_error(s, false, error);
    if (error->kind == TURBOWASM_WASI02_STREAM_ERROR_NONE) { s->flushing = s->tx_busy; s->permit = 0; native_changed(s); }
    return TURBOWASM_OK;
}
static void native_input_drop(void *context, turbowasm_value rep) {
    tw_cnet_slot *s = slot_lookup(context, rep);
    if (s && s->input_live) { s->input_live = false; --s->refs; s->rx_closed = true; s->rx_size = 0; s->shutdown_pending |= 1u; native_changed(s); }
}
static void native_output_drop(void *context, turbowasm_value rep) {
    tw_cnet_slot *s = slot_lookup(context, rep);
    if (s && s->output_live) { s->output_live = false; --s->refs; s->tx_closed = true; s->permit = 0; s->shutdown_pending |= 2u; native_changed(s); }
}
static turbowasm_status native_error_debug(void *context, turbowasm_value rep, turbowasm_wasi02_string_view *out) {
    static const char message[] = "TCP transport operation failed"; (void)context;
    if (!out || rep.kind != TURBOWASM_VALUE_I64 || rep.as.i64 < 0 || rep.as.i64 > TURBOWASM_WASI02_SOCKET_ERROR_PERMANENT_RESOLVER_FAILURE)
        return TURBOWASM_INVALID_ARGUMENT;
    *out = (turbowasm_wasi02_string_view){(const uint8_t *)message, sizeof(message) - 1}; return TURBOWASM_OK;
}
static void native_error_drop(void *context, turbowasm_value rep) { (void)context; (void)rep; /* Immutable scalar error representation. */ }
static void tx_storage_free(void *data, void *context) { (void)context; turbowasm_rt_free(data); }
static void native_storage_release(tw_cnet_slot *s) {
    if (s->tx) mem_buffer_release(s->tx);
    turbowasm_rt_free(s->rx); s->rx = NULL; s->tx = NULL;
    s->owner->payload_used -= s->payload_charge; s->payload_charge = 0;
}
static void native_drop_pending(tw_cnet_slot *s) {
    turbowasm_wasi02_stream_provider ops; turbowasm_wasi02_poll_provider poll;
    if (s->input_rep.kind == TURBOWASM_VALUE_I64 || s->output_rep.kind == TURBOWASM_VALUE_I64) {
        if (turbowasm_wasi02_io_providers(s->owner->io, &ops, &poll) != TURBOWASM_OK) return;
        if (s->input_rep.kind == TURBOWASM_VALUE_I64) { turbowasm_value rep = s->input_rep; memset(&s->input_rep, 0, sizeof(rep)); ops.input_drop(ops.context, rep); }
        if (s->output_rep.kind == TURBOWASM_VALUE_I64) { turbowasm_value rep = s->output_rep; memset(&s->output_rep, 0, sizeof(rep)); ops.output_drop(ops.context, rep); }
    }
}
static turbowasm_status native_prepare_streams(tw_cnet_slot *s) {
    tw_cnet_impl *p = s->owner; turbowasm_runtime_scope scope; uint8_t *tx = NULL;
    turbowasm_wasi02_stream_provider ops = {0}; turbowasm_value rep = {0}; turbowasm_status status;
    size_t bytes = p->config.receive_bytes + p->config.send_bytes;
    if (s->payload_charge || bytes > p->config.payload_bytes - p->payload_used) return TURBOWASM_OUT_OF_MEMORY;
    p->payload_used += bytes; s->payload_charge = bytes;
    scope = turbowasm_runtime_scope_enter(&p->runtime);
    s->rx = turbowasm_rt_malloc(p->config.receive_bytes); tx = turbowasm_rt_malloc(p->config.send_bytes);
    if (tx) s->tx = mem_wrap_external(tx, p->config.send_bytes, tx_storage_free, NULL);
    if (tx && !s->tx) turbowasm_rt_free(tx);
    turbowasm_runtime_scope_leave(scope);
    if (!s->rx || !s->tx) { status = TURBOWASM_OUT_OF_MEMORY; goto fail; }
    for (unsigned i = 1; i < 3; ++i) {
        status = native_source_register(s, i); if (status != TURBOWASM_OK) goto fail;
    }
    rep.kind = TURBOWASM_VALUE_I64; rep.as.i64 = (int64_t)s->token;
    ops.context = p; ops.input_read = native_input_read; ops.input_skip = native_input_skip;
    ops.output_check_write = native_output_check; ops.output_write = native_output_write;
    ops.output_flush = native_flush; ops.output_write_zeroes = native_output_zeroes;
    ops.input_drop = native_input_drop; ops.output_drop = native_output_drop;
    ops.error_debug = native_error_debug; ops.error_drop = native_error_drop;
    status = turbowasm_wasi02_io_stream_register(p->io, TURBOWASM_WASI02_IO_INPUT, &ops, rep, s->sources[1], &s->input_rep);
    if (status != TURBOWASM_OK) goto fail;
    s->input_live = true; ++s->refs;
    status = turbowasm_wasi02_io_stream_register(p->io, TURBOWASM_WASI02_IO_OUTPUT, &ops, rep, s->sources[2], &s->output_rep);
    if (status != TURBOWASM_OK) goto fail;
    s->output_live = true; ++s->refs; return TURBOWASM_OK;
fail:
    native_drop_pending(s);
    for (unsigned i = 1; i < 3; ++i) if (s->sources[i].token) (void)turbowasm_wasi02_io_source_close(p->io, &s->sources[i]);
    native_storage_release(s); s->rx_closed = s->tx_closed = false; s->shutdown_pending = 0; return status;
}
static turbowasm_status native_start_connect(void *context, turbowasm_value rep, turbowasm_value network,
    const turbowasm_wasi02_ip_socket_address *remote, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = context; tw_cnet_slot *s = slot_from_rep(p, rep); cnet_stream_endpoint endpoint = CNET_STREAM_ENDPOINT_INIT;
    turbowasm_status status; cnet_observer observer; int rc;
    if (!p || p->busy || p->stopping || !error || !s || !s->socket_live) return TURBOWASM_INVALID_ARGUMENT;
    if (!network_rep_valid(p, network) || !endpoint_from_wasi(remote, &endpoint) || remote->family != s->family) {
        *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT; return TURBOWASM_OK; }
    if (!permitted(p, TURBOWASM_WASI02_CNET_CONNECT, remote)) { *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK; }
    if (s->state != TW_CNET_SLOT_UNBOUND && s->state != TW_CNET_SLOT_BOUND) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    p->busy = true; status = native_prepare_streams(s);
    if (status != TURBOWASM_OK) { p->busy = false; *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    observer = native_observer(s);
    rc = cnet_listener_connect_endpoint(&s->listener, &p->client, &endpoint, &observer, &s->connection);
    *error = map_error(rc);
    if (rc == SALTS_OK) { s->connection_live = true; s->state = TW_CNET_SLOT_CONNECTING; }
    else { native_drop_pending(s); s->state = TW_CNET_SLOT_CLOSED; }
    native_changed(s); p->busy = false; return TURBOWASM_OK;
}
static turbowasm_status native_finish_connect(void *context, turbowasm_value rep, turbowasm_value *input,
    turbowasm_value *output, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = context; tw_cnet_slot *s = slot_from_rep(p, rep);
    if (!p || p->busy || !s || !input || !output || !error) return TURBOWASM_INVALID_ARGUMENT;
    if (s->state != TW_CNET_SLOT_CONNECTING) { *error = TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS; return TURBOWASM_OK; }
    if (!s->connected && !s->terminal) { *error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK; return TURBOWASM_OK; }
    if (!s->connected && s->terminal) { *error = map_error(s->terminal_status == SALTS_OK ? SALTS_ECONNABORTED : s->terminal_status);
        native_drop_pending(s); s->state = TW_CNET_SLOT_CLOSED; return TURBOWASM_OK; }
    if (!s->terminal && !s->eof_enabled) {
        int rc = cnet_connection_preserve_send_on_eof(&p->client, s->connection);
        if (rc != SALTS_OK) { *error = map_error(rc); return TURBOWASM_OK; }
        s->eof_enabled = true;
    }
    *input = s->input_rep; *output = s->output_rep; memset(&s->input_rep, 0, sizeof(*input)); memset(&s->output_rep, 0, sizeof(*output));
    s->state = TW_CNET_SLOT_CONNECTED; *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE; native_changed(s); return TURBOWASM_OK;
}
static turbowasm_status native_accept(void *context, turbowasm_value rep, turbowasm_value *child,
    turbowasm_value *input, turbowasm_value *output, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = context; tw_cnet_slot *listener = slot_from_rep(p, rep), *s;
    cnet_observer observer; uint32_t index; turbowasm_status status; int rc;
    cnet_stream_endpoint peer = CNET_STREAM_ENDPOINT_INIT; cnet_stream_peer accepted_peer = {0};
    turbowasm_wasi02_ip_socket_address address;
    if (!p || p->busy || p->stopping || !listener || !child || !input || !output || !error) return TURBOWASM_INVALID_ARGUMENT;
    if (listener->state != TW_CNET_SLOT_LISTENING) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    if (!listener->accept_ready) { *error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK; return TURBOWASM_OK; }
    if (!p->config.allow_accept) { *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK; }
    if (listener->accept_status != SALTS_OK) { *error = map_error(listener->accept_status); listener->accept_ready = false; return TURBOWASM_OK; }
    s = reserve_slot(p, listener->family, &index);
    if (!s) { *error = TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT; return TURBOWASM_OK; }
    p->busy = true; status = native_source_register(s, 0);
    if (status == TURBOWASM_OK) status = native_prepare_streams(s);
    if (status != TURBOWASM_OK) { native_drop_pending(s); s->socket_live = false; --s->refs;
        *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; p->busy = false; return TURBOWASM_OK; }
    observer = native_observer(s); rc = cnet_listener_accept_peer(&listener->listener, &p->client, &observer, &s->connection, &accepted_peer);
    listener->accept_ready = false;
    if (rc == SALTS_OK) {
        size_t events;
        s->connection_live = true; s->state = TW_CNET_SLOT_CONNECTED;
        /* Commit CNet's guaranteed connected callback before publishing a WIT
         * connected child. This advances owner state without observing I/O. */
        rc = cnet_client_advance_external(&p->client, &events);
        if (rc == SALTS_OK && !s->connected) rc = SALTS_EPROTO;
        if (rc == SALTS_OK) rc = cnet_connection_preserve_send_on_eof(&p->client, s->connection);
        if (rc == SALTS_OK) s->eof_enabled = true;
        if (rc == SALTS_OK) rc = cnet_connection_remote_endpoint(&p->client, s->connection, &peer);
        if (rc == SALTS_OK && (!endpoint_to_wasi(&peer, &address) || !permitted(p, TURBOWASM_WASI02_CNET_ACCEPT, &address))) rc = SALTS_EPERM;
    }
    if (rc != SALTS_OK) {
        native_drop_pending(s); s->socket_live = false; --s->refs; s->closing = s->connection_live;
        *error = rc == SALTS_ETIMEDOUT ? TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK : map_error(rc);
        p->busy = false; return TURBOWASM_OK;
    }
    child->kind = TURBOWASM_VALUE_I64; child->as.i64 = (int64_t)s->token;
    *input = s->input_rep; *output = s->output_rep; memset(&s->input_rep, 0, sizeof(*input)); memset(&s->output_rep, 0, sizeof(*output));
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE; p->busy = false; native_changed(listener); return TURBOWASM_OK;
}
static turbowasm_status native_remote_address(void *context, turbowasm_value rep,
    turbowasm_wasi02_ip_socket_address *address, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = context; tw_cnet_slot *s = slot_from_rep(p, rep); cnet_stream_endpoint peer = CNET_STREAM_ENDPOINT_INIT; int rc;
    if (!p || p->busy || !s || !address || !error) return TURBOWASM_INVALID_ARGUMENT;
    if (!s->connected || s->terminal) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    rc = cnet_connection_remote_endpoint(&p->client, s->connection, &peer); *error = map_error(rc);
    if (rc == SALTS_OK && !endpoint_to_wasi(&peer, address)) return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}
static turbowasm_status native_subscribe(void *context, turbowasm_value rep, turbowasm_value *out) {
    tw_cnet_impl *p = context; tw_cnet_slot *s = slot_from_rep(p, rep);
    if (!p || p->busy || !s || !s->socket_live || !out) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_wasi02_io_pollable_register(p->io, s->sources[0], out);
}
static turbowasm_status native_is_closed(void *context, turbowasm_value rep, bool *out) {
    tw_cnet_slot *s = slot_from_rep(context, rep);
    if (!s || !s->socket_live || !out) return TURBOWASM_INVALID_ARGUMENT;
    *out = s->terminal; return TURBOWASM_OK;
}
extern turbowasm_status turbowasm_wasi02_set_transport_observer(turbowasm_wasi02 *,
    turbowasm_status (*)(void *, turbowasm_value, bool *), void *, void (*)(void *));
static void native_facade_release(void *context) { --((tw_cnet_impl *)context)->facades; }
turbowasm_status turbowasm_wasi02_cnet_wasi02_init(turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02 *wasi, const turbowasm_wasi02_config *config, const turbowasm_runtime_config *runtime) {
    tw_cnet_impl *p = impl_mut(adapter); turbowasm_wasi02_config copy; turbowasm_status status;
    if (!p || !p->external || p->busy || p->stopping || p->facades == UINT32_MAX || !config ||
        !config->tcp_socket_resource_capacity || !config->socket_network_resource_capacity ||
        (config->sockets.context && config->sockets.context != p)) return TURBOWASM_INVALID_ARGUMENT;
    copy = *config; status = turbowasm_wasi02_cnet_socket_provider(adapter, &copy.sockets);
    if (status != TURBOWASM_OK) return status;
    p->busy = true; ++p->facades;
    status = turbowasm_wasi02_io_wasi02_init(p->io, wasi, &copy, runtime);
    if (status == TURBOWASM_OK) {
        status = turbowasm_wasi02_set_transport_observer(wasi, native_is_closed, p, native_facade_release);
        if (status != TURBOWASM_OK) (void)turbowasm_wasi02_destroy(wasi);
    }
    if (status != TURBOWASM_OK) --p->facades;
    p->busy = false; return status;
}

static turbowasm_status native_shutdown(void *context, turbowasm_value rep, turbowasm_wasi02_tcp_shutdown_type how,
    turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = context; tw_cnet_slot *s = slot_from_rep(p, rep); int rc;
    if (!p || p->busy || !s || !error) return TURBOWASM_INVALID_ARGUMENT;
    if (how < TURBOWASM_WASI02_TCP_SHUTDOWN_RECEIVE || how > TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH) {
        *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT; return TURBOWASM_OK;
    }
    if (!s->connected || s->terminal) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    rc = cnet_connection_shutdown(&p->client, s->connection, (cnet_tcp_shutdown)((unsigned)how + 1u)); *error = map_error(rc);
    if (rc == SALTS_OK) {
        if (how == TURBOWASM_WASI02_TCP_SHUTDOWN_RECEIVE || how == TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH) { s->rx_closed = true; s->rx_size = 0; }
        if (how == TURBOWASM_WASI02_TCP_SHUTDOWN_SEND || how == TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH) { s->tx_closed = true; s->permit = 0; }
        native_changed(s);
    }
    return TURBOWASM_OK;
}

void turbowasm_wasi02_cnet_config_init(turbowasm_wasi02_cnet_config *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config)); config->size = sizeof(*config); config->api_version = 1;
#if defined(_WIN32)
    config->backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    config->backend = NATIVE_IO_BACKEND_KQUEUE;
#else
    config->backend = NATIVE_IO_BACKEND_EPOLL;
#endif
    config->socket_capacity = 64; config->default_listen_backlog = TW_CNET_DEFAULT_BACKLOG;
    config->receive_bytes = config->send_bytes = 65536; config->payload_bytes = 16777216;
    config->connect_timeout_ms = 30000;
}

turbowasm_status turbowasm_wasi02_cnet_init_external(turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_io *io, native_io_backend *backend,
    const turbowasm_wasi02_cnet_config *config, const turbowasm_runtime_config *runtime) {
    tw_cnet_impl *p; turbowasm_runtime_config normalized; turbowasm_runtime_scope scope;
    native_io_backend_config native_config; native_io_backend_stats stats;
    cnet_client_config cc = {0}; int rc;
    tw_cnet_impl constructing = {0};
    if (!adapter || adapter->impl || !config || config->size != sizeof(*config) || config->api_version != 1 ||
        !config->socket_capacity || config->socket_capacity > UINT32_MAX / 8u ||
        !config->receive_bytes || !config->send_bytes || !config->connect_timeout_ms ||
        config->receive_bytes > SIZE_MAX - config->send_bytes ||
        config->payload_bytes < config->receive_bytes + config->send_bytes ||
        !turbowasm_runtime_config_normalize(runtime, &normalized) ||
        !native_io_backend_get_config(backend, &native_config) ||
        !native_io_backend_get_stats(backend, &stats) || !stats.admission_open ||
        native_config.kind != config->backend || native_config.endpoint_capacity < (size_t)config->socket_capacity * 2u ||
        native_config.request_capacity < (size_t)config->socket_capacity * 3u ||
        !turbowasm_wasi02_io_retain_private(io)) return TURBOWASM_INVALID_ARGUMENT;
    constructing.busy = constructing.external = true; adapter->impl = &constructing;
    scope = turbowasm_runtime_scope_enter(&normalized);
    p = turbowasm_rt_calloc(1, sizeof(*p));
    if (p) {
        p->slots = turbowasm_rt_calloc(config->socket_capacity, sizeof(*p->slots));
        p->free_indices = turbowasm_rt_calloc(config->socket_capacity, sizeof(*p->free_indices));
    }
    turbowasm_runtime_scope_leave(scope);
    if (!p || !p->slots || !p->free_indices) {
        if (p) { turbowasm_rt_free(p->slots); turbowasm_rt_free(p->free_indices); turbowasm_rt_free(p); }
        turbowasm_wasi02_io_release_private(turbowasm_wasi02_io_context_private(io)); adapter->impl = NULL; return TURBOWASM_OUT_OF_MEMORY;
    }
    p->config = *config; p->runtime = normalized; p->external = true; p->io = io; p->native_backend = backend;
    p->backend = config->backend; p->capacity = p->free_count = config->socket_capacity; p->network_generation = 1;
    p->network_token = turbowasm_wasi02_io_token_private();
    p->default_listen_backlog = config->default_listen_backlog ? config->default_listen_backlog : TW_CNET_DEFAULT_BACKLOG;
    for (uint32_t i = 0; i < p->capacity; ++i) p->free_indices[i] = p->capacity - 1u - i;
    cc.backend = config->backend; cc.connection_capacity = p->capacity;
    cc.command_capacity = 2u;
    while (cc.command_capacity < (size_t)p->capacity * 4u) cc.command_capacity *= 2u;
    cc.event_capacity = cc.command_capacity;
    cc.request_capacity = (size_t)p->capacity * 3u; cc.completion_batch_capacity = native_config.completion_batch_capacity < cc.request_capacity ? native_config.completion_batch_capacity : cc.request_capacity;
    cc.max_send_bytes = config->send_bytes; cc.receive_buffer_bytes = config->receive_bytes;
    cc.connect_timeout_ms = config->connect_timeout_ms; cc.read_timeout_ms = config->read_timeout_ms;
    cc.write_timeout_ms = config->write_timeout_ms;
    rc = p->network_token ? cnet_client_init_external(&p->client, &cc, backend) : SALTS_ENOMEM;
    if (rc != SALTS_OK) {
        turbowasm_rt_free(p->slots); turbowasm_rt_free(p->free_indices); turbowasm_rt_free(p);
        turbowasm_wasi02_io_release_private(turbowasm_wasi02_io_context_private(io));
        adapter->impl = NULL;
        return rc == SALTS_ENOMEM || rc == SALTS_ENOBUFS ? TURBOWASM_OUT_OF_MEMORY : TURBOWASM_INVALID_ARGUMENT;
    }
    adapter->impl = p; return TURBOWASM_OK;
}

#include "wasi02_cnet_network.inc"

static void native_cleanup_error(tw_cnet_impl *p, int rc) {
    if (rc != SALTS_OK && rc != SALTS_EALREADY && rc != SALTS_EBUSY && p->cleanup_error == SALTS_OK)
        p->cleanup_error = rc;
}
static void native_retire_sources(tw_cnet_slot *s) {
    tw_cnet_impl *p = s->owner;
    bool close[3] = {!s->socket_live || p->stopping, !s->input_live || p->stopping, !s->output_live || p->stopping};
    for (unsigned i = 0; i < 3; ++i) if (close[i] && s->sources[i].token)
        (void)turbowasm_wasi02_io_source_close(p->io, &s->sources[i]);
}
turbowasm_status turbowasm_wasi02_cnet_advance(turbowasm_wasi02_cnet *adapter, size_t *events) {
    tw_cnet_impl *p = impl_mut(adapter); int rc;
    if (!p || !p->external || p->busy || !events) return TURBOWASM_INVALID_ARGUMENT;
    *events = 0; if (p->stopped) return TURBOWASM_OK;
    p->busy = true;
    for (uint32_t i = 0; i < p->capacity; ++i) {
        tw_cnet_slot *s = &p->slots[i];
        if (!s->active) continue;
        if (!s->socket_live || p->stopping) native_drop_pending(s);
        native_retire_sources(s);
        if (p->stopping || (!s->socket_live && !s->input_live && !s->output_live)) s->closing = true;
        if (s->connection_live && !s->terminal) {
            if (s->closing && !s->close_requested) {
                rc = cnet_close(&p->client, s->connection);
                if (rc == SALTS_OK || rc == SALTS_EALREADY) s->close_requested = true;
                else if (rc != SALTS_ENOBUFS) native_cleanup_error(p, rc);
            } else if (!s->closing) {
                if (s->connected && !s->eof_enabled) {
                    rc = cnet_connection_preserve_send_on_eof(&p->client, s->connection);
                    if (rc == SALTS_OK) s->eof_enabled = true;
                    else if (rc != SALTS_EBUSY) { s->terminal_status = rc; s->closing = true; }
                }
                if (s->shutdown_pending) {
                    rc = cnet_connection_shutdown(&p->client, s->connection, (cnet_tcp_shutdown)s->shutdown_pending);
                    if (rc == SALTS_OK) s->shutdown_pending = 0;
                    else if (rc != SALTS_ENOBUFS && rc != SALTS_EBUSY) native_cleanup_error(p, rc);
                }
                if (s->connected && s->eof_enabled && !s->rx_closed && !s->rx_pending && !s->rx_size) {
                    rc = cnet_receive(&p->client, s->connection, 1);
                    if (rc == SALTS_OK) s->rx_pending = true;
                    else if (rc != SALTS_ENOBUFS && rc != SALTS_EBUSY) { s->terminal_status = rc; s->closing = true; }
                }
            }
        }
        if (s->listener.impl) {
            if (s->closing) {
                rc = cnet_listener_close(&s->listener); native_cleanup_error(p, rc);
                if (rc == SALTS_OK || rc == SALTS_EALREADY) native_cleanup_error(p, cnet_listener_destroy(&s->listener));
            } else if (s->state == TW_CNET_SLOT_LISTENING && !s->accept_ready) {
                if (!p->config.allow_accept) {
                    s->accept_status = SALTS_EPERM; s->accept_ready = true; native_changed(s); continue;
                }
                if (!s->listener_attached) {
                    rc = cnet_listener_attach_external(&s->listener, p->native_backend);
                    if (rc == SALTS_OK) s->listener_attached = true;
                    else { s->accept_status = rc; s->accept_ready = true; }
                }
                if (s->listener_attached && !s->accept_ready) {
                    rc = cnet_listener_submit_external_accept(&s->listener, &s->accept_request);
                    if (rc == SALTS_OK) s->accept_pending = true;
                    else if (rc == SALTS_EALREADY) s->accept_ready = true;
                    else if (rc != SALTS_ENOBUFS) { s->accept_status = rc; s->accept_ready = true; }
                }
                native_changed(s);
            }
        }
    }
    rc = cnet_client_advance_external(&p->client, events);
    native_cleanup_error(p, rc);
    /* A terminal callback settles every CNet retained send/receive payload.
     * Source aliases may keep metadata alive beyond that terminal event. */
    for (uint32_t i = 0; i < p->capacity; ++i) {
        tw_cnet_slot *s = &p->slots[i];
        if (s->active && s->refs == 0 && (!s->connection_live || s->terminal) && !s->listener.impl) {
            native_storage_release(s); release_slot(p, s);
        }
    }
    if (rc == SALTS_OK) rc = cnet_network_advance(p);
    p->busy = false;
    return rc == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}
turbowasm_status turbowasm_wasi02_cnet_route_completion(turbowasm_wasi02_cnet *adapter,
    const native_io_completion *completion, bool *consumed) {
    tw_cnet_impl *p = impl_mut(adapter); size_t events; int rc;
    if (!p || !p->external || p->busy || !completion || !consumed) return TURBOWASM_INVALID_ARGUMENT;
    p->busy = true; *consumed = false;
    rc = cnet_client_route_external_completion(&p->client, completion, consumed, &events);
    if (rc == SALTS_OK && !*consumed) for (uint32_t i = 0; i < p->capacity; ++i) {
        tw_cnet_slot *s = &p->slots[i];
        if (!s->active || !s->accept_pending || s->accept_request.slot != completion->request.slot ||
            s->accept_request.generation != completion->request.generation) continue;
        rc = cnet_listener_route_external_completion(&s->listener, completion, consumed);
        if (*consumed) {
            s->accept_pending = false; s->accept_ready = true;
            s->accept_status = completion->status;
            if (s->accept_status == SALTS_OK && completion->kind != NATIVE_IO_COMPLETION_OK) s->accept_status = SALTS_EIO;
            native_changed(s);
        }
        break;
    }
    if (rc == SALTS_OK && !*consumed) rc = cnet_network_route(p, completion, consumed);
    native_cleanup_error(p, rc); p->busy = false;
    return rc == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}
turbowasm_status turbowasm_wasi02_cnet_next_timeout(turbowasm_wasi02_cnet *adapter,
    uint32_t max, uint32_t *timeout) {
    tw_cnet_impl *p = impl_mut(adapter);
    if (!p || !p->external || p->busy || !timeout) return TURBOWASM_INVALID_ARGUMENT;
    if (cnet_client_external_timeout(&p->client, max, timeout) != SALTS_OK) return TURBOWASM_TRAPPED;
    if (p->network && p->network->lookup.impl && cnet_name_lookup_next_timeout(&p->network->lookup, *timeout, timeout) != SALTS_OK) return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_cnet_shutdown_request(turbowasm_wasi02_cnet *adapter) {
    tw_cnet_impl *p = impl_mut(adapter);
    if (!p || !p->external || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    p->stopping = true;
    if (p->network && p->network->lookup.impl && cnet_name_lookup_close(&p->network->lookup) != SALTS_OK) return TURBOWASM_TRAPPED;
    for (uint32_t i = 0; i < p->capacity; ++i) if (p->slots[i].active) {
        tw_cnet_slot *s = &p->slots[i]; s->rx_closed = s->tx_closed = true; s->rx_size = s->permit = 0; native_changed(s);
    }
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_cnet_shutdown_poll(turbowasm_wasi02_cnet *adapter, bool *complete) {
    tw_cnet_impl *p = impl_mut(adapter); size_t events; int rc;
    if (!p || !p->external || p->busy || !p->stopping || !complete) return TURBOWASM_INVALID_ARGUMENT;
    *complete = false;
    if (turbowasm_wasi02_cnet_advance(adapter, &events) != TURBOWASM_OK) return TURBOWASM_TRAPPED;
    if (p->active_count || cnet_network_active(p)) return p->cleanup_error == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
    rc = cnet_client_stop_external(&p->client);
    if (rc == SALTS_EBUSY) return TURBOWASM_OK;
    native_cleanup_error(p, rc);
    if (rc == SALTS_OK) p->stopped = true;
    *complete = p->stopped && !p->active_count && !p->network_live;
    return p->cleanup_error == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}

#endif /* TURBOWASM_WASI02_NATIVE_TCP */

turbowasm_status turbowasm_wasi02_cnet_init(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config) {
    tw_cnet_impl *impl;
    uint32_t i;

    if (adapter == NULL || adapter->impl != NULL ||
        config == NULL || config->socket_capacity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (tw_cnet_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    impl->slots = (tw_cnet_slot *)calloc(
        config->socket_capacity, sizeof(*impl->slots));
    impl->free_indices = (uint32_t *)malloc(
        (size_t)config->socket_capacity *
        sizeof(*impl->free_indices));
    if (impl->slots == NULL || impl->free_indices == NULL) {
        free(impl->free_indices);
        free(impl->slots);
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->backend = config->backend;
    impl->capacity = config->socket_capacity;
    impl->free_count = config->socket_capacity;
    impl->network_generation = 1u;
    impl->default_listen_backlog =
        config->default_listen_backlog != 0u
            ? config->default_listen_backlog
            : (size_t)TW_CNET_DEFAULT_BACKLOG;
    for (i = 0u; i < impl->capacity; ++i)
        impl->free_indices[i] = impl->capacity - 1u - i;

    adapter->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (adapter == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl == NULL)
        return TURBOWASM_OK;
    if (impl->active_count != 0u || impl->network_live != 0u || impl->facades || impl->busy ||
        (impl->external && !impl->stopped))
        return TURBOWASM_INVALID_ARGUMENT;

#if defined(TURBOWASM_WASI02_NATIVE_TCP)
    if (impl->external) {
        if (cnet_network_destroy(impl) != SALTS_OK) return TURBOWASM_TRAPPED;
        if (cnet_client_destroy(&impl->client) != SALTS_OK) return TURBOWASM_TRAPPED;
        turbowasm_wasi02_io_release_private(turbowasm_wasi02_io_context_private(impl->io));
        adapter->impl = NULL;
        turbowasm_rt_free(impl->free_indices); turbowasm_rt_free(impl->slots); turbowasm_rt_free(impl);
        return TURBOWASM_OK;
    }
#endif

    free(impl->free_indices);
    free(impl->slots);
    free(impl);
    adapter->impl = NULL;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_cnet_socket_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_socket_provider *out_provider) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (impl == NULL || impl->busy || out_provider == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_provider, 0, sizeof(*out_provider));
    out_provider->context = impl;
    out_provider->instance_network = provider_instance_network;
    out_provider->network_drop = provider_network_drop;
    out_provider->tcp_create = provider_tcp_create;
    out_provider->tcp_drop = provider_tcp_drop;
    out_provider->tcp_start_bind = provider_start_bind;
    out_provider->tcp_finish_bind = provider_finish_bind;
    out_provider->tcp_start_listen = provider_start_listen;
    out_provider->tcp_finish_listen = provider_finish_listen;
    out_provider->tcp_local_address = provider_local_address;
    out_provider->tcp_set_listen_backlog_size = provider_set_backlog;
    out_provider->tcp_keep_alive_enabled = provider_get_bool;
    out_provider->tcp_set_keep_alive_enabled = provider_set_bool;
    out_provider->tcp_keep_alive_idle_time = provider_get_idle;
    out_provider->tcp_set_keep_alive_idle_time = provider_set_idle;
    out_provider->tcp_keep_alive_interval = provider_get_interval;
    out_provider->tcp_set_keep_alive_interval = provider_set_interval;
    out_provider->tcp_keep_alive_count = provider_get_count;
    out_provider->tcp_set_keep_alive_count = provider_set_count;
    out_provider->tcp_hop_limit = provider_get_hop;
    out_provider->tcp_set_hop_limit = provider_set_hop;
    out_provider->tcp_receive_buffer_size = provider_get_recv;
    out_provider->tcp_set_receive_buffer_size = provider_set_recv;
    out_provider->tcp_send_buffer_size = provider_get_send;
    out_provider->tcp_set_send_buffer_size = provider_set_send;
#if defined(TURBOWASM_WASI02_NATIVE_TCP)
    if (impl->external) {
        out_provider->tcp_start_connect = native_start_connect;
        out_provider->tcp_finish_connect = native_finish_connect;
        out_provider->tcp_accept = native_accept;
        out_provider->tcp_remote_address = native_remote_address;
        out_provider->tcp_subscribe = native_subscribe;
        out_provider->tcp_shutdown = native_shutdown;
    }
#endif
    return TURBOWASM_OK;
}
