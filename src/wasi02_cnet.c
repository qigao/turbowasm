#include "wasi02_cnet.h"

#include <salts/error_codes.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TW_CNET_DEFAULT_BACKLOG = 128u
};

typedef struct tw_cnet_impl tw_cnet_impl;

typedef enum tw_cnet_slot_state {
    TW_CNET_SLOT_FREE = 0,
    TW_CNET_SLOT_UNBOUND,
    TW_CNET_SLOT_BOUND,
    /*
     * The native listen call has completed, but the outer WASI state machine
     * has not consumed finish-listen yet. Socket subscribe is ready here.
     */
    TW_CNET_SLOT_LISTEN_READY,
    TW_CNET_SLOT_LISTENING,
    TW_CNET_SLOT_CONNECTING,
    TW_CNET_SLOT_CONNECTED,
    TW_CNET_SLOT_CLOSED
} tw_cnet_slot_state;

typedef struct tw_cnet_slot {
    bool active;
    bool guest_dropped;
    bool external_attached;
    bool accept_request_active;
    bool accept_terminal;

    bool connection_active;
    bool connection_connected;
    bool connection_terminal;
    bool connection_close_requested;
    bool streams_issued;

    uint32_t generation;
    uint32_t poll_leases;
    uint32_t input_stream_leases;
    uint32_t output_stream_leases;

    turbowasm_wasi02_ip_address_family family;
    tw_cnet_slot_state state;
    size_t listen_backlog;
    int connection_status;

    native_io_request accept_request;
    cnet_listener listener;
    cnet_connection connection;
    tw_cnet_impl *owner;
} tw_cnet_slot;

struct tw_cnet_impl {
    native_io_backend_kind backend;
    native_io_backend *external_backend;
    cnet_client client;
    bool external_stopped;
    tw_cnet_slot *slots;
    uint32_t *free_indices;
    uint32_t capacity;
    uint32_t free_count;
    uint32_t network_live;
    uint32_t network_generation;
    size_t default_listen_backlog;

    void *poll_registry_context;
    turbowasm_wasi02_cnet_poll_register_fn poll_register;
};

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

static turbowasm_status map_backend_status(int status) {
    switch (status) {
        case SALTS_OK:
            return TURBOWASM_OK;
        case SALTS_EINVAL:
        case SALTS_EFAULT:
        case SALTS_ERANGE:
        case SALTS_EBUSY:
        case SALTS_EALREADY:
        case SALTS_ESHUTDOWN:
            return TURBOWASM_INVALID_ARGUMENT;
        case SALTS_ENOMEM:
        case SALTS_ENOBUFS:
            return TURBOWASM_OUT_OF_MEMORY;
        case SALTS_ENOTSUP:
        case SALTS_ENOSYS:
            return TURBOWASM_UNSUPPORTED;
        default:
            return TURBOWASM_TRAPPED;
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
    if (!slot->active || slot->guest_dropped ||
        slot->generation != generation)
        return NULL;
    return slot;
}

static tw_cnet_slot *slot_from_poll_rep(
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
    if (!slot->active ||
        slot->generation != generation)
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
    slot->owner = impl;
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

static bool request_equal(
    native_io_request left,
    native_io_request right) {
    return native_io_request_valid(left) &&
           native_io_request_valid(right) &&
           left.slot == right.slot &&
           left.generation == right.generation;
}

static void maybe_release_tombstone(
    tw_cnet_impl *impl,
    tw_cnet_slot *slot);

static bool connection_equal(
    cnet_connection left,
    cnet_connection right) {
    return left.slot == right.slot &&
           left.generation == right.generation &&
           left.slot != 0u &&
           left.generation != 0u;
}

static void connection_on_state(
    void *user,
    cnet_connection connection,
    cnet_connection_state state,
    const cnet_error *error) {
    tw_cnet_slot *slot = (tw_cnet_slot *)user;
    tw_cnet_impl *impl;

    if (slot == NULL || !slot->active ||
        !slot->connection_active ||
        !connection_equal(slot->connection, connection))
        return;

    impl = slot->owner;
    switch (state) {
        case CNET_CONNECTION_CONNECTING:
        case CNET_CONNECTION_TLS_HANDSHAKING:
            slot->state = TW_CNET_SLOT_CONNECTING;
            break;

        case CNET_CONNECTION_CONNECTED:
            slot->connection_connected = true;
            slot->connection_terminal = false;
            slot->connection_status = SALTS_OK;
            slot->state = TW_CNET_SLOT_CONNECTED;
            break;

        case CNET_CONNECTION_CLOSING:
            slot->connection_close_requested = true;
            break;

        case CNET_CONNECTION_CLOSED:
            slot->connection_connected = false;
            slot->connection_terminal = true;
            slot->connection_status =
                error != NULL ? error->status : SALTS_OK;
            slot->state = TW_CNET_SLOT_CLOSED;
            maybe_release_tombstone(impl, slot);
            break;

        case CNET_CONNECTION_FAILED:
            slot->connection_connected = false;
            slot->connection_terminal = true;
            slot->connection_status =
                error != NULL && error->status != SALTS_OK
                    ? error->status
                    : SALTS_EIO;
            slot->state = TW_CNET_SLOT_CLOSED;
            maybe_release_tombstone(impl, slot);
            break;

        default:
            break;
    }
}

static cnet_observer connection_observer(
    tw_cnet_slot *slot) {
    cnet_observer observer;
    memset(&observer, 0, sizeof(observer));
    observer.on_state = connection_on_state;
    observer.user = slot;
    return observer;
}

static turbowasm_value slot_rep(
    const tw_cnet_impl *impl,
    const tw_cnet_slot *slot) {
    turbowasm_value rep = {0};
    uint32_t index;

    if (impl == NULL || slot == NULL ||
        slot < impl->slots ||
        slot >= impl->slots + impl->capacity)
        return rep;

    index = (uint32_t)(slot - impl->slots);
    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = (int64_t)pack_rep(
        index, slot->generation);
    return rep;
}

static void maybe_release_tombstone(
    tw_cnet_impl *impl,
    tw_cnet_slot *slot) {
    if (impl == NULL || slot == NULL || !slot->active)
        return;
    if (!slot->guest_dropped ||
        slot->poll_leases != 0u ||
        slot->input_stream_leases != 0u ||
        slot->output_stream_leases != 0u ||
        slot->accept_request_active ||
        slot->listener.impl != NULL ||
        (slot->connection_active &&
         !slot->connection_terminal))
        return;
    release_slot(impl, slot);
}

static turbowasm_status finalize_listener_slot(
    tw_cnet_impl *impl,
    tw_cnet_slot *slot) {
    int status;

    if (impl == NULL || slot == NULL || !slot->active)
        return TURBOWASM_INVALID_ARGUMENT;

    status = cnet_listener_close(&slot->listener);
    if (status != SALTS_OK && status != SALTS_EALREADY)
        return status == SALTS_EBUSY
            ? TURBOWASM_INVALID_ARGUMENT
            : TURBOWASM_TRAPPED;

    status = cnet_listener_destroy(&slot->listener);
    if (slot->listener.impl != NULL)
        return status == SALTS_OK
            ? TURBOWASM_TRAPPED
            : TURBOWASM_INVALID_ARGUMENT;

    slot->external_attached = false;
    slot->state = TW_CNET_SLOT_CLOSED;
    maybe_release_tombstone(impl, slot);
    return TURBOWASM_OK;
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

    /*
     * Consume the guest socket identity immediately. Pollables and returned
     * streams keep independent slot leases until their own drops.
     */
    slot->guest_dropped = true;

    if (slot->connection_active) {
        if (!slot->connection_terminal &&
            !slot->connection_close_requested) {
            status = cnet_close(
                &impl->client, slot->connection);
            if (status != SALTS_OK &&
                status != SALTS_EALREADY &&
                status != SALTS_ENOENT &&
                status != SALTS_ESHUTDOWN)
                return TURBOWASM_TRAPPED;
            slot->connection_close_requested = true;
        }
        maybe_release_tombstone(impl, slot);
        return TURBOWASM_OK;
    }

    if (slot->listener.impl == NULL) {
        slot->state = TW_CNET_SLOT_CLOSED;
        maybe_release_tombstone(impl, slot);
        return TURBOWASM_OK;
    }

    status = cnet_listener_close(&slot->listener);
    if (status == SALTS_EBUSY) {
        /*
         * External accept cancellation is asynchronous. Keep the physical
         * slot/generation until the authoritative terminal completion drains.
         */
        return TURBOWASM_OK;
    }
    if (status != SALTS_OK && status != SALTS_EALREADY)
        return TURBOWASM_TRAPPED;

    status = cnet_listener_destroy(&slot->listener);
    if (slot->listener.impl != NULL)
        return status == SALTS_OK
            ? TURBOWASM_TRAPPED
            : TURBOWASM_INVALID_ARGUMENT;

    slot->external_attached = false;
    slot->state = TW_CNET_SLOT_CLOSED;
    maybe_release_tombstone(impl, slot);
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

static turbowasm_status issue_connection_stream_reps(
    tw_cnet_impl *impl,
    tw_cnet_slot *slot,
    turbowasm_value *out_input,
    turbowasm_value *out_output) {
    turbowasm_value rep;

    if (impl == NULL || slot == NULL ||
        out_input == NULL || out_output == NULL ||
        slot->streams_issued ||
        slot->input_stream_leases == UINT32_MAX ||
        slot->output_stream_leases == UINT32_MAX)
        return TURBOWASM_INVALID_ARGUMENT;

    rep = slot_rep(impl, slot);
    if (rep.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_TRAPPED;

    ++slot->input_stream_leases;
    ++slot->output_stream_leases;
    slot->streams_issued = true;
    *out_input = rep;
    *out_output = rep;
    return TURBOWASM_OK;
}

static void close_listener_after_connect_failure(
    tw_cnet_slot *slot) {
    int status;

    if (slot == NULL || slot->listener.impl == NULL)
        return;
    status = cnet_listener_close(&slot->listener);
    if (status == SALTS_OK || status == SALTS_EALREADY)
        (void)cnet_listener_destroy(&slot->listener);
}

static turbowasm_status provider_start_connect(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *remote_address,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    cnet_stream_endpoint endpoint = CNET_STREAM_ENDPOINT_INIT;
    cnet_observer observer;
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL ||
        (slot->state != TW_CNET_SLOT_UNBOUND &&
         slot->state != TW_CNET_SLOT_BOUND)) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }
    if (!network_rep_valid(impl, network_rep) ||
        remote_address == NULL ||
        !endpoint_from_wasi(remote_address, &endpoint) ||
        remote_address->family != slot->family) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
        return TURBOWASM_OK;
    }
    if (impl->external_backend == NULL ||
        impl->external_stopped) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_NOT_SUPPORTED;
        return TURBOWASM_OK;
    }

    observer = connection_observer(slot);
    status = cnet_listener_connect_endpoint(
        &slot->listener,
        &impl->client,
        &endpoint,
        &observer,
        &slot->connection);
    if (status != SALTS_OK) {
        /*
         * Once CNet reaches adoption the listener owner is consumed even when
         * admission later fails. Either way WASI start-connect failure closes
         * this socket resource.
         */
        close_listener_after_connect_failure(slot);
        slot->connection_active = false;
        slot->connection_connected = false;
        slot->connection_terminal = true;
        slot->connection_status = status;
        slot->state = TW_CNET_SLOT_CLOSED;
        *out_error = map_error(status);
        return TURBOWASM_OK;
    }

    if (!connection_equal(slot->connection, slot->connection))
        return TURBOWASM_TRAPPED;

    slot->connection_active = true;
    slot->connection_connected = false;
    slot->connection_terminal = false;
    slot->connection_close_requested = false;
    slot->connection_status = SALTS_OK;
    slot->state = TW_CNET_SLOT_CONNECTING;
    return TURBOWASM_OK;
}

static turbowasm_status provider_finish_connect(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_input,
    turbowasm_value *out_output,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    turbowasm_status status;

    if (out_input == NULL || out_output == NULL ||
        out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out_input, 0, sizeof(*out_input));
    memset(out_output, 0, sizeof(*out_output));
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL || !slot->connection_active) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    if (slot->connection_connected &&
        slot->state == TW_CNET_SLOT_CONNECTED) {
        status = issue_connection_stream_reps(
            impl, slot, out_input, out_output);
        if (status != TURBOWASM_OK)
            return status;
        return TURBOWASM_OK;
    }

    if (slot->connection_terminal) {
        *out_error = map_error(
            slot->connection_status != SALTS_OK
                ? slot->connection_status
                : SALTS_ECONNABORTED);
        return TURBOWASM_OK;
    }

    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
    return TURBOWASM_OK;
}

static turbowasm_status provider_accept(
    void *context,
    turbowasm_value listener_rep,
    turbowasm_value *out_socket,
    turbowasm_value *out_input,
    turbowasm_value *out_output,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *listener = slot_from_rep(
        impl, listener_rep);
    tw_cnet_slot *child;
    cnet_observer observer;
    turbowasm_status stream_status;
    uint32_t child_index = 0u;
    size_t events = 0u;
    int status;

    if (out_socket == NULL || out_input == NULL ||
        out_output == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_socket, 0, sizeof(*out_socket));
    memset(out_input, 0, sizeof(*out_input));
    memset(out_output, 0, sizeof(*out_output));
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (listener == NULL ||
        listener->state != TW_CNET_SLOT_LISTENING ||
        impl->external_backend == NULL ||
        impl->external_stopped) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    if (!listener->accept_terminal) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
        return TURBOWASM_OK;
    }

    child = reserve_slot(
        impl, listener->family, &child_index);
    if (child == NULL) {
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT;
        return TURBOWASM_OK;
    }

    /*
     * Keep the child hidden until CNet has published CONNECTED and both
     * stream leases have been created. Any failure before publication leaves
     * an internal tombstone that normal external progress can drain safely.
     */
    child->guest_dropped = true;
    observer = connection_observer(child);
    status = cnet_listener_accept(
        &listener->listener,
        &impl->client,
        &observer,
        &child->connection);

    /*
     * cnet_listener_accept() consumes exactly one pending accept result,
     * including a stored terminal error. The listener is not ready again
     * until a fresh external accept request settles.
     */
    listener->accept_terminal = false;

    if (status != SALTS_OK) {
        release_slot(impl, child);
        *out_error =
            status == SALTS_ETIMEDOUT
                ? TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK
                : map_error(status);
        return TURBOWASM_OK;
    }

    if (!connection_equal(
            child->connection, child->connection)) {
        release_slot(impl, child);
        return TURBOWASM_TRAPPED;
    }

    child->connection_active = true;
    child->connection_connected = false;
    child->connection_terminal = false;
    child->connection_close_requested = false;
    child->connection_status = SALTS_OK;
    child->state = TW_CNET_SLOT_CONNECTING;

    /*
     * Accepted sockets are already native-connected. One owner-local CNet
     * advance publishes their CONNECTED state callback without observing
     * NativeIO or blocking.
     */
    status = cnet_client_advance_external(
        &impl->client, &events);
    if (status != SALTS_OK) {
        (void)cnet_close(
            &impl->client, child->connection);
        child->connection_close_requested = true;
        *out_error = map_error(status);
        return TURBOWASM_OK;
    }

    if (child->connection_terminal) {
        *out_error = map_error(
            child->connection_status != SALTS_OK
                ? child->connection_status
                : SALTS_ECONNABORTED);
        maybe_release_tombstone(impl, child);
        return TURBOWASM_OK;
    }

    if (!child->connection_connected ||
        child->state != TW_CNET_SLOT_CONNECTED) {
        (void)cnet_close(
            &impl->client, child->connection);
        child->connection_close_requested = true;
        *out_error =
            TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK;
        return TURBOWASM_OK;
    }

    stream_status = issue_connection_stream_reps(
        impl, child, out_input, out_output);
    if (stream_status != TURBOWASM_OK) {
        (void)cnet_close(
            &impl->client, child->connection);
        child->connection_close_requested = true;
        return stream_status;
    }

    child->guest_dropped = false;
    out_socket->kind = TURBOWASM_VALUE_I64;
    out_socket->as.i64 = (int64_t)pack_rep(
        child_index, child->generation);
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
        slot->state = TW_CNET_SLOT_LISTEN_READY;
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
    if (slot != NULL &&
        slot->state == TW_CNET_SLOT_LISTEN_READY) {
        slot->state = TW_CNET_SLOT_LISTENING;
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    } else {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
    }
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

    if (slot->connection_active) {
        status = cnet_connection_local_endpoint(
            &impl->client,
            slot->connection,
            &endpoint);
    } else {
        status = cnet_listener_local_endpoint(
            &slot->listener,
            &endpoint);
    }

    *out_error = map_error(status);
    if (status == SALTS_OK &&
        !endpoint_to_wasi(&endpoint, out_address))
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

static turbowasm_status provider_remote_address(
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

    if (slot == NULL || !slot->connection_active ||
        !slot->connection_connected ||
        slot->state != TW_CNET_SLOT_CONNECTED) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    status = cnet_connection_remote_endpoint(
        &impl->client,
        slot->connection,
        &endpoint);
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
    if (slot->connection_active) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
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
    if (slot->connection_active)
        return cnet_connection_tcp_option_get(
            &impl->client,
            slot->connection,
            option,
            out_value);
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
    if (slot->connection_active)
        return cnet_connection_tcp_option_set(
            &impl->client,
            slot->connection,
            option,
            value);
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

static turbowasm_status provider_shutdown(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_tcp_shutdown_type how,
    turbowasm_wasi02_socket_error *out_error) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    cnet_tcp_shutdown native_how;
    int status;

    if (out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;

    if (slot == NULL || !slot->connection_active ||
        !slot->connection_connected ||
        slot->state != TW_CNET_SLOT_CONNECTED) {
        *out_error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        return TURBOWASM_OK;
    }

    switch (how) {
        case TURBOWASM_WASI02_TCP_SHUTDOWN_RECEIVE:
            native_how = CNET_TCP_SHUTDOWN_RECEIVE;
            break;
        case TURBOWASM_WASI02_TCP_SHUTDOWN_SEND:
            native_how = CNET_TCP_SHUTDOWN_SEND;
            break;
        case TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH:
            native_how = CNET_TCP_SHUTDOWN_BOTH;
            break;
        default:
            *out_error =
                TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
            return TURBOWASM_OK;
    }

    status = cnet_connection_shutdown(
        &impl->client,
        slot->connection,
        native_how);
    *out_error = map_error(status);
    return TURBOWASM_OK;
}

static void provider_input_stream_drop(
    void *context,
    turbowasm_value rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_poll_rep(impl, rep);

    if (slot == NULL || slot->input_stream_leases == 0u)
        return;
    --slot->input_stream_leases;
    maybe_release_tombstone(impl, slot);
}

static void provider_output_stream_drop(
    void *context,
    turbowasm_value rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_poll_rep(impl, rep);

    if (slot == NULL || slot->output_stream_leases == 0u)
        return;
    --slot->output_stream_leases;
    maybe_release_tombstone(impl, slot);
}

static turbowasm_status cnet_init_common(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config,
    native_io_backend *external_backend,
    const cnet_client_config *client_config) {
    tw_cnet_impl *impl;
    bool external;
    uint32_t i;
    int status;

    external = external_backend != NULL || client_config != NULL;
    if (adapter == NULL || adapter->impl != NULL ||
        config == NULL || config->socket_capacity == 0u ||
        (external && (external_backend == NULL ||
                      client_config == NULL)))
        return TURBOWASM_INVALID_ARGUMENT;

    if (external &&
        (client_config->backend != config->backend ||
         client_config->connection_capacity <
             (size_t)config->socket_capacity))
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

    if (external) {
        status = cnet_client_init_external(
            &impl->client, client_config, external_backend);
        if (status != SALTS_OK) {
            turbowasm_status mapped = map_backend_status(status);
            free(impl->free_indices);
            free(impl->slots);
            free(impl);
            return mapped;
        }
        impl->external_backend = external_backend;
    }

    adapter->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_cnet_init(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config) {
    return cnet_init_common(adapter, config, NULL, NULL);
}

turbowasm_status turbowasm_wasi02_cnet_init_external(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config,
    native_io_backend *external_backend,
    const cnet_client_config *client_config) {
    return cnet_init_common(
        adapter, config, external_backend, client_config);
}

turbowasm_status turbowasm_wasi02_cnet_attach_poll_registry(
    turbowasm_wasi02_cnet *adapter,
    void *registry_context,
    turbowasm_wasi02_cnet_poll_register_fn register_fn) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (impl == NULL || registry_context == NULL ||
        register_fn == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->poll_register != NULL &&
        (impl->poll_registry_context != registry_context ||
         impl->poll_register != register_fn))
        return TURBOWASM_INVALID_ARGUMENT;

    impl->poll_registry_context = registry_context;
    impl->poll_register = register_fn;
    return TURBOWASM_OK;
}

static turbowasm_status socket_poll_ready_impl(
    tw_cnet_impl *impl,
    turbowasm_value socket_rep,
    bool *out_ready) {
    tw_cnet_slot *slot = slot_from_poll_rep(impl, socket_rep);

    if (out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_ready = false;
    if (slot == NULL)
        return TURBOWASM_TRAPPED;

    if (slot->guest_dropped ||
        slot->state == TW_CNET_SLOT_CLOSED) {
        *out_ready = true;
        return TURBOWASM_OK;
    }

    switch (slot->state) {
        case TW_CNET_SLOT_UNBOUND:
        case TW_CNET_SLOT_BOUND:
        case TW_CNET_SLOT_LISTEN_READY:
            *out_ready = true;
            return TURBOWASM_OK;
        case TW_CNET_SLOT_LISTENING:
            *out_ready = slot->accept_terminal;
            return TURBOWASM_OK;
        case TW_CNET_SLOT_CONNECTING:
            *out_ready =
                slot->connection_connected ||
                slot->connection_terminal;
            return TURBOWASM_OK;
        case TW_CNET_SLOT_CONNECTED:
            *out_ready = true;
            return TURBOWASM_OK;
        default:
            return TURBOWASM_TRAPPED;
    }
}

static turbowasm_status socket_poll_prepare_impl(
    tw_cnet_impl *impl,
    turbowasm_value socket_rep,
    bool *out_ready,
    native_io_request *out_request) {
    tw_cnet_slot *slot = slot_from_poll_rep(impl, socket_rep);
    int status;

    if (out_ready == NULL || out_request == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_ready = false;
    *out_request = (native_io_request){0};
    if (slot == NULL)
        return TURBOWASM_TRAPPED;

    status = socket_poll_ready_impl(
        impl, socket_rep, out_ready);
    if (status != TURBOWASM_OK || *out_ready)
        return status;

    if (slot->state == TW_CNET_SLOT_CONNECTING) {
        native_io_request request = {0};
        size_t events = 0u;
        size_t count = 0u;

        if (impl->external_backend == NULL ||
            impl->external_stopped ||
            !slot->connection_active)
            return TURBOWASM_UNSUPPORTED;

        /*
         * External advance owns command/deadline progress but never observes
         * NativeIO. It publishes the current connect request identity for W4.
         */
        status = cnet_client_advance_external(
            &impl->client, &events);
        if (status != SALTS_OK)
            return map_backend_status(status);

        status = socket_poll_ready_impl(
            impl, socket_rep, out_ready);
        if (status != TURBOWASM_OK || *out_ready)
            return status;

        status = cnet_client_external_requests(
            &impl->client,
            slot->connection,
            NULL,
            0u,
            &count);
        if (status == SALTS_OK && count == 0u)
            return TURBOWASM_TRAPPED;
        if (status != SALTS_ENOBUFS || count != 1u)
            return status == SALTS_ENOBUFS
                ? TURBOWASM_UNSUPPORTED
                : map_backend_status(status);

        status = cnet_client_external_requests(
            &impl->client,
            slot->connection,
            &request,
            1u,
            &count);
        if (status != SALTS_OK || count != 1u ||
            !native_io_request_valid(request))
            return status == SALTS_OK
                ? TURBOWASM_TRAPPED
                : map_backend_status(status);

        *out_request = request;
        return TURBOWASM_OK;
    }

    if (slot->state != TW_CNET_SLOT_LISTENING ||
        impl->external_backend == NULL)
        return TURBOWASM_UNSUPPORTED;

    /*
     * A request already owned by the listener remains a valid wait source even
     * if the cnet_client side was stopped after that request was submitted.
     */
    if (slot->accept_request_active) {
        *out_request = slot->accept_request;
        return TURBOWASM_OK;
    }

    if (impl->external_stopped)
        return TURBOWASM_UNSUPPORTED;

    if (!slot->external_attached) {
        status = cnet_listener_attach_external(
            &slot->listener, impl->external_backend);
        if (status != SALTS_OK && status != SALTS_EALREADY)
            return map_backend_status(status);
        slot->external_attached = true;
    }

    status = cnet_listener_submit_external_accept(
        &slot->listener, out_request);
    if (status == SALTS_EALREADY) {
        slot->accept_terminal = true;
        *out_ready = true;
        *out_request = (native_io_request){0};
        return TURBOWASM_OK;
    }
    if (status != SALTS_OK)
        return map_backend_status(status);
    if (!native_io_request_valid(*out_request))
        return TURBOWASM_TRAPPED;

    slot->accept_request = *out_request;
    slot->accept_request_active = true;
    slot->accept_terminal = false;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_cnet_socket_poll_ready(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_value socket_rep,
    bool *out_ready) {
    return socket_poll_ready_impl(
        impl_mut(adapter), socket_rep, out_ready);
}

turbowasm_status turbowasm_wasi02_cnet_socket_poll_prepare(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_value socket_rep,
    bool *out_ready,
    native_io_request *out_request) {
    return socket_poll_prepare_impl(
        impl_mut(adapter), socket_rep,
        out_ready, out_request);
}

static turbowasm_status dynamic_poll_ready(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready) {
    return socket_poll_ready_impl(
        (tw_cnet_impl *)context,
        source_rep,
        out_ready);
}

static turbowasm_status dynamic_poll_prepare(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready,
    native_io_request *out_request) {
    return socket_poll_prepare_impl(
        (tw_cnet_impl *)context,
        source_rep,
        out_ready,
        out_request);
}

static turbowasm_status dynamic_poll_drop(
    void *context,
    turbowasm_value source_rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot =
        slot_from_poll_rep(impl, source_rep);

    if (slot == NULL || slot->poll_leases == 0u)
        return TURBOWASM_TRAPPED;

    --slot->poll_leases;
    maybe_release_tombstone(impl, slot);
    return TURBOWASM_OK;
}

static turbowasm_status provider_subscribe(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_pollable_rep) {
    tw_cnet_impl *impl = (tw_cnet_impl *)context;
    tw_cnet_slot *slot = slot_from_rep(impl, socket_rep);
    turbowasm_status status;

    if (slot == NULL)
        return TURBOWASM_TRAPPED;
    if (out_pollable_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->poll_register == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (slot->poll_leases == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    ++slot->poll_leases;
    status = impl->poll_register(
        impl->poll_registry_context,
        impl,
        socket_rep,
        dynamic_poll_ready,
        dynamic_poll_prepare,
        dynamic_poll_drop,
        out_pollable_rep);
    if (status != TURBOWASM_OK) {
        --slot->poll_leases;
        maybe_release_tombstone(impl, slot);
    }
    return status;
}

int turbowasm_wasi02_cnet_advance_external(
    turbowasm_wasi02_cnet *adapter,
    size_t *out_events) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (impl == NULL || impl->external_backend == NULL ||
        out_events == NULL)
        return SALTS_EINVAL;
    if (impl->external_stopped)
        return SALTS_ESHUTDOWN;
    return cnet_client_advance_external(&impl->client, out_events);
}

int turbowasm_wasi02_cnet_route_external_completion(
    turbowasm_wasi02_cnet *adapter,
    const native_io_completion *completion,
    bool *out_consumed,
    size_t *out_events) {
    tw_cnet_impl *impl = impl_mut(adapter);
    uint32_t i;

    if (impl == NULL || impl->external_backend == NULL ||
        completion == NULL || out_consumed == NULL ||
        out_events == NULL)
        return SALTS_EINVAL;
    *out_consumed = false;
    *out_events = 0u;

    /*
     * Listener accept requests are owned by cnet_listener, not cnet_client.
     * Route those first so one runtime-observed completion has exactly one
     * authoritative CNet consumer.
     */
    for (i = 0u; i < impl->capacity; ++i) {
        tw_cnet_slot *slot = &impl->slots[i];
        bool consumed = false;
        int status;

        if (!slot->active || !slot->accept_request_active ||
            !request_equal(slot->accept_request,
                           completion->request))
            continue;

        status = cnet_listener_route_external_completion(
            &slot->listener, completion, &consumed);
        if (status != SALTS_OK)
            return status;
        if (!consumed)
            return SALTS_EPROTO;

        slot->accept_request_active = false;
        slot->accept_request = (native_io_request){0};
        slot->accept_terminal =
            completion->kind != NATIVE_IO_COMPLETION_CANCELLED &&
            completion->status != SALTS_ECANCELED;
        *out_consumed = true;

        if (slot->guest_dropped) {
            turbowasm_status finalize_status =
                finalize_listener_slot(impl, slot);
            if (finalize_status != TURBOWASM_OK)
                return SALTS_EPROTO;
        }
        return SALTS_OK;
    }

    /*
     * Stopping the external CNet client does not consume listener-owned accept
     * requests. Their terminal cancellation packets remain routable above.
     */
    if (impl->external_stopped)
        return SALTS_ESHUTDOWN;

    return cnet_client_route_external_completion(
        &impl->client, completion, out_consumed, out_events);
}

int turbowasm_wasi02_cnet_external_timeout(
    turbowasm_wasi02_cnet *adapter,
    uint32_t max_wait_ms,
    uint32_t *out_timeout_ms) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (impl == NULL || impl->external_backend == NULL ||
        out_timeout_ms == NULL)
        return SALTS_EINVAL;
    if (impl->external_stopped)
        return SALTS_ESHUTDOWN;
    return cnet_client_external_timeout(
        &impl->client, max_wait_ms, out_timeout_ms);
}

int turbowasm_wasi02_cnet_stop_external(
    turbowasm_wasi02_cnet *adapter) {
    tw_cnet_impl *impl = impl_mut(adapter);
    int status;

    if (impl == NULL || impl->external_backend == NULL)
        return SALTS_EINVAL;
    if (impl->external_stopped)
        return SALTS_OK;

    status = cnet_client_stop_external(&impl->client);
    if (status == SALTS_OK)
        impl->external_stopped = true;
    return status;
}

turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter) {
    tw_cnet_impl *impl = impl_mut(adapter);
    int status;

    if (adapter == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl == NULL)
        return TURBOWASM_OK;
    if (impl->free_count != impl->capacity ||
        impl->network_live != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    if (impl->external_backend != NULL) {
        status = turbowasm_wasi02_cnet_stop_external(adapter);
        if (status != SALTS_OK)
            return map_backend_status(status);
        status = cnet_client_destroy(&impl->client);
        if (status != SALTS_OK)
            return map_backend_status(status);
    }

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
    out_provider->tcp_start_connect = provider_start_connect;
    out_provider->tcp_finish_connect = provider_finish_connect;
    out_provider->tcp_start_listen = provider_start_listen;
    out_provider->tcp_finish_listen = provider_finish_listen;
    out_provider->tcp_accept = provider_accept;
    out_provider->tcp_local_address = provider_local_address;
    out_provider->tcp_remote_address = provider_remote_address;
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
    if (impl->poll_register != NULL)
        out_provider->tcp_subscribe = provider_subscribe;
    out_provider->tcp_shutdown = provider_shutdown;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_cnet_stream_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_stream_provider *out_provider) {
    tw_cnet_impl *impl = impl_mut(adapter);

    if (impl == NULL || out_provider == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_provider, 0, sizeof(*out_provider));
    out_provider->context = impl;
    out_provider->input_drop = provider_input_stream_drop;
    out_provider->output_drop = provider_output_stream_drop;
    return TURBOWASM_OK;
}
