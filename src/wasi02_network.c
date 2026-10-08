#include "wasi02_network.h"
#include "wasi02_sockets.h"
#include "runtime_alloc.h"
#include "wasi02_descriptor.h"
#include <string.h>

static const char *const resource_names[] = {
    "udp-socket", "incoming-datagram-stream", "outgoing-datagram-stream", "resolve-address-stream"
};
uint64_t tw_network_resource_id(unsigned kind) { return UINT64_C(0x77617369326e7800) + kind; }
int tw_network_resource_kind(const char *interface_name, const char *resource_name) {
    if (!interface_name || !resource_name) return -1;
    for (unsigned i = 0; i < TW_NETWORK_KINDS; ++i)
        if (!strcmp(interface_name, i == TW_NETWORK_RESOLVE ? "ip-name-lookup" : "udp") &&
            !strcmp(resource_name, resource_names[i])) return (int)i;
    return -1;
}
static tw_network_slot *slot_get(turbowasm_wasi02_network *p, unsigned kind, uint32_t handle) {
    turbowasm_value rep;
    if (!p || kind >= TW_NETWORK_KINDS ||
        turbowasm_component_resource_rep(&p->tables[kind], handle, tw_network_resource_id(kind), &rep) != TURBOWASM_OK ||
        rep.kind != TURBOWASM_VALUE_I32 || rep.as.i32 < 0 || (uint32_t)rep.as.i32 >= p->capacities[kind]) return NULL;
    tw_network_slot *s = &p->slots[kind][rep.as.i32]; return s->active ? s : NULL;
}
turbowasm_status tw_network_rep(turbowasm_wasi02_network *p, unsigned kind, uint32_t handle, turbowasm_value *out) {
    tw_network_slot *s = slot_get(p, kind, handle);
    if (!s || !out) return TURBOWASM_TRAPPED;
    *out = s->provider_rep; return TURBOWASM_OK;
}
static turbowasm_status provider_drop(turbowasm_wasi02_network *p, unsigned kind, turbowasm_value rep) {
    switch (kind) {
        case TW_NETWORK_UDP: return p->provider.udp_drop(p->provider.context, rep);
        case TW_NETWORK_INCOMING: return p->provider.incoming_drop(p->provider.context, rep);
        case TW_NETWORK_OUTGOING: return p->provider.outgoing_drop(p->provider.context, rep);
        default: return p->provider.resolve_drop(p->provider.context, rep);
    }
}
turbowasm_status tw_network_drop(turbowasm_wasi02_network *p, unsigned kind, uint32_t handle) {
    tw_network_slot *s = slot_get(p, kind, handle); turbowasm_value rep; turbowasm_status status;
    if (!s) return TURBOWASM_TRAPPED;
    status = turbowasm_component_resource_take_owned(&p->tables[kind], handle, tw_network_resource_id(kind), &rep);
    if (status != TURBOWASM_OK) return status;
    rep = s->provider_rep; memset(s, 0, sizeof(*s)); return provider_drop(p, kind, rep);
}
turbowasm_status tw_network_destroy(turbowasm_wasi02_sockets *s) {
    turbowasm_wasi02_network *p = s->network; turbowasm_status status;
    if (!p) return TURBOWASM_OK;
    /* Preserve both direction owners until their native terminal; drop child
     * facades first so provider shutdown can cancel demand before parent drop. */
    const unsigned order[] = {TW_NETWORK_INCOMING, TW_NETWORK_OUTGOING, TW_NETWORK_RESOLVE, TW_NETWORK_UDP};
    for (unsigned j = 0; j < TW_NETWORK_KINDS; ++j) {
        unsigned kind = order[j];
        for (uint32_t i = 0; i < p->tables[kind].capacity; ++i) {
            uint32_t handle; turbowasm_component_handle_kind k; void *object;
            if (!turbowasm_component_handle_at(&p->tables[kind], i, &handle, &k, &object)) continue;
            status = tw_network_drop(p, kind, handle); if (status != TURBOWASM_OK) return status;
        }
    }
    for (unsigned i = 0; i < TW_NETWORK_KINDS; ++i) {
        turbowasm_component_resource_table_destroy(&p->tables[i]); turbowasm_rt_free(p->slots[i]);
    }
    turbowasm_rt_free(p); s->network = NULL; return TURBOWASM_OK;
}
turbowasm_status tw_network_init(turbowasm_wasi02_sockets *s, const turbowasm_wasi02_config_v2 *c) {
    turbowasm_wasi02_network *p; const turbowasm_wasi02_network_provider *v = &c->network;
    if (!s || s->network || c->size != sizeof(*c) || c->api_version != 2 || v->size != sizeof(*v) || v->api_version != 1 ||
        (!c->udp_socket_capacity != !c->datagram_stream_capacity) ||
        c->udp_socket_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        c->datagram_stream_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        c->resolve_stream_capacity > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS) return TURBOWASM_INVALID_ARGUMENT;
    if (c->udp_socket_capacity && (!v->max_datagram_bytes || v->max_datagram_bytes > 65535 || !v->max_datagram_batch ||
        v->max_datagram_batch > UINT32_MAX || v->max_datagram_batch > SIZE_MAX / v->max_datagram_bytes ||
        !v->udp_create || !v->udp_drop || !v->udp_start_bind || !v->udp_finish_bind || !v->udp_stream ||
        !v->udp_local_address || !v->udp_remote_address || !v->udp_option_get || !v->udp_option_set || !v->udp_subscribe ||
        !v->incoming_receive || !v->incoming_subscribe || !v->incoming_drop || !v->outgoing_check_send ||
        !v->outgoing_send || !v->outgoing_subscribe || !v->outgoing_drop)) return TURBOWASM_INVALID_ARGUMENT;
    if (c->resolve_stream_capacity && (!v->resolve_addresses || !v->resolve_next_address ||
        !v->resolve_subscribe || !v->resolve_drop)) return TURBOWASM_INVALID_ARGUMENT;
    p = turbowasm_rt_calloc(1, sizeof(*p)); if (!p) return TURBOWASM_OUT_OF_MEMORY;
    s->network = p; p->provider = *v;
    p->capacities[0] = c->udp_socket_capacity; p->capacities[1] = p->capacities[2] = c->datagram_stream_capacity;
    p->capacities[3] = c->resolve_stream_capacity;
    for (unsigned i = 0; i < TW_NETWORK_KINDS; ++i) if (p->capacities[i]) {
        p->slots[i] = turbowasm_rt_calloc(p->capacities[i], sizeof(tw_network_slot));
        if (!p->slots[i] || !turbowasm_component_resource_table_init(&p->tables[i], p->capacities[i])) {
            (void)tw_network_destroy(s); return TURBOWASM_OUT_OF_MEMORY;
        }
    }
    return TURBOWASM_OK;
}
typedef struct reservation {
    turbowasm_wasi02_network *p; unsigned kind; uint32_t handle, index;
} reservation;
static turbowasm_status reserve(turbowasm_wasi02_network *p, unsigned kind, reservation *r) {
    turbowasm_status status; *r = (reservation){p, kind, 0, 0};
    for (r->index = 0; r->index < p->capacities[kind]; ++r->index) if (!p->slots[kind][r->index].active) break;
    if (r->index == p->capacities[kind]) return TURBOWASM_OUT_OF_MEMORY;
    p->slots[kind][r->index].active = true;
    status = turbowasm_component_handle_insert(&p->tables[kind], TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION, r, &r->handle);
    if (status != TURBOWASM_OK) memset(&p->slots[kind][r->index], 0, sizeof(tw_network_slot));
    return status;
}
static void cancel(reservation *r) {
    if (r->handle) { void *owner;
        (void)turbowasm_component_handle_remove(&r->p->tables[r->kind], r->handle, TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION, &owner);
        memset(&r->p->slots[r->kind][r->index], 0, sizeof(tw_network_slot)); r->handle = 0;
    }
}
static turbowasm_status publish(reservation *r, turbowasm_value rep, turbowasm_wasi02_ip_address_family family) {
    turbowasm_value index = {.kind = TURBOWASM_VALUE_I32, .as.i32 = (int32_t)r->index};
    turbowasm_status status = turbowasm_component_resource_publish(&r->p->tables[r->kind], r->handle, r,
        tw_network_resource_id(r->kind), index);
    if (status == TURBOWASM_OK) { tw_network_slot *s = &r->p->slots[r->kind][r->index]; s->provider_rep = rep; s->family = family; }
    return status;
}
static turbowasm_status result_prepare(turbowasm_wasi02_value *out) {
    memset(out, 0, sizeof(*out)); out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.value = turbowasm_rt_calloc(1, sizeof(*out->as.result.value));
    return out->as.result.value ? TURBOWASM_OK : TURBOWASM_OUT_OF_MEMORY;
}
static turbowasm_status finish(turbowasm_status status, turbowasm_wasi02_socket_error error, turbowasm_wasi02_value *out) {
    if (status != TURBOWASM_OK || error > TURBOWASM_WASI02_SOCKET_ERROR_PERMANENT_RESOLVER_FAILURE || error < 0) {
        turbowasm_wasi02_value_destroy(out); return status == TURBOWASM_OK ? TURBOWASM_TRAPPED : status;
    }
    if (error) { turbowasm_wasi02_value_destroy(out->as.result.value); out->as.result.is_error = true;
        out->as.result.value->kind = TURBOWASM_WASI02_VALUE_ENUM; out->as.result.value->as.enum_index = (uint32_t)error - 1;
    }
    return TURBOWASM_OK;
}
static void unit(turbowasm_wasi02_value *out) { turbowasm_rt_free(out->as.result.value); out->as.result.value = NULL; }
static turbowasm_status address_prepare(turbowasm_wasi02_value *v, bool socket) {
    v->kind = TURBOWASM_WASI02_VALUE_VARIANT;
    v->as.variant.value = turbowasm_rt_calloc(1, sizeof(*v)); if (!v->as.variant.value) return TURBOWASM_OUT_OF_MEMORY;
    turbowasm_wasi02_value *tuple = v->as.variant.value;
    if (socket) { tuple->kind = TURBOWASM_WASI02_VALUE_RECORD;
        tuple->as.record.items = turbowasm_rt_calloc(4, sizeof(*v)); if (!tuple->as.record.items) return TURBOWASM_OUT_OF_MEMORY;
        tuple->as.record.count = 4;
        tuple = &tuple->as.record.items[2];
    }
    tuple->kind = TURBOWASM_WASI02_VALUE_TUPLE;
    tuple->as.tuple.items = turbowasm_rt_calloc(8, sizeof(*v));
    if (tuple->as.tuple.items) tuple->as.tuple.count = 8;
    return tuple->as.tuple.items ? TURBOWASM_OK : TURBOWASM_OUT_OF_MEMORY;
}
static void address_fill(turbowasm_wasi02_value *v, const turbowasm_wasi02_ip_socket_address *a, bool socket) {
    bool wide = a->family == TURBOWASM_WASI02_IP_ADDRESS_IPV6;
    turbowasm_wasi02_value *tuple = v->as.variant.value; v->as.variant.case_index = wide ? 1 : 0;
    if (socket) { turbowasm_wasi02_value *items = tuple->as.record.items;
        items[0].kind = TURBOWASM_WASI02_VALUE_U16; items[0].as.u16 = wide ? a->as.ipv6.port : a->as.ipv4.port;
        if (!wide) { items[1] = items[2]; memset(&items[2], 0, sizeof(*items)); tuple->as.record.count = 2; tuple = &items[1]; }
        else { items[1].kind = items[3].kind = TURBOWASM_WASI02_VALUE_U32;
            items[1].as.u32 = a->as.ipv6.flow_info; items[3].as.u32 = a->as.ipv6.scope_id; tuple = &items[2]; }
    }
    tuple->as.tuple.count = wide ? 8 : 4;
    for (size_t i = 0; i < tuple->as.tuple.count; ++i) {
        tuple->as.tuple.items[i].kind = wide ? TURBOWASM_WASI02_VALUE_U16 : TURBOWASM_WASI02_VALUE_U8;
        if (wide) tuple->as.tuple.items[i].as.u16 = a->as.ipv6.address[i]; else tuple->as.tuple.items[i].as.u8 = a->as.ipv4.address[i];
    }
}

static turbowasm_status receive_datagrams(turbowasm_wasi02_network *p, tw_network_slot *slot,
    uint64_t maximum, turbowasm_wasi02_value *out) {
    size_t capacity = maximum < p->provider.max_datagram_batch ? (size_t)maximum : p->provider.max_datagram_batch;
    size_t count = 0; turbowasm_wasi02_incoming_datagram *records = NULL; uint8_t *bytes = NULL;
    turbowasm_status status = result_prepare(out); turbowasm_wasi02_socket_error error = 0;
    if (status != TURBOWASM_OK) return status;
    turbowasm_wasi02_value *list = out->as.result.value; list->kind = TURBOWASM_WASI02_VALUE_LIST;
    if (capacity) {
        records = turbowasm_rt_calloc(capacity, sizeof(*records));
        bytes = turbowasm_rt_malloc(capacity * p->provider.max_datagram_bytes);
        list->as.list.items = turbowasm_rt_calloc(capacity, sizeof(*list));
        if (!records || !bytes || !list->as.list.items) { status = TURBOWASM_OUT_OF_MEMORY; goto done; }
        list->as.list.count = capacity;
        for (size_t i = 0; i < capacity; ++i) {
            turbowasm_wasi02_value *record = &list->as.list.items[i]; record->kind = TURBOWASM_WASI02_VALUE_RECORD;
            record->as.record.items = turbowasm_rt_calloc(2, sizeof(*record));
            if (!record->as.record.items) { status = TURBOWASM_OUT_OF_MEMORY; goto done; }
            record->as.record.count = 2;
            turbowasm_wasi02_value *payload = &record->as.record.items[0]; payload->kind = TURBOWASM_WASI02_VALUE_LIST;
            payload->as.list.items = turbowasm_rt_calloc(p->provider.max_datagram_bytes, sizeof(*payload));
            if (!payload->as.list.items) { status = TURBOWASM_OUT_OF_MEMORY; goto done; }
            status = address_prepare(&record->as.record.items[1], true); if (status != TURBOWASM_OK) goto done;
            records[i].data = bytes + i * p->provider.max_datagram_bytes; records[i].capacity = p->provider.max_datagram_bytes;
        }
    }
    status = p->provider.incoming_receive(p->provider.context, slot->provider_rep, records, capacity, &count, &error);
    if (status != TURBOWASM_OK || error) goto done;
    if (count > capacity) { status = TURBOWASM_TRAPPED; goto done; }
    for (size_t i = 0; i < count; ++i) if (records[i].size > p->provider.max_datagram_bytes ||
        (records[i].remote_address.family != TURBOWASM_WASI02_IP_ADDRESS_IPV4 &&
         records[i].remote_address.family != TURBOWASM_WASI02_IP_ADDRESS_IPV6)) { status = TURBOWASM_TRAPPED; goto done; }
    for (size_t i = 0; i < capacity; ++i) {
        turbowasm_wasi02_value *record = &list->as.list.items[i];
        if (i >= count) turbowasm_wasi02_value_destroy(record);
        else {
            turbowasm_wasi02_value *payload = &record->as.record.items[0]; payload->as.list.count = records[i].size;
            for (size_t j = 0; j < records[i].size; ++j) {
                payload->as.list.items[j].kind = TURBOWASM_WASI02_VALUE_U8; payload->as.list.items[j].as.u8 = records[i].data[j];
            }
            address_fill(&record->as.record.items[1], &records[i].remote_address, true);
        }
    }
    list->as.list.count = count;
done:
    turbowasm_rt_free(records); turbowasm_rt_free(bytes); return finish(status, error, out);
}
static turbowasm_status send_datagrams(turbowasm_wasi02_network *p, tw_network_slot *slot,
    const turbowasm_wasi02_value *list, turbowasm_wasi02_value *out) {
    size_t count = list->as.list.count, sent = 0, byte_count = 0, offset = 0;
    turbowasm_wasi02_outgoing_datagram *records = NULL; uint8_t *bytes = NULL;
    turbowasm_wasi02_socket_error error = 0; turbowasm_status status;
    if (!slot->checked || count > slot->permit) return TURBOWASM_TRAPPED;
    if (count > SIZE_MAX / sizeof(*records)) return TURBOWASM_OUT_OF_MEMORY;
    status = result_prepare(out); if (status != TURBOWASM_OK) return status;
    for (size_t i = 0; i < count; ++i) {
        size_t size = list->as.list.items[i].as.record.items[0].as.list.count;
        if (size > SIZE_MAX - byte_count) { status = TURBOWASM_OUT_OF_MEMORY; goto done; } byte_count += size;
    }
    if (count) { records = turbowasm_rt_calloc(count, sizeof(*records)); bytes = turbowasm_rt_malloc(byte_count ? byte_count : 1);
        if (!records || !bytes) { status = TURBOWASM_OUT_OF_MEMORY; goto done; }
    }
    for (size_t i = 0; i < count; ++i) {
        const turbowasm_wasi02_value *r = list->as.list.items[i].as.record.items;
        records[i].data = bytes + offset; records[i].size = r[0].as.list.count; records[i].has_remote_address = r[1].as.option.has_value;
        for (size_t j = 0; j < records[i].size; ++j) bytes[offset++] = r[0].as.list.items[j].as.u8;
        if (records[i].has_remote_address) {
            status = turbowasm_wasi02_socket_address_from_value(r[1].as.option.value, &records[i].remote_address);
            if (status != TURBOWASM_OK) goto done;
        }
    }
    slot->checked = false; slot->permit = 0;
    status = p->provider.outgoing_send(p->provider.context, slot->provider_rep, records, count, &sent, &error);
    if (status == TURBOWASM_OK && sent > count) status = TURBOWASM_TRAPPED;
    if (status == TURBOWASM_OK && sent && error) status = TURBOWASM_TRAPPED;
    if (status == TURBOWASM_OK && !error) { out->as.result.value->kind = TURBOWASM_WASI02_VALUE_U64; out->as.result.value->as.u64 = sent; }
done:
    turbowasm_rt_free(records); turbowasm_rt_free(bytes); return finish(status, error, out);
}
static turbowasm_status create_resource(turbowasm_wasi02_sockets *s, unsigned kind,
    const turbowasm_wasi02_value *args, turbowasm_wasi02_value *out) {
    turbowasm_wasi02_network *p = s->network; reservation r = {0}; turbowasm_value rep = {0}, network = {0};
    turbowasm_wasi02_socket_error error = 0; turbowasm_status status;
    turbowasm_wasi02_ip_address_family family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
    status = result_prepare(out); if (status != TURBOWASM_OK) return status;
    if (kind == TW_NETWORK_RESOLVE) {
        status = turbowasm_component_resource_rep(&s->networks, args[0].as.resource, UINT64_C(0x77617369326e6574), &network);
        if (status != TURBOWASM_OK) return finish(status, error, out);
    } else family = args[0].as.enum_index == 0 ? TURBOWASM_WASI02_IP_ADDRESS_IPV4 : TURBOWASM_WASI02_IP_ADDRESS_IPV6;
    bool free_slot = false;
    for (uint32_t i = 0; i < p->capacities[kind]; ++i) if (!p->slots[kind][i].active) { free_slot = true; break; }
    if (!free_slot) return finish(TURBOWASM_OK, kind == TW_NETWORK_UDP ? TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT : TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY, out);
    status = reserve(p, kind, &r); if (status != TURBOWASM_OK) return finish(status, error, out);
    if (kind == TW_NETWORK_UDP) status = p->provider.udp_create(p->provider.context, family, &rep, &error);
    else status = p->provider.resolve_addresses(p->provider.context, network,
        (turbowasm_wasi02_string_view){args[1].as.string.data, args[1].as.string.size}, &rep, &error);
    if (status == TURBOWASM_OK && !error) {
        status = publish(&r, rep, family);
        if (status == TURBOWASM_OK) { out->as.result.value->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
            out->as.result.value->as.resource = r.handle; r.handle = 0; }
        else (void)provider_drop(p, kind, rep);
    }
    cancel(&r); return finish(status, error, out);
}
static turbowasm_status stream_pair(turbowasm_wasi02_network *p, tw_network_slot *slot,
    const turbowasm_wasi02_value *remote, turbowasm_wasi02_value *out) {
    reservation r[2] = {{0}}; turbowasm_value reps[2] = {{0}}; turbowasm_wasi02_ip_socket_address address;
    turbowasm_wasi02_socket_error error = 0; turbowasm_status status = result_prepare(out);
    if (status != TURBOWASM_OK) return status;
    if (slot->state != 2) return finish(TURBOWASM_OK, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE, out);
    for (unsigned i = 0; i < 2; ++i) if (slot_get(p, TW_NETWORK_INCOMING + i, slot->children[i]))
        return finish(TURBOWASM_TRAPPED, 0, out);
    if (remote->as.option.has_value) {
        status = turbowasm_wasi02_socket_address_from_value(remote->as.option.value, &address);
        if (status != TURBOWASM_OK) return finish(status, 0, out);
    }
    out->as.result.value->kind = TURBOWASM_WASI02_VALUE_TUPLE;
    out->as.result.value->as.tuple.items = turbowasm_rt_calloc(2, sizeof(*out->as.result.value));
    if (!out->as.result.value->as.tuple.items) return finish(TURBOWASM_OUT_OF_MEMORY, 0, out);
    out->as.result.value->as.tuple.count = 2;
    for (unsigned i = 0; i < 2; ++i) { status = reserve(p, TW_NETWORK_INCOMING + i, &r[i]); if (status != TURBOWASM_OK) goto done; }
    status = p->provider.udp_stream(p->provider.context, slot->provider_rep,
        remote->as.option.has_value ? &address : NULL, &reps[0], &reps[1], &error);
    if (status == TURBOWASM_OK && !error) {
        for (unsigned i = 0; i < 2; ++i) {
            status = publish(&r[i], reps[i], slot->family);
            if (status != TURBOWASM_OK) {
                for (unsigned j = 0; j < i; ++j) (void)tw_network_drop(p, TW_NETWORK_INCOMING + j, slot->children[j]);
                for (unsigned j = i; j < 2; ++j) (void)provider_drop(p, TW_NETWORK_INCOMING + j, reps[j]);
                goto done;
            }
            slot->children[i] = r[i].handle; out->as.result.value->as.tuple.items[i].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
            out->as.result.value->as.tuple.items[i].as.resource = r[i].handle; r[i].handle = 0;
        }
    }
done:
    for (unsigned i = 0; i < 2; ++i) cancel(&r[i]); return finish(status, error, out);
}
static turbowasm_status subscribe(turbowasm_wasi02_sockets *s, unsigned kind, tw_network_slot *slot, turbowasm_wasi02_value *out) {
    turbowasm_wasi02_network_provider *v = &s->network->provider; turbowasm_value rep = {0}; uint32_t handle;
    turbowasm_status status;
    if (kind == TW_NETWORK_UDP) status = v->udp_subscribe(v->context, slot->provider_rep, &rep);
    else if (kind == TW_NETWORK_INCOMING) status = v->incoming_subscribe(v->context, slot->provider_rep, &rep);
    else if (kind == TW_NETWORK_OUTGOING) status = v->outgoing_subscribe(v->context, slot->provider_rep, &rep);
    else status = v->resolve_subscribe(v->context, slot->provider_rep, &rep);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_wasi02_pollable_new(s->poll, rep, &handle);
    if (status != TURBOWASM_OK) { if (s->poll->provider.drop) (void)s->poll->provider.drop(s->poll->provider.context, rep); return status; }
    out->kind = TURBOWASM_WASI02_VALUE_RESOURCE; out->as.resource = handle; return TURBOWASM_OK;
}
turbowasm_status tw_network_call(turbowasm_wasi02_sockets *s, const char *iface, const char *function,
    const turbowasm_wasi02_value *args, size_t count, turbowasm_wasi02_value *out) {
    turbowasm_wasi02_network *p = s->network; turbowasm_status status; turbowasm_wasi02_socket_error error = 0;
    const turbowasm_wasi02_interface_desc *desc = turbowasm_wasi02_find_interface("wasi:sockets", iface);
    const turbowasm_wasi02_function_desc *fn = turbowasm_wasi02_find_function(desc, function);
    tw_network_slot *slot; unsigned kind; uint32_t handle; const char *method;
    if (!p || !fn) return TURBOWASM_UNSUPPORTED;
    if (count != fn->param_count || (count && !args)) return TURBOWASM_TYPE_MISMATCH;
    for (size_t i = 0; i < count; ++i) if (!turbowasm_wasi02_value_matches_type(fn->params[i].type, &args[i])) return TURBOWASM_TYPE_MISMATCH;
    memset(out, 0, sizeof(*out));
    if (!strcmp(iface, "udp-create-socket")) return p->capacities[0] ? create_resource(s, 0, args, out) : TURBOWASM_UNSUPPORTED;
    if (!strcmp(function, "resolve-addresses")) return p->capacities[3] ? create_resource(s, 3, args, out) : TURBOWASM_UNSUPPORTED;
    if (!strncmp(function, "[method]udp-socket.", sizeof("[method]udp-socket.") - 1)) kind = TW_NETWORK_UDP;
    else if (!strncmp(function, "[method]incoming-datagram-stream.", sizeof("[method]incoming-datagram-stream.") - 1)) kind = TW_NETWORK_INCOMING;
    else if (!strncmp(function, "[method]outgoing-datagram-stream.", sizeof("[method]outgoing-datagram-stream.") - 1)) kind = TW_NETWORK_OUTGOING;
    else kind = TW_NETWORK_RESOLVE;
    method = strchr(function, '.'); if (!method) return TURBOWASM_UNSUPPORTED; ++method;
    handle = args[0].as.resource; slot = slot_get(p, kind, handle); if (!slot) return TURBOWASM_TRAPPED;
    status = turbowasm_component_resource_lend_acquire(&p->tables[kind], handle, tw_network_resource_id(kind));
    if (status != TURBOWASM_OK) return status;
    if (!strcmp(method, "subscribe")) { status = subscribe(s, kind, slot, out); goto done; }
    if (!strcmp(method, "address-family")) { out->kind = TURBOWASM_WASI02_VALUE_ENUM;
        out->as.enum_index = slot->family == TURBOWASM_WASI02_IP_ADDRESS_IPV6; status = TURBOWASM_OK; goto done; }
    if (kind == TW_NETWORK_INCOMING) { status = receive_datagrams(p, slot, args[1].as.u64, out); goto done; }
    if (kind == TW_NETWORK_OUTGOING && !strcmp(method, "send")) { status = send_datagrams(p, slot, &args[1], out); goto done; }
    if (kind == TW_NETWORK_UDP && !strcmp(method, "stream")) { status = stream_pair(p, slot, &args[1], out); goto done; }
    status = result_prepare(out); if (status != TURBOWASM_OK) goto done;
    if (kind == TW_NETWORK_RESOLVE) {
        turbowasm_wasi02_value *option = out->as.result.value; turbowasm_wasi02_ip_address ip; bool has = false;
        option->kind = TURBOWASM_WASI02_VALUE_OPTION; option->as.option.value = turbowasm_rt_calloc(1, sizeof(*option));
        if (!option->as.option.value) { status = TURBOWASM_OUT_OF_MEMORY; goto result_done; }
        status = address_prepare(option->as.option.value, false); if (status != TURBOWASM_OK) goto result_done;
        status = p->provider.resolve_next_address(p->provider.context, slot->provider_rep, &has, &ip, &error);
        if (status == TURBOWASM_OK && !error) {
            if (!has) { turbowasm_wasi02_value_destroy(option->as.option.value); turbowasm_rt_free(option->as.option.value); option->as.option.value = NULL; }
            else { turbowasm_wasi02_ip_socket_address a = {0}; a.family = ip.family;
                if (ip.family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) memcpy(a.as.ipv4.address, ip.as.ipv4, 4);
                else if (ip.family == TURBOWASM_WASI02_IP_ADDRESS_IPV6) memcpy(a.as.ipv6.address, ip.as.ipv6, 16);
                else { status = TURBOWASM_TRAPPED; goto result_done; }
                option->as.option.has_value = true; address_fill(option->as.option.value, &a, false);
            }
        }
    } else if (kind == TW_NETWORK_OUTGOING) {
        uint64_t permit = 0; status = p->provider.outgoing_check_send(p->provider.context, slot->provider_rep, &permit, &error);
        if (status == TURBOWASM_OK && !error) { slot->checked = true; slot->permit = permit;
            out->as.result.value->kind = TURBOWASM_WASI02_VALUE_U64; out->as.result.value->as.u64 = permit; }
        else { slot->checked = false; slot->permit = 0; }
    } else if (!strcmp(method, "start-bind")) {
        turbowasm_value network; turbowasm_wasi02_ip_socket_address address;
        if (slot->state) error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        else { status = turbowasm_component_resource_rep(&s->networks, args[1].as.resource, UINT64_C(0x77617369326e6574), &network);
            if (status == TURBOWASM_OK) status = turbowasm_wasi02_socket_address_from_value(&args[2], &address);
            if (status == TURBOWASM_OK) status = p->provider.udp_start_bind(p->provider.context, slot->provider_rep, network, &address, &error);
            if (status == TURBOWASM_OK && !error) slot->state = 1;
        }
        if (status == TURBOWASM_OK && !error) unit(out);
    } else if (!strcmp(method, "finish-bind")) {
        if (slot->state != 1) error = TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS;
        else { status = p->provider.udp_finish_bind(p->provider.context, slot->provider_rep, &error);
            if (status == TURBOWASM_OK && !error) slot->state = 2;
            else if (status == TURBOWASM_OK && error != TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK) slot->state = 0;
        }
        if (status == TURBOWASM_OK && !error) unit(out);
    } else if (!strcmp(method, "local-address") || !strcmp(method, "remote-address")) {
        turbowasm_wasi02_ip_socket_address a = {0};
        status = address_prepare(out->as.result.value, true); if (status != TURBOWASM_OK) goto result_done;
        if (slot->state != 2) error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE;
        else if (*method == 'l') status = p->provider.udp_local_address(p->provider.context, slot->provider_rep, &a, &error);
        else status = p->provider.udp_remote_address(p->provider.context, slot->provider_rep, &a, &error);
        if (status == TURBOWASM_OK && !error) {
            if (a.family != slot->family) status = TURBOWASM_TRAPPED; else address_fill(out->as.result.value, &a, true);
        }
    } else {
        bool set = !strncmp(method, "set-", 4); const char *name = set ? method + 4 : method;
        turbowasm_wasi02_udp_option option = !strcmp(name, "unicast-hop-limit") ? TURBOWASM_WASI02_UDP_HOP_LIMIT :
            !strcmp(name, "receive-buffer-size") ? TURBOWASM_WASI02_UDP_RECEIVE_BUFFER : TURBOWASM_WASI02_UDP_SEND_BUFFER;
        uint64_t value = set ? option == TURBOWASM_WASI02_UDP_HOP_LIMIT ? args[1].as.u8 : args[1].as.u64 : 0;
        if (set && !value) error = TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT;
        else if (set) status = p->provider.udp_option_set(p->provider.context, slot->provider_rep, option, value, &error);
        else status = p->provider.udp_option_get(p->provider.context, slot->provider_rep, option, &value, &error);
        if (status == TURBOWASM_OK && !error) {
            if (set) unit(out);
            else if (option == TURBOWASM_WASI02_UDP_HOP_LIMIT) { if (value > UINT8_MAX) status = TURBOWASM_TRAPPED;
                else { out->as.result.value->kind = TURBOWASM_WASI02_VALUE_U8; out->as.result.value->as.u8 = (uint8_t)value; } }
            else { out->as.result.value->kind = TURBOWASM_WASI02_VALUE_U64; out->as.result.value->as.u64 = value; }
        }
    }
result_done: status = finish(status, error, out);
done:
    (void)turbowasm_component_resource_lend_release(&p->tables[kind], handle, tw_network_resource_id(kind)); return status;
}
