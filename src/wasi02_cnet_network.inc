/* Private extension compiled in the CNet adapter translation unit so the
 * existing owner, admission guard and backend stay the sole authority. */
#include <cnet/name_lookup.h>
#include <limits.h>
#include "component_resource.h"

typedef struct tw_udp tw_udp;
typedef struct tw_udp_pair {
    bool active, live[2];
    uint64_t token[2];
    uint32_t refs;
    tw_udp *udp;
    turbowasm_wasi02_io_source source[2];
    struct { struct tw_udp_pair *pair; unsigned direction; } readiness[2];
    bool checked;
    uint64_t permit;
} tw_udp_pair;
typedef struct tw_udp_packet { size_t size; turbowasm_wasi02_ip_socket_address peer; } tw_udp_packet;
struct tw_udp {
    bool active, live, bound, binding, stopping, terminal, receive_pending, associated;
    uint64_t token;
    uint32_t refs;
    tw_cnet_impl *owner;
    cnet_datagram datagram;
    turbowasm_wasi02_ip_address_family family;
    turbowasm_wasi02_io_source source;
    tw_udp_pair *pair;
    uint8_t *bytes;
    tw_udp_packet *packets;
    size_t head, count, sends;
    int send_error, receive_error;
    turbowasm_wasi02_ip_socket_address remote;
};
typedef struct tw_dns {
    bool active, live, terminal;
    uint64_t token;
    uint32_t refs;
    turbowasm_wasi02_socket_error terminal_error;
    tw_cnet_impl *owner;
    cnet_name_query query;
    turbowasm_wasi02_io_source source;
} tw_dns;
typedef struct tw_cnet_network {
    turbowasm_wasi02_cnet_config_v2 config;
    tw_udp *udp;
    tw_udp_pair *pairs;
    tw_dns *dns;
    cnet_name_lookup lookup;
} tw_cnet_network;

static bool net_allowed(tw_cnet_impl *p, unsigned op, const turbowasm_wasi02_ip_socket_address *address) {
    const turbowasm_wasi02_cnet_config_v2 *c = &p->network->config;
    bool allow = op == TURBOWASM_WASI02_CNET_UDP_BIND ? c->allow_udp_bind :
        op == TURBOWASM_WASI02_CNET_UDP_SEND ? c->allow_udp_send :
        op == TURBOWASM_WASI02_CNET_UDP_RECEIVE ? c->allow_udp_receive : c->allow_name_lookup;
    if (allow && c->base.authorize) {
        bool busy = p->busy; p->busy = true;
        allow = c->base.authorize(c->base.policy_context, op, address); p->busy = busy;
    }
    return allow;
}
static turbowasm_value net_rep(uint64_t token) {
    turbowasm_value v = {0}; v.kind = TURBOWASM_VALUE_I64; v.as.i64 = (int64_t)token; return v;
}
static tw_udp *udp_get(tw_cnet_impl *p, turbowasm_value rep) {
    if (!p || !p->network || rep.kind != TURBOWASM_VALUE_I64) return NULL;
    for (uint32_t i = 0; i < p->network->config.udp_capacity; ++i) {
        tw_udp *u = &p->network->udp[i]; if (u->active && u->token == (uint64_t)rep.as.i64) return u;
    }
    return NULL;
}
static tw_udp_pair *pair_get(tw_cnet_impl *p, turbowasm_value rep, unsigned direction) {
    if (!p || !p->network || rep.kind != TURBOWASM_VALUE_I64) return NULL;
    for (uint32_t i = 0; i < p->network->config.pair_capacity; ++i) {
        tw_udp_pair *pair = &p->network->pairs[i];
        if (pair->active && pair->live[direction] && pair->token[direction] == (uint64_t)rep.as.i64) return pair;
    }
    return NULL;
}
static tw_dns *dns_get(tw_cnet_impl *p, turbowasm_value rep) {
    if (!p || !p->network || rep.kind != TURBOWASM_VALUE_I64) return NULL;
    for (uint32_t i = 0; i < p->network->config.lookup_capacity; ++i) {
        tw_dns *d = &p->network->dns[i]; if (d->active && d->live && d->token == (uint64_t)rep.as.i64) return d;
    }
    return NULL;
}
static void udp_changed(tw_udp *u) {
    if (u->source.token) (void)turbowasm_wasi02_io_source_changed(u->owner->io, u->source);
    if (u->pair) for (unsigned i = 0; i < 2; ++i) if (u->pair->source[i].token)
        (void)turbowasm_wasi02_io_source_changed(u->owner->io, u->pair->source[i]);
}
static void udp_source_retain(void *ctx) { ++((tw_udp *)ctx)->refs; }
static void udp_source_release(void *ctx) { --((tw_udp *)ctx)->refs; }
static turbowasm_status udp_ready(void *ctx, bool *ready) {
    tw_udp *u = ctx; bool quiet = false;
    if (!ready) return TURBOWASM_INVALID_ARGUMENT;
    int rc = u->terminal ? SALTS_OK : cnet_datagram_quiescent(&u->datagram, &quiet);
    *ready = u->binding || u->stopping || u->terminal || quiet;
    return rc == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}
static void pair_source_retain(void *ctx) {
    /* Both contexts have this common prefix. */
    tw_udp_pair *pair = *(tw_udp_pair **)ctx; ++pair->refs; ++pair->udp->refs;
}
static void pair_source_release(void *ctx) {
    tw_udp_pair *pair = *(tw_udp_pair **)ctx; --pair->refs; --pair->udp->refs;
}
static turbowasm_status pair_ready(void *ctx, bool *ready) {
    tw_udp_pair *pair = *(tw_udp_pair **)ctx;
    unsigned dir = ctx == &pair->readiness[0] ? 0 : 1; tw_udp *u = pair->udp;
    const turbowasm_wasi02_cnet_config_v2 *c = &u->owner->network->config;
    *ready = !pair->live[dir] || u->stopping || u->terminal ||
        (dir ? !c->allow_udp_send || u->send_error || u->sends < c->send_datagrams : !c->allow_udp_receive || u->receive_error || u->count);
    return TURBOWASM_OK;
}
static void udp_receive(void *ctx, cnet_datagram *d, const cnet_datagram_peer *peer, const cnet_receive_view *v) {
    tw_udp *u = ctx; const turbowasm_wasi02_cnet_config_v2 *c = &u->owner->network->config;
    cnet_stream_endpoint endpoint = CNET_STREAM_ENDPOINT_INIT;
    turbowasm_wasi02_ip_socket_address address; (void)peer;
    u->receive_pending = false;
    if (u->stopping || !u->pair || !u->pair->live[0]) return;
    if (cnet_datagram_received_endpoint(d, &endpoint) != SALTS_OK || !endpoint_to_wasi(&endpoint, &address)) {
        u->receive_error = SALTS_EPROTO; udp_changed(u); return;
    }
    /* Denied peers are discarded before publication; the next owner advance
     * can arm demand again. No borrowed callback payload survives return. */
    if (!net_allowed(u->owner, TURBOWASM_WASI02_CNET_UDP_RECEIVE, &address)) return;
    if (u->count >= c->receive_datagrams || v->size > c->datagram_bytes) {
        u->receive_error = SALTS_EMSGSIZE; udp_changed(u); return;
    }
    size_t index = (u->head + u->count) % c->receive_datagrams;
    u->packets[index].size = v->size; u->packets[index].peer = address;
    if (v->size) memcpy(u->bytes + index * c->datagram_bytes, v->data, v->size);
    ++u->count; udp_changed(u);
}
static void udp_sent(void *ctx, cnet_datagram *d, const cnet_datagram_peer *peer, size_t bytes, int rc, uint64_t tag) {
    tw_udp *u = ctx; (void)d; (void)peer; (void)bytes; (void)tag;
    if (u->sends) --u->sends;
    if (rc != SALTS_OK && !u->stopping && !u->send_error) u->send_error = rc;
    udp_changed(u);
}
static turbowasm_status net_udp_create(void *ctx, turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_cnet_network *n = p->network; tw_udp *u = NULL;
    if (p->busy || !n || !out || !error) return TURBOWASM_INVALID_ARGUMENT;
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (p->stopping) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    for (uint32_t i = 0; i < n->config.udp_capacity; ++i) if (!n->udp[i].active) { u = &n->udp[i]; break; }
    if (!u) { *error = TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT; return TURBOWASM_OK; }
    u->token = turbowasm_wasi02_io_token_private();
    if (!u->token) { *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    cnet_datagram_config c = CNET_DATAGRAM_CONFIG_INIT;
    c.backend = p->backend; c.send_capacity = n->config.send_datagrams;
    c.request_capacity = c.send_capacity + 1; c.completion_batch_capacity = c.request_capacity;
    c.max_datagram_bytes = c.receive_buffer_bytes = n->config.datagram_bytes;
    c.observer = (cnet_datagram_observer){udp_receive, udp_sent, u};
    int rc = cnet_datagram_open_external(&u->datagram, &c, p->native_backend, cnet_family(family));
    if (rc != SALTS_OK) { *error = map_error(rc); return TURBOWASM_OK; }
    u->active = u->live = true; u->refs = 1; u->owner = p; u->family = family;
    turbowasm_wasi02_io_source_ops ops = {u, udp_ready, udp_source_retain, udp_source_release};
    turbowasm_status status = turbowasm_wasi02_io_source_register(p->io, &ops, &u->source);
    if (status != TURBOWASM_OK) { u->live = false; --u->refs; u->stopping = true;
        *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    *out = net_rep(u->token); return TURBOWASM_OK;
}
static turbowasm_status net_udp_drop(void *ctx, turbowasm_value rep) {
    tw_cnet_impl *p = ctx; tw_udp *u = udp_get(p, rep);
    if (!u || !u->live || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    u->live = false; --u->refs;
    if (u->source.token) return turbowasm_wasi02_io_source_close(p->io, &u->source);
    return TURBOWASM_OK;
}
static turbowasm_status net_udp_bind(void *ctx, turbowasm_value rep, turbowasm_value network,
    const turbowasm_wasi02_ip_socket_address *address, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_udp *u = udp_get(p, rep); cnet_stream_endpoint ep = CNET_STREAM_ENDPOINT_INIT;
    if (!u || !u->live || p->busy || !error) return TURBOWASM_INVALID_ARGUMENT;
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (p->stopping || u->bound || u->binding) *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
    else if (!network_rep_valid(p, network) || !endpoint_from_wasi(address, &ep) || address->family != u->family)
        *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
    else if (!net_allowed(p, TURBOWASM_WASI02_CNET_UDP_BIND, address)) *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED;
    else { int rc = cnet_datagram_bind_endpoint(&u->datagram, &ep); *error = map_error(rc);
        if (rc == SALTS_OK) u->binding = true; }
    udp_changed(u); return TURBOWASM_OK;
}
static turbowasm_status net_udp_finish_bind(void *ctx, turbowasm_value rep, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx;
    tw_udp *u = udp_get(p, rep); if (!u || !u->live || p->busy || !error) return TURBOWASM_INVALID_ARGUMENT;
    *error = u->binding ? TURBOWASM_WASI02_SOCKET_ERROR_NONE : TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS;
    if (u->binding) { u->binding = false; u->bound = true; } return TURBOWASM_OK;
}
static bool udp_destination(const cnet_stream_endpoint *ep) {
    static const uint8_t zero[16] = {0};
    return ep->port && memcmp(ep->address,zero,ep->family == CNET_DATAGRAM_ADDRESS_IPV4 ? 4 : 16) != 0;
}
static turbowasm_status net_udp_stream(void *ctx, turbowasm_value rep,
    const turbowasm_wasi02_ip_socket_address *remote, turbowasm_value *in, turbowasm_value *out,
    turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_udp *u = udp_get(p, rep); tw_udp_pair *pair = NULL;
    cnet_stream_endpoint ep = CNET_STREAM_ENDPOINT_INIT; bool quiet = false;
    if (!u || !u->live || p->busy || !error) return TURBOWASM_INVALID_ARGUMENT;
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (!u->bound || p->stopping) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    if (u->pair && (u->pair->live[0] || u->pair->live[1])) return TURBOWASM_TRAPPED;
    if (remote && (!endpoint_from_wasi(remote, &ep) || remote->family != u->family || !udp_destination(&ep))) {
        *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT; return TURBOWASM_OK; }
    if (remote && !net_allowed(p, TURBOWASM_WASI02_CNET_UDP_SEND, remote)) {
        *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK; }
    int rc = cnet_datagram_pause_receive(&u->datagram);
    if (rc == SALTS_OK) rc = cnet_datagram_quiescent(&u->datagram, &quiet);
    if (rc != SALTS_OK || !quiet) { *error = rc != SALTS_OK ? map_error(rc) : TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT; return TURBOWASM_OK; }
    for (uint32_t i = 0; i < p->network->config.pair_capacity; ++i) if (!p->network->pairs[i].active) {
        pair = &p->network->pairs[i]; break; }
    if (!pair) { *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    memset(pair, 0, sizeof(*pair)); pair->active = true; pair->udp = u;
    turbowasm_status status = TURBOWASM_OK;
    for (unsigned i = 0; i < 2; ++i) {
        pair->token[i] = turbowasm_wasi02_io_token_private(); pair->readiness[i].pair = pair; pair->readiness[i].direction = i;
        turbowasm_wasi02_io_source_ops ops = {&pair->readiness[i], pair_ready, pair_source_retain, pair_source_release};
        status = pair->token[i] ? turbowasm_wasi02_io_source_register(p->io, &ops, &pair->source[i]) : TURBOWASM_OUT_OF_MEMORY;
        if (status != TURBOWASM_OK) break;
    }
    if (status == TURBOWASM_OK && (remote || u->associated)) rc = cnet_datagram_associate_endpoint(&u->datagram, remote ? &ep : NULL);
    if (status != TURBOWASM_OK || rc != SALTS_OK) {
        for (unsigned i = 0; i < 2; ++i) if (pair->source[i].token) (void)turbowasm_wasi02_io_source_close(p->io, &pair->source[i]);
        pair->active = false; *error = status != TURBOWASM_OK ? TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY : map_error(rc);
        return TURBOWASM_OK;
    }
    pair->live[0] = pair->live[1] = true; pair->refs += 2; u->refs += 2;
    u->pair = pair; u->associated = remote != NULL; if (remote) u->remote = *remote;
    u->head = u->count = 0; u->receive_error = u->send_error = SALTS_OK; u->receive_pending = false;
    *in = net_rep(pair->token[0]); *out = net_rep(pair->token[1]); return TURBOWASM_OK;
}
static turbowasm_status net_udp_address(void *ctx, turbowasm_value rep,
    turbowasm_wasi02_ip_socket_address *out, turbowasm_wasi02_socket_error *error, bool remote) {
    tw_cnet_impl *p = ctx; tw_udp *u = udp_get(p, rep); cnet_stream_endpoint ep = CNET_STREAM_ENDPOINT_INIT;
    if (!u || !u->live || p->busy || !out || !error) return TURBOWASM_INVALID_ARGUMENT;
    if (remote && !u->associated) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    int rc = remote ? cnet_datagram_remote_endpoint(&u->datagram, &ep) : cnet_datagram_local_endpoint(&u->datagram, &ep);
    *error = rc == SALTS_ENOTCONN ? TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE : map_error(rc);
    if (rc == SALTS_OK && !endpoint_to_wasi(&ep, out)) return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}
static turbowasm_status net_udp_local(void *c, turbowasm_value r, turbowasm_wasi02_ip_socket_address *a, turbowasm_wasi02_socket_error *e) { return net_udp_address(c,r,a,e,false); }
static turbowasm_status net_udp_remote(void *c, turbowasm_value r, turbowasm_wasi02_ip_socket_address *a, turbowasm_wasi02_socket_error *e) { return net_udp_address(c,r,a,e,true); }
static cnet_tcp_socket_option udp_option(turbowasm_wasi02_udp_option option) {
    return option == TURBOWASM_WASI02_UDP_HOP_LIMIT ? CNET_TCP_SOCKET_HOP_LIMIT :
        option == TURBOWASM_WASI02_UDP_RECEIVE_BUFFER ? CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES : CNET_TCP_SOCKET_SEND_BUFFER_BYTES;
}
static turbowasm_status net_udp_get_option(void *c, turbowasm_value r, turbowasm_wasi02_udp_option o, uint64_t *v, turbowasm_wasi02_socket_error *e) {
    tw_cnet_impl *p = c; tw_udp *u = udp_get(p,r);
    if (!u || !u->live || p->busy || !v || !e || (unsigned)o > TURBOWASM_WASI02_UDP_SEND_BUFFER) return TURBOWASM_INVALID_ARGUMENT;
    *e = map_error(cnet_datagram_option_get(&u->datagram, udp_option(o), v)); return TURBOWASM_OK;
}
static turbowasm_status net_udp_set_option(void *c, turbowasm_value r, turbowasm_wasi02_udp_option o, uint64_t v, turbowasm_wasi02_socket_error *e) {
    tw_cnet_impl *p = c; tw_udp *u = udp_get(p,r);
    if (!u || !u->live || p->busy || !e || (unsigned)o > TURBOWASM_WASI02_UDP_SEND_BUFFER) return TURBOWASM_INVALID_ARGUMENT;
    bool buffer = o != TURBOWASM_WASI02_UDP_HOP_LIMIT;
    if (buffer && v > INT_MAX) v = INT_MAX;
    int rc = cnet_datagram_option_set(&u->datagram, udp_option(o), v);
    /* WIT allows buffer rounding/clamping. BSD reports ENOBUFS for a size over
     * its socket limit; halve that request until it succeeds, at most 31 times.
     * Other errors and an exhausted minimum remain observable. */
    while (buffer && rc == SALTS_ENOBUFS && v > 1) {
        v /= 2;
        rc = cnet_datagram_option_set(&u->datagram, udp_option(o), v);
    }
    *e = map_error(rc); return TURBOWASM_OK;
}
static turbowasm_status net_udp_subscribe(void *ctx, turbowasm_value rep, turbowasm_value *out) {
    tw_cnet_impl *p = ctx; tw_udp *u = udp_get(p, rep);
    if (!u || !u->live || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_wasi02_io_pollable_register(p->io, u->source, out);
}
static turbowasm_status net_pair_subscribe(void *ctx, turbowasm_value rep, turbowasm_value *out, unsigned dir) {
    tw_cnet_impl *p = ctx; tw_udp_pair *pair = pair_get(p, rep, dir);
    if (!pair || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_wasi02_io_pollable_register(p->io, pair->source[dir], out);
}
static turbowasm_status net_in_subscribe(void *c, turbowasm_value r, turbowasm_value *o) { return net_pair_subscribe(c,r,o,0); }
static turbowasm_status net_out_subscribe(void *c, turbowasm_value r, turbowasm_value *o) { return net_pair_subscribe(c,r,o,1); }
static turbowasm_status net_pair_drop(void *ctx, turbowasm_value rep, unsigned dir) {
    tw_cnet_impl *p = ctx; tw_udp_pair *pair = pair_get(p, rep, dir);
    if (!pair || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    pair->live[dir] = false; --pair->refs; --pair->udp->refs;
    int pause_status = SALTS_OK;
    if (!dir) {
        pair->udp->count = 0;
        pause_status = cnet_datagram_pause_receive(&pair->udp->datagram);
        if (pause_status != SALTS_OK) pair->udp->receive_error = pause_status;
    }
    turbowasm_status status = turbowasm_wasi02_io_source_close(p->io, &pair->source[dir]);
    return status != TURBOWASM_OK ? status : pause_status == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}
static turbowasm_status net_in_drop(void *c, turbowasm_value r) { return net_pair_drop(c,r,0); }
static turbowasm_status net_out_drop(void *c, turbowasm_value r) { return net_pair_drop(c,r,1); }
static turbowasm_status net_receive(void *ctx, turbowasm_value rep, turbowasm_wasi02_incoming_datagram *records,
    size_t capacity, size_t *count, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_udp_pair *pair = pair_get(p, rep, 0);
    if (!pair || p->busy || !count || !error || (capacity && !records)) return TURBOWASM_INVALID_ARGUMENT;
    tw_udp *u = pair->udp; const turbowasm_wasi02_cnet_config_v2 *c = &p->network->config;
    *count = 0; *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (!capacity) return TURBOWASM_OK;
    if (u->stopping || u->terminal) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    if (!c->allow_udp_receive) { *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK; }
    if (u->receive_error) { *error = map_error(u->receive_error); u->receive_error = 0; return TURBOWASM_OK; }
    size_t n = capacity < u->count ? capacity : u->count;
    for (size_t i = 0; i < n; ++i) {
        size_t index = (u->head + i) % c->receive_datagrams;
        if (records[i].capacity < u->packets[index].size || (u->packets[index].size && !records[i].data))
            return TURBOWASM_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < n; ++i) {
        size_t index = (u->head + i) % c->receive_datagrams;
        records[i].size = u->packets[index].size; records[i].remote_address = u->packets[index].peer;
        if (records[i].size) memcpy(records[i].data, u->bytes + index * c->datagram_bytes, records[i].size);
    }
    u->head = (u->head + n) % c->receive_datagrams; u->count -= n; *count = n; udp_changed(u); return TURBOWASM_OK;
}
static turbowasm_status net_check_send(void *ctx, turbowasm_value rep, uint64_t *permit, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_udp_pair *pair = pair_get(p, rep, 1);
    if (!pair || p->busy || !permit || !error) return TURBOWASM_INVALID_ARGUMENT;
    tw_udp *u = pair->udp; *permit = 0; pair->checked = false; pair->permit = 0;
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (u->terminal || u->stopping) *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
    else if (!p->network->config.allow_udp_send) *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED;
    else if (u->send_error) { *error = map_error(u->send_error); u->send_error = 0; }
    else { *permit = p->network->config.send_datagrams - u->sends; pair->checked = true; pair->permit = *permit; }
    udp_changed(u); return TURBOWASM_OK;
}
static bool same_address(const turbowasm_wasi02_ip_socket_address *a, const turbowasm_wasi02_ip_socket_address *b) {
    cnet_stream_endpoint x = CNET_STREAM_ENDPOINT_INIT, y = CNET_STREAM_ENDPOINT_INIT;
    return endpoint_from_wasi(a,&x) && endpoint_from_wasi(b,&y) && x.family == y.family && x.port == y.port &&
        x.scope_id == y.scope_id && x.flow_info == y.flow_info && !memcmp(x.address,y.address,x.family == CNET_DATAGRAM_ADDRESS_IPV4 ? 4 : 16);
}
static turbowasm_status net_send(void *ctx, turbowasm_value rep, const turbowasm_wasi02_outgoing_datagram *records,
    size_t count, size_t *sent, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_udp_pair *pair = pair_get(p,rep,1);
    if (!pair || p->busy || !sent || !error || (count && !records)) return TURBOWASM_INVALID_ARGUMENT;
    *sent = 0; *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (!pair->checked || count > pair->permit) return TURBOWASM_TRAPPED;
    pair->checked = false; pair->permit = 0; tw_udp *u = pair->udp;
    if (u->stopping || u->terminal) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    for (size_t i = 0; i < count; ++i) {
        const turbowasm_wasi02_outgoing_datagram *r = &records[i];
        const turbowasm_wasi02_ip_socket_address *address = r->has_remote_address ? &r->remote_address : u->associated ? &u->remote : NULL;
        cnet_stream_endpoint ep = CNET_STREAM_ENDPOINT_INIT; int rc = SALTS_OK;
        if (!address || !endpoint_from_wasi(address,&ep) || address->family != u->family || !udp_destination(&ep) ||
            (u->associated && !same_address(address,&u->remote)) || (r->size && !r->data)) rc = SALTS_EINVAL;
        else if (r->size > p->network->config.datagram_bytes) rc = SALTS_EMSGSIZE;
        else if (!net_allowed(p,TURBOWASM_WASI02_CNET_UDP_SEND,address)) rc = SALTS_EPERM;
        else rc = cnet_datagram_send_endpoint(&u->datagram,u->associated ? NULL : &ep,r->data,r->size,0);
        if (rc != SALTS_OK) {
            if (rc == SALTS_ENOBUFS || rc == SALTS_EBUSY) break;
            if (*sent) u->send_error = rc; else *error = map_error(rc);
            break;
        }
        ++u->sends; ++*sent;
    }
    udp_changed(u); return TURBOWASM_OK;
}
static void dns_retain(void *ctx) { ++((tw_dns *)ctx)->refs; }
static void dns_release(void *ctx) { --((tw_dns *)ctx)->refs; }
static turbowasm_status dns_ready(void *ctx, bool *ready) {
    tw_dns *d = ctx;
    if (!d->live || d->terminal || d->owner->stopping) { *ready = true; return TURBOWASM_OK; }
    return cnet_name_lookup_ready(&d->owner->network->lookup,d->query,ready) == SALTS_OK ? TURBOWASM_OK : TURBOWASM_TRAPPED;
}
static turbowasm_status net_resolve(void *ctx, turbowasm_value network, turbowasm_wasi02_string_view name,
    turbowasm_value *out, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_cnet_network *n = p->network; tw_dns *d = NULL; char ascii[256]; size_t size;
    if (!n || p->busy || !out || !error) return TURBOWASM_INVALID_ARGUMENT;
    *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (p->stopping || !network_rep_valid(p,network)) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    int rc = cnet_name_lookup_normalize(&n->lookup,(const char *)name.data,name.size,ascii,sizeof(ascii),&size,NULL,NULL);
    if (rc != SALTS_OK) { *error = map_error(rc); return TURBOWASM_OK; }
    bool allow = n->config.allow_name_lookup;
    if (allow && n->config.authorize_name) { p->busy = true;
        allow = n->config.authorize_name(n->config.base.policy_context,(turbowasm_wasi02_string_view){(const uint8_t *)ascii,size}); p->busy = false; }
    if (!allow) { *error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; return TURBOWASM_OK; }
    for (uint32_t i = 0; i < n->config.lookup_capacity; ++i) if (!n->dns[i].active) { d = &n->dns[i]; break; }
    if (!d) { *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    memset(d,0,sizeof(*d)); d->owner = p; d->token = turbowasm_wasi02_io_token_private();
    if (!d->token) { *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    turbowasm_wasi02_io_source_ops ops = {d,dns_ready,dns_retain,dns_release};
    if (turbowasm_wasi02_io_source_register(p->io,&ops,&d->source) != TURBOWASM_OK) {
        *error = TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY; return TURBOWASM_OK; }
    rc = cnet_name_lookup_submit(&n->lookup,ascii,size,&d->query);
    if (rc != SALTS_OK) { (void)turbowasm_wasi02_io_source_close(p->io,&d->source); *error = map_error(rc); return TURBOWASM_OK; }
    d->active = d->live = true; ++d->refs; *out = net_rep(d->token); return TURBOWASM_OK;
}
static turbowasm_status net_resolve_next(void *ctx, turbowasm_value rep, bool *has,
    turbowasm_wasi02_ip_address *out, turbowasm_wasi02_socket_error *error) {
    tw_cnet_impl *p = ctx; tw_dns *d = dns_get(p,rep); cnet_ip_address a;
    if (!d || p->busy || !has || !out || !error) return TURBOWASM_INVALID_ARGUMENT;
    *has = false; *error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    if (p->stopping) { *error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE; return TURBOWASM_OK; }
    if (d->terminal_error) { *error = d->terminal_error; return TURBOWASM_OK; }
    int rc = cnet_name_lookup_next(&p->network->lookup,d->query,&a);
    if (rc == SALTS_ETIMEDOUT) { *error = TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK; return TURBOWASM_OK; }
    if (rc == SALTS_EOF) { d->terminal = true; return TURBOWASM_OK; }
    if (rc != SALTS_OK) { *error = d->terminal_error = map_error(rc); d->terminal = true; return TURBOWASM_OK; }
    cnet_stream_endpoint ep = CNET_STREAM_ENDPOINT_INIT; ep.family = a.family; memcpy(ep.address,a.address,16);
    turbowasm_wasi02_ip_socket_address address;
    if (!endpoint_to_wasi(&ep,&address)) return TURBOWASM_TRAPPED;
    if (!net_allowed(p,TURBOWASM_WASI02_CNET_RESOLVE_RESULT,&address)) {
        *error = d->terminal_error = TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED; d->terminal = true; return TURBOWASM_OK; }
    out->family = address.family;
    if (out->family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) memcpy(out->as.ipv4,address.as.ipv4.address,4);
    else memcpy(out->as.ipv6,address.as.ipv6.address,16);
    *has = true; return TURBOWASM_OK;
}
static turbowasm_status net_resolve_subscribe(void *ctx, turbowasm_value rep, turbowasm_value *out) {
    tw_cnet_impl *p = ctx; tw_dns *d = dns_get(p,rep); if (!d || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_wasi02_io_pollable_register(p->io,d->source,out);
}
static turbowasm_status net_resolve_drop(void *ctx, turbowasm_value rep) {
    tw_cnet_impl *p = ctx; tw_dns *d = dns_get(p,rep); if (!d || p->busy) return TURBOWASM_INVALID_ARGUMENT;
    int rc = cnet_name_lookup_query_drop(&p->network->lookup,&d->query);
    if (rc != SALTS_OK) return TURBOWASM_TRAPPED;
    d->live = false; --d->refs; return turbowasm_wasi02_io_source_close(p->io,&d->source);
}

static bool cnet_network_active(tw_cnet_impl *p) {
    if (!p->network) return false;
    for (uint32_t i = 0; p->network->udp && i < p->network->config.udp_capacity; ++i) if (p->network->udp[i].active) return true;
    for (uint32_t i = 0; p->network->dns && i < p->network->config.lookup_capacity; ++i) if (p->network->dns[i].active) return true;
    return false;
}
static int cnet_network_advance(tw_cnet_impl *p) {
    tw_cnet_network *n = p->network; if (!n) return SALTS_OK;
    for (uint32_t i = 0; i < n->config.pair_capacity; ++i) {
        tw_udp_pair *pair = &n->pairs[i]; if (pair->active && !pair->refs) {
            if (pair->udp->pair == pair) pair->udp->pair = NULL; pair->active = false;
        }
    }
    for (uint32_t i = 0; i < n->config.udp_capacity; ++i) {
        tw_udp *u = &n->udp[i]; if (!u->active) continue;
        if (p->stopping || (!u->live && (!u->pair || (!u->pair->live[0] && !u->pair->live[1])))) u->stopping = true;
        int rc = SALTS_OK; size_t events;
        if (u->stopping && !u->terminal) {
            bool stopped = false; rc = cnet_datagram_stop_external(&u->datagram,&stopped);
            if (rc == SALTS_EBUSY) rc = SALTS_OK;
            if (stopped) { u->terminal = true; u->receive_pending = false; }
        } else if (!u->terminal) {
            rc = cnet_datagram_advance_external(&u->datagram,&events);
            if (rc == SALTS_OK && u->bound && u->pair && u->pair->live[0] && n->config.allow_udp_receive &&
                !u->receive_error && !u->receive_pending && u->count < n->config.receive_datagrams) {
                rc = cnet_datagram_receive(&u->datagram,1);
                if (rc == SALTS_OK) u->receive_pending = true;
                else if (rc == SALTS_EBUSY || rc == SALTS_ENOBUFS) rc = SALTS_OK;
            }
        }
        if (rc != SALTS_OK) return rc;
        udp_changed(u);
        if (u->terminal && !u->refs) {
            rc = cnet_datagram_destroy(&u->datagram); if (rc != SALTS_OK) return rc;
            uint8_t *bytes = u->bytes; tw_udp_packet *packets = u->packets;
            memset(u,0,sizeof(*u)); u->bytes = bytes; u->packets = packets;
        }
    }
    if (n->lookup.impl) {
        int rc = cnet_name_lookup_advance(&n->lookup); if (rc != SALTS_OK) return rc;
        for (uint32_t i = 0; i < n->config.lookup_capacity; ++i) {
            tw_dns *d = &n->dns[i]; if (!d->active) continue;
            if (d->source.token) (void)turbowasm_wasi02_io_source_changed(p->io,d->source);
            if (!d->refs) memset(d,0,sizeof(*d));
        }
    }
    return SALTS_OK;
}
static int cnet_network_route(tw_cnet_impl *p, const native_io_completion *completion, bool *consumed) {
    if (!p->network) return SALTS_OK;
    for (uint32_t i = 0; i < p->network->config.udp_capacity; ++i) {
        tw_udp *u = &p->network->udp[i]; if (!u->active || u->terminal) continue;
        size_t events; int rc = cnet_datagram_route_external_completion(&u->datagram,completion,consumed,&events);
        if (*consumed || rc != SALTS_OK) {
            /* A cancelled receive has no user callback. Request identity stays
             * retained in CNet until routing this actual terminal. */
            if (completion->user_data == 0) {
                u->receive_pending = false;
                if (rc != SALTS_OK && rc != SALTS_EPROTO && !u->stopping) { u->receive_error = rc; rc = SALTS_OK; }
            }
            udp_changed(u); return rc;
        }
    }
    return SALTS_OK;
}
static int cnet_network_destroy(tw_cnet_impl *p) {
    tw_cnet_network *n = p->network; if (!n) return SALTS_OK;
    if (cnet_network_active(p)) return SALTS_EBUSY;
    if (n->lookup.impl) { int rc = cnet_name_lookup_close(&n->lookup); if (rc != SALTS_OK) return rc;
        rc = cnet_name_lookup_destroy(&n->lookup); if (rc != SALTS_OK) return rc; }
    for (uint32_t i = 0; i < n->config.udp_capacity; ++i) { turbowasm_rt_free(n->udp[i].bytes); turbowasm_rt_free(n->udp[i].packets); }
    turbowasm_rt_free(n->udp); turbowasm_rt_free(n->pairs); turbowasm_rt_free(n->dns); turbowasm_rt_free(n);
    p->network = NULL; return SALTS_OK;
}
void turbowasm_wasi02_cnet_config_v2_init(turbowasm_wasi02_cnet_config_v2 *c) {
    if (!c) return; memset(c,0,sizeof(*c)); c->size = sizeof(*c); c->api_version = 2;
    turbowasm_wasi02_cnet_config_init(&c->base);
    c->udp_capacity = 16; c->pair_capacity = 32; c->lookup_capacity = 16;
    c->datagram_bytes = 65507; c->receive_datagrams = c->send_datagrams = 4;
    c->lookup_results = 64; c->max_name_bytes = 4096; c->lookup_timeout_ms = 30000;
}
turbowasm_status turbowasm_wasi02_cnet_init_external_v2(turbowasm_wasi02_cnet *a, turbowasm_wasi02_io *io,
    native_io_backend *backend, const turbowasm_wasi02_cnet_config_v2 *c, const turbowasm_runtime_config *runtime) {
    native_io_backend_config bc;
    if (!c || c->size != sizeof(*c) || c->api_version != 2 || !native_io_backend_get_config(backend,&bc) ||
        c->base.socket_capacity > UINT32_MAX / 8u ||
        c->receive_datagrams > UINT32_MAX || c->send_datagrams > UINT32_MAX ||
        !c->udp_capacity || !c->pair_capacity || !c->lookup_capacity || !c->datagram_bytes || c->datagram_bytes > 65507 ||
        !c->receive_datagrams || !c->send_datagrams || c->send_datagrams >= SIZE_MAX ||
        c->udp_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS || c->pair_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        c->lookup_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        c->receive_datagrams > SIZE_MAX / c->datagram_bytes || c->send_datagrams > SIZE_MAX / c->datagram_bytes ||
        (size_t)c->base.socket_capacity * 2 > SIZE_MAX - c->udp_capacity ||
        bc.endpoint_capacity < (size_t)c->base.socket_capacity * 2 + c->udp_capacity ||
        c->udp_capacity > (SIZE_MAX - (size_t)c->base.socket_capacity * 3) / (c->send_datagrams + 1) ||
        bc.request_capacity < (size_t)c->base.socket_capacity * 3 + c->udp_capacity * (c->send_datagrams + 1))
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = turbowasm_wasi02_cnet_init_external(a,io,backend,&c->base,runtime);
    if (status != TURBOWASM_OK) return status;
    tw_cnet_impl *p = impl_mut(a); turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&p->runtime);
    tw_cnet_network *n = turbowasm_rt_calloc(1,sizeof(*n));
    if (n) {
        n->config = *c; p->network = n;
        n->udp = turbowasm_rt_calloc(c->udp_capacity,sizeof(*n->udp));
        n->pairs = turbowasm_rt_calloc(c->pair_capacity,sizeof(*n->pairs));
        n->dns = turbowasm_rt_calloc(c->lookup_capacity,sizeof(*n->dns));
        if (!n->udp || !n->pairs || !n->dns) status = TURBOWASM_OUT_OF_MEMORY;
        else for (uint32_t i = 0; i < c->udp_capacity; ++i) {
            n->udp[i].bytes = turbowasm_rt_calloc(c->receive_datagrams,c->datagram_bytes);
            n->udp[i].packets = turbowasm_rt_calloc(c->receive_datagrams,sizeof(tw_udp_packet));
            if (!n->udp[i].bytes || !n->udp[i].packets) { status = TURBOWASM_OUT_OF_MEMORY; break; }
        }
        if (status == TURBOWASM_OK) {
            cnet_name_lookup_config lc; cnet_name_lookup_config_init(&lc);
            lc.query_capacity = c->lookup_capacity; lc.results_per_query = c->lookup_results;
            lc.max_name_bytes = c->max_name_bytes; lc.timeout_ms = c->lookup_timeout_ms; lc.servers_csv = c->servers_csv;
            int rc = cnet_name_lookup_init(&n->lookup,&lc);
            if (rc != SALTS_OK) status = rc == SALTS_ENOMEM ? TURBOWASM_OUT_OF_MEMORY : TURBOWASM_INVALID_ARGUMENT;
        }
    } else status = TURBOWASM_OUT_OF_MEMORY;
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK) {
        if (n && !n->udp) { n->config.udp_capacity = 0; }
        (void)cnet_network_destroy(p); (void)cnet_client_stop_external(&p->client); p->stopped = true;
        (void)turbowasm_wasi02_cnet_destroy(a);
    }
    return status;
}
turbowasm_status turbowasm_wasi02_cnet_network_provider(turbowasm_wasi02_cnet *a, turbowasm_wasi02_network_provider *v) {
    tw_cnet_impl *p = impl_mut(a); if (!p || !p->network || p->busy || !v) return TURBOWASM_INVALID_ARGUMENT;
    memset(v,0,sizeof(*v)); v->size = sizeof(*v); v->api_version = 1; v->context = p;
    v->max_datagram_bytes = p->network->config.datagram_bytes;
    v->max_datagram_batch = p->network->config.send_datagrams > p->network->config.receive_datagrams ?
        p->network->config.send_datagrams : p->network->config.receive_datagrams;
    v->udp_create = net_udp_create; v->udp_drop = net_udp_drop;
    v->udp_start_bind = net_udp_bind; v->udp_finish_bind = net_udp_finish_bind; v->udp_stream = net_udp_stream;
    v->udp_local_address = net_udp_local; v->udp_remote_address = net_udp_remote;
    v->udp_option_get = net_udp_get_option; v->udp_option_set = net_udp_set_option; v->udp_subscribe = net_udp_subscribe;
    v->incoming_receive = net_receive; v->incoming_subscribe = net_in_subscribe; v->incoming_drop = net_in_drop;
    v->outgoing_check_send = net_check_send; v->outgoing_send = net_send; v->outgoing_subscribe = net_out_subscribe; v->outgoing_drop = net_out_drop;
    v->resolve_addresses = net_resolve; v->resolve_next_address = net_resolve_next;
    v->resolve_subscribe = net_resolve_subscribe; v->resolve_drop = net_resolve_drop;
    return TURBOWASM_OK;
}
extern turbowasm_status turbowasm_wasi02_set_network_private(turbowasm_wasi02 *, const turbowasm_wasi02_config_v2 *);
turbowasm_status turbowasm_wasi02_cnet_wasi02_init_v2(turbowasm_wasi02_cnet *a, turbowasm_wasi02 *w,
    const turbowasm_wasi02_config_v2 *c, const turbowasm_runtime_config *runtime) {
    if (!c || c->size != sizeof(*c) || c->api_version != 2) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi02_config_v2 copy = *c;
    turbowasm_status status = turbowasm_wasi02_cnet_network_provider(a,&copy.network);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_wasi02_cnet_wasi02_init(a,w,&copy.base,runtime);
    if (status == TURBOWASM_OK) {
        status = turbowasm_wasi02_set_network_private(w,&copy);
        if (status != TURBOWASM_OK) (void)turbowasm_wasi02_destroy(w);
    }
    return status;
}
