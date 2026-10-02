#include "wasi02_cnet.h"

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
    TW_CNET_SLOT_LISTENING
} tw_cnet_slot_state;

typedef struct tw_cnet_slot {
    bool active;
    uint32_t generation;
    turbowasm_wasi02_ip_address_family family;
    tw_cnet_slot_state state;
    size_t listen_backlog;
    cnet_listener listener;
} tw_cnet_slot;

typedef struct tw_cnet_impl {
    native_io_backend_kind backend;
    tw_cnet_slot *slots;
    uint32_t *free_indices;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t network_live;
    uint32_t network_generation;
    size_t default_listen_backlog;
} tw_cnet_impl;

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

static tw_cnet_slot *slot_from_rep(
    tw_cnet_impl *impl,
    turbowasm_value rep) {
    uint32_t index;
    uint32_t generation;
    tw_cnet_slot *slot;

    if (impl == NULL ||
        !unpack_rep(rep, &index, &generation) ||
        index >= impl->capacity)
        return NULL;

    slot = &impl->slots[index];
    if (!slot->active || slot->generation != generation)
        return NULL;
    return slot;
}

static tw_cnet_slot *reserve_slot(
    tw_cnet_impl *impl,
    turbowasm_wasi02_ip_address_family family,
    uint32_t *out_index) {
    uint32_t index;
    uint32_t generation;
    tw_cnet_slot *slot;

    if (impl == NULL || out_index == NULL ||
        impl->free_count == 0u)
        return NULL;

    index = impl->free_indices[--impl->free_count];
    slot = &impl->slots[index];
    generation = slot->generation + 1u;
    if (generation == 0u)
        generation = 1u;

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = generation;
    slot->family = family;
    slot->state = TW_CNET_SLOT_UNBOUND;
    slot->listen_backlog = impl->default_listen_backlog;
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
    impl->free_indices[impl->free_count++] = index;
}

static bool network_rep_valid(
    const tw_cnet_impl *impl,
    turbowasm_value rep) {
    return impl != NULL &&
           rep.kind == TURBOWASM_VALUE_I64 &&
           impl->network_generation != 0u &&
           (uint64_t)rep.as.i64 ==
               (uint64_t)impl->network_generation;
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
        impl->network_live == UINT32_MAX)
        return TURBOWASM_INVALID_ARGUMENT;

    ++impl->network_live;
    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)impl->network_generation;
    return TURBOWASM_OK;
}

static turbowasm_status provider_network_drop(
    void *context,
    turbowasm_value rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;

    if (!network_rep_valid(impl, rep) ||
        impl->network_live == 0u)
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

    if (impl == NULL || out_rep == NULL || out_error == NULL ||
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

    status = cnet_listener_open(
        &slot->listener, impl->backend, cnet_family(family));
    if (status != SALTS_OK) {
        *out_error = map_error(status);
        release_slot(impl, slot);
        return TURBOWASM_OK;
    }

    out_rep->kind = TURBOWASM_VALUE_I64;
    out_rep->as.i64 = (int64_t)pack_rep(index, slot->generation);
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

    status = cnet_listener_local_endpoint(
        &slot->listener, &endpoint);
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

turbowasm_status turbowasm_wasi02_cnet_init(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config) {
    tw_cnet_impl *impl;
    uint32_t i;

    if (adapter == NULL || adapter->impl != NULL ||
        config == NULL || config->socket_capacity == 0u ||
        config->socket_capacity > UINT32_MAX)
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
    if (impl->free_count != impl->capacity ||
        impl->network_live != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

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

    if (impl == NULL || out_provider == NULL)
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
    return TURBOWASM_OK;
}
