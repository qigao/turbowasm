#include "wasi02_sockets.h"

#include "runtime_alloc.h"
#include "wasi02_component.h"
#include "wasi02_descriptor.h"

#include <limits.h>
#include <string.h>

#define TW_WASI02_NETWORK_ID UINT64_C(0x77617369326e6574)
#define TW_WASI02_TCP_SOCKET_ID UINT64_C(0x7761736932746370)

static bool socket_error_valid(turbowasm_wasi02_socket_error error) {
    return error >= TURBOWASM_WASI02_SOCKET_ERROR_NONE &&
           error <=
               TURBOWASM_WASI02_SOCKET_ERROR_PERMANENT_RESOLVER_FAILURE;
}

static turbowasm_status result_take_payload(
    turbowasm_wasi02_value *out,
    bool is_error,
    turbowasm_wasi02_value *payload) {
    turbowasm_wasi02_value *owned = NULL;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.is_error = is_error;
    if (payload == NULL)
        return TURBOWASM_OK;

    owned = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*owned));
    if (owned == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    *owned = *payload;
    memset(payload, 0, sizeof(*payload));
    out->as.result.value = owned;
    return TURBOWASM_OK;
}

static turbowasm_status result_error(
    turbowasm_wasi02_value *out,
    turbowasm_wasi02_socket_error error) {
    turbowasm_wasi02_value payload = {0};

    if (!socket_error_valid(error) ||
        error == TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return TURBOWASM_INVALID_ARGUMENT;

    payload.kind = TURBOWASM_WASI02_VALUE_ENUM;
    payload.as.enum_index = (uint32_t)error - 1u;
    return result_take_payload(out, true, &payload);
}

static turbowasm_status result_unit_ok(
    turbowasm_wasi02_value *out) {
    return result_take_payload(out, false, NULL);
}

static turbowasm_status result_bool_ok(
    turbowasm_wasi02_value *out,
    bool value) {
    turbowasm_wasi02_value payload = {0};
    payload.kind = TURBOWASM_WASI02_VALUE_BOOL;
    payload.as.boolean = value;
    return result_take_payload(out, false, &payload);
}

static turbowasm_status result_u8_ok(
    turbowasm_wasi02_value *out,
    uint8_t value) {
    turbowasm_wasi02_value payload = {0};
    payload.kind = TURBOWASM_WASI02_VALUE_U8;
    payload.as.u8 = value;
    return result_take_payload(out, false, &payload);
}

static turbowasm_status result_u32_ok(
    turbowasm_wasi02_value *out,
    uint32_t value) {
    turbowasm_wasi02_value payload = {0};
    payload.kind = TURBOWASM_WASI02_VALUE_U32;
    payload.as.u32 = value;
    return result_take_payload(out, false, &payload);
}

static turbowasm_status result_u64_ok(
    turbowasm_wasi02_value *out,
    uint64_t value) {
    turbowasm_wasi02_value payload = {0};
    payload.kind = TURBOWASM_WASI02_VALUE_U64;
    payload.as.u64 = value;
    return result_take_payload(out, false, &payload);
}

static turbowasm_status result_resource_ok(
    turbowasm_wasi02_value *out,
    uint32_t resource) {
    turbowasm_wasi02_value payload = {0};

    if (resource == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    payload.kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    payload.as.resource = resource;
    return result_take_payload(out, false, &payload);
}

static turbowasm_status make_result_tuple_resources(
    const uint32_t *resources,
    size_t count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value tuple = {0};
    turbowasm_wasi02_value *items = NULL;
    size_t i;
    turbowasm_status status;

    if (resources == NULL || out == NULL || count == 0u ||
        count > SIZE_MAX / sizeof(*items))
        return TURBOWASM_INVALID_ARGUMENT;

    items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        count, sizeof(*items));
    if (items == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    for (i = 0u; i < count; ++i) {
        items[i].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
        items[i].as.resource = resources[i];
    }

    tuple.kind = TURBOWASM_WASI02_VALUE_TUPLE;
    tuple.as.tuple.items = items;
    tuple.as.tuple.count = count;
    status = result_take_payload(out, false, &tuple);
    if (status != TURBOWASM_OK)
        turbowasm_wasi02_value_destroy(&tuple);
    return status;
}

static bool family_valid(
    turbowasm_wasi02_ip_address_family family) {
    return family == TURBOWASM_WASI02_IP_ADDRESS_IPV4 ||
           family == TURBOWASM_WASI02_IP_ADDRESS_IPV6;
}

static turbowasm_status address_from_value(
    const turbowasm_wasi02_value *value,
    turbowasm_wasi02_ip_socket_address *out) {
    const turbowasm_wasi02_value *record;
    const turbowasm_wasi02_value *tuple;
    size_t i;

    if (value == NULL || out == NULL ||
        value->kind != TURBOWASM_WASI02_VALUE_VARIANT ||
        value->as.variant.value == NULL ||
        value->as.variant.case_index > 1u)
        return TURBOWASM_TYPE_MISMATCH;

    memset(out, 0, sizeof(*out));
    out->family = value->as.variant.case_index == 0u
        ? TURBOWASM_WASI02_IP_ADDRESS_IPV4
        : TURBOWASM_WASI02_IP_ADDRESS_IPV6;

    record = value->as.variant.value;
    if (record->kind != TURBOWASM_WASI02_VALUE_RECORD)
        return TURBOWASM_TYPE_MISMATCH;

    if (out->family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) {
        if (record->as.record.count != 2u ||
            record->as.record.items == NULL ||
            record->as.record.items[0].kind !=
                TURBOWASM_WASI02_VALUE_U16 ||
            record->as.record.items[1].kind !=
                TURBOWASM_WASI02_VALUE_TUPLE)
            return TURBOWASM_TYPE_MISMATCH;
        tuple = &record->as.record.items[1];
        if (tuple->as.tuple.count != 4u ||
            tuple->as.tuple.items == NULL)
            return TURBOWASM_TYPE_MISMATCH;
        out->as.ipv4.port = record->as.record.items[0].as.u16;
        for (i = 0u; i < 4u; ++i) {
            if (tuple->as.tuple.items[i].kind !=
                TURBOWASM_WASI02_VALUE_U8)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.ipv4.address[i] =
                tuple->as.tuple.items[i].as.u8;
        }
        return TURBOWASM_OK;
    }

    if (record->as.record.count != 4u ||
        record->as.record.items == NULL ||
        record->as.record.items[0].kind !=
            TURBOWASM_WASI02_VALUE_U16 ||
        record->as.record.items[1].kind !=
            TURBOWASM_WASI02_VALUE_U32 ||
        record->as.record.items[2].kind !=
            TURBOWASM_WASI02_VALUE_TUPLE ||
        record->as.record.items[3].kind !=
            TURBOWASM_WASI02_VALUE_U32)
        return TURBOWASM_TYPE_MISMATCH;

    tuple = &record->as.record.items[2];
    if (tuple->as.tuple.count != 8u ||
        tuple->as.tuple.items == NULL)
        return TURBOWASM_TYPE_MISMATCH;

    out->as.ipv6.port = record->as.record.items[0].as.u16;
    out->as.ipv6.flow_info =
        record->as.record.items[1].as.u32;
    out->as.ipv6.scope_id =
        record->as.record.items[3].as.u32;
    for (i = 0u; i < 8u; ++i) {
        if (tuple->as.tuple.items[i].kind !=
            TURBOWASM_WASI02_VALUE_U16)
            return TURBOWASM_TYPE_MISMATCH;
        out->as.ipv6.address[i] =
            tuple->as.tuple.items[i].as.u16;
    }
    return TURBOWASM_OK;
}

static turbowasm_status address_to_value(
    const turbowasm_wasi02_ip_socket_address *address,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *record_items = NULL;
    turbowasm_wasi02_value *tuple_items = NULL;
    turbowasm_wasi02_value *payload = NULL;
    size_t tuple_count;
    size_t record_count;
    size_t i;

    if (address == NULL || out == NULL ||
        !family_valid(address->family))
        return TURBOWASM_INVALID_ARGUMENT;

    tuple_count = address->family ==
        TURBOWASM_WASI02_IP_ADDRESS_IPV4 ? 4u : 8u;
    record_count = address->family ==
        TURBOWASM_WASI02_IP_ADDRESS_IPV4 ? 2u : 4u;

    record_items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        record_count, sizeof(*record_items));
    tuple_items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        tuple_count, sizeof(*tuple_items));
    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (record_items == NULL || tuple_items == NULL ||
        payload == NULL) {
        turbowasm_rt_free(record_items);
        turbowasm_rt_free(tuple_items);
        turbowasm_rt_free(payload);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    record_items[0].kind = TURBOWASM_WASI02_VALUE_U16;
    if (address->family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) {
        record_items[0].as.u16 = address->as.ipv4.port;
        for (i = 0u; i < 4u; ++i) {
            tuple_items[i].kind = TURBOWASM_WASI02_VALUE_U8;
            tuple_items[i].as.u8 = address->as.ipv4.address[i];
        }
        record_items[1].kind = TURBOWASM_WASI02_VALUE_TUPLE;
        record_items[1].as.tuple.items = tuple_items;
        record_items[1].as.tuple.count = 4u;
    } else {
        record_items[0].as.u16 = address->as.ipv6.port;
        record_items[1].kind = TURBOWASM_WASI02_VALUE_U32;
        record_items[1].as.u32 = address->as.ipv6.flow_info;
        for (i = 0u; i < 8u; ++i) {
            tuple_items[i].kind = TURBOWASM_WASI02_VALUE_U16;
            tuple_items[i].as.u16 = address->as.ipv6.address[i];
        }
        record_items[2].kind = TURBOWASM_WASI02_VALUE_TUPLE;
        record_items[2].as.tuple.items = tuple_items;
        record_items[2].as.tuple.count = 8u;
        record_items[3].kind = TURBOWASM_WASI02_VALUE_U32;
        record_items[3].as.u32 = address->as.ipv6.scope_id;
    }

    payload->kind = TURBOWASM_WASI02_VALUE_RECORD;
    payload->as.record.items = record_items;
    payload->as.record.count = record_count;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_VARIANT;
    out->as.variant.case_index =
        address->family == TURBOWASM_WASI02_IP_ADDRESS_IPV4
            ? 0u : 1u;
    out->as.variant.value = payload;
    return TURBOWASM_OK;
}

static turbowasm_status result_address_ok(
    turbowasm_wasi02_value *out,
    const turbowasm_wasi02_ip_socket_address *address) {
    turbowasm_wasi02_value payload = {0};
    turbowasm_status status = address_to_value(address, &payload);

    if (status != TURBOWASM_OK)
        return status;
    status = result_take_payload(out, false, &payload);
    if (status != TURBOWASM_OK)
        turbowasm_wasi02_value_destroy(&payload);
    return status;
}

static turbowasm_wasi02_tcp_slot *reserve_tcp_slot(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_value provider_rep,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_wasi02_tcp_state state,
    uint32_t *out_index) {
    uint32_t index;
    turbowasm_wasi02_tcp_slot *slot;

    if (sockets == NULL || out_index == NULL ||
        sockets->tcp_free_count == 0u ||
        !family_valid(family))
        return NULL;

    index = sockets->tcp_free_indices[--sockets->tcp_free_count];
    slot = &sockets->tcp_slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->provider_rep = provider_rep;
    slot->family = family;
    slot->state = state;
    *out_index = index;
    return slot;
}

static void release_tcp_slot(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot) {
    uint32_t index;

    if (sockets == NULL || slot == NULL || !slot->active)
        return;
    index = (uint32_t)(slot - sockets->tcp_slots);
    memset(slot, 0, sizeof(*slot));
    sockets->tcp_free_indices[sockets->tcp_free_count++] = index;
}

static turbowasm_status tcp_resource_new(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_value provider_rep,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_wasi02_tcp_state state,
    uint32_t *out_resource) {
    turbowasm_wasi02_tcp_slot *slot;
    turbowasm_value table_rep = {0};
    uint32_t index = 0u;
    turbowasm_status status;

    if (sockets == NULL || !sockets->initialized ||
        out_resource == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    slot = reserve_tcp_slot(
        sockets, provider_rep, family, state, &index);
    if (slot == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    table_rep.kind = TURBOWASM_VALUE_I32;
    table_rep.as.i32 = (int32_t)(index + 1u);
    status = turbowasm_component_resource_new_owned(
        &sockets->tcp_resources,
        TW_WASI02_TCP_SOCKET_ID,
        table_rep,
        out_resource);
    if (status != TURBOWASM_OK)
        release_tcp_slot(sockets, slot);
    return status;
}

static turbowasm_status tcp_slot_get(
    turbowasm_wasi02_sockets *sockets,
    uint32_t resource,
    turbowasm_wasi02_tcp_slot **out_slot) {
    turbowasm_value rep = {0};
    uint32_t encoded;
    uint32_t index;
    turbowasm_status status;

    if (sockets == NULL || !sockets->initialized ||
        out_slot == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        &sockets->tcp_resources,
        resource,
        TW_WASI02_TCP_SOCKET_ID,
        &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (rep.kind != TURBOWASM_VALUE_I32 || rep.as.i32 <= 0)
        return TURBOWASM_TRAPPED;
    encoded = (uint32_t)rep.as.i32;
    index = encoded - 1u;
    if (index >= sockets->tcp_capacity)
        return TURBOWASM_TRAPPED;
    if (!sockets->tcp_slots[index].active)
        return TURBOWASM_TRAPPED;
    *out_slot = &sockets->tcp_slots[index];
    return TURBOWASM_OK;
}

static turbowasm_status network_rep_get(
    turbowasm_wasi02_sockets *sockets,
    uint32_t resource,
    turbowasm_value *out_rep) {
    if (sockets == NULL || !sockets->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_resource_rep(
        &sockets->networks,
        resource,
        TW_WASI02_NETWORK_ID,
        out_rep);
}

static turbowasm_status semantic_result(
    turbowasm_status status,
    turbowasm_wasi02_socket_error error,
    turbowasm_wasi02_value *out) {
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return result_error(out, error);
    return TURBOWASM_OK;
}

static turbowasm_status stream_reps_drop(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_value input_rep,
    turbowasm_value output_rep) {
    if (sockets == NULL || sockets->streams == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (sockets->streams->provider.input_drop != NULL)
        sockets->streams->provider.input_drop(
            sockets->streams->provider.context, input_rep);
    if (sockets->streams->provider.output_drop != NULL)
        sockets->streams->provider.output_drop(
            sockets->streams->provider.context, output_rep);
    return TURBOWASM_OK;
}

static turbowasm_status wrap_stream_pair(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_value input_rep,
    turbowasm_value output_rep,
    uint32_t *out_input,
    uint32_t *out_output) {
    turbowasm_status status;

    if (sockets == NULL || sockets->streams == NULL ||
        out_input == NULL || out_output == NULL ||
        sockets->streams->provider.input_drop == NULL ||
        sockets->streams->provider.output_drop == NULL)
        return TURBOWASM_UNSUPPORTED;

    *out_input = 0u;
    *out_output = 0u;
    status = turbowasm_wasi02_input_stream_new(
        sockets->streams, input_rep, out_input);
    if (status != TURBOWASM_OK) {
        (void)stream_reps_drop(sockets, input_rep, output_rep);
        return status;
    }

    status = turbowasm_wasi02_output_stream_new(
        sockets->streams, output_rep, out_output);
    if (status != TURBOWASM_OK) {
        if (sockets->streams->provider.output_drop != NULL)
            sockets->streams->provider.output_drop(
                sockets->streams->provider.context, output_rep);
        (void)turbowasm_wasi02_stream_resource_drop(
            sockets->streams, *out_input);
        *out_input = 0u;
        return status;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_sockets_init(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_wasi02_socket_provider *provider,
    uint32_t max_networks,
    uint32_t max_tcp_sockets) {
    uint32_t i;

    if (sockets == NULL || provider == NULL ||
        sockets->initialized ||
        max_networks == 0u ||
        max_tcp_sockets == 0u ||
        max_networks > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        max_tcp_sockets > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        provider->instance_network == NULL ||
        provider->network_drop == NULL ||
        provider->tcp_create == NULL ||
        provider->tcp_drop == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(sockets, 0, sizeof(*sockets));
    sockets->tcp_slots = (turbowasm_wasi02_tcp_slot *)
        turbowasm_rt_calloc(
            max_tcp_sockets, sizeof(*sockets->tcp_slots));
    sockets->tcp_free_indices = (uint32_t *)turbowasm_rt_malloc(
        (size_t)max_tcp_sockets *
        sizeof(*sockets->tcp_free_indices));
    if (sockets->tcp_slots == NULL ||
        sockets->tcp_free_indices == NULL) {
        turbowasm_rt_free(sockets->tcp_slots);
        turbowasm_rt_free(sockets->tcp_free_indices);
        memset(sockets, 0, sizeof(*sockets));
        return TURBOWASM_OUT_OF_MEMORY;
    }

    if (!turbowasm_component_resource_table_init(
            &sockets->networks, max_networks) ||
        !turbowasm_component_resource_table_init(
            &sockets->tcp_resources, max_tcp_sockets)) {
        turbowasm_component_resource_table_destroy(
            &sockets->networks);
        turbowasm_component_resource_table_destroy(
            &sockets->tcp_resources);
        turbowasm_rt_free(sockets->tcp_slots);
        turbowasm_rt_free(sockets->tcp_free_indices);
        memset(sockets, 0, sizeof(*sockets));
        return TURBOWASM_INVALID_ARGUMENT;
    }

    sockets->provider = *provider;
    sockets->tcp_capacity = max_tcp_sockets;
    sockets->tcp_free_count = max_tcp_sockets;
    for (i = 0u; i < max_tcp_sockets; ++i)
        sockets->tcp_free_indices[i] =
            max_tcp_sockets - 1u - i;
    sockets->initialized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_sockets_destroy(
    turbowasm_wasi02_sockets *sockets) {
    if (sockets == NULL || !sockets->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    if (sockets->networks.live_count != 0u ||
        sockets->tcp_resources.live_count != 0u ||
        sockets->tcp_free_count != sockets->tcp_capacity)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_component_resource_table_destroy(
        &sockets->networks);
    turbowasm_component_resource_table_destroy(
        &sockets->tcp_resources);
    turbowasm_rt_free(sockets->tcp_slots);
    turbowasm_rt_free(sockets->tcp_free_indices);
    memset(sockets, 0, sizeof(*sockets));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_sockets_attach_io(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_poll *poll) {
    if (sockets == NULL || !sockets->initialized ||
        streams == NULL || !streams->initialized ||
        poll == NULL || !poll->initialized ||
        streams->poll != poll ||
        streams->provider.input_drop == NULL ||
        streams->provider.output_drop == NULL ||
        poll->provider.drop == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    sockets->streams = streams;
    sockets->poll = poll;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_tcp_state_get(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource,
    turbowasm_wasi02_tcp_state *out_state) {
    turbowasm_wasi02_tcp_slot *slot;
    turbowasm_status status;

    if (out_state == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = tcp_slot_get(sockets, socket_resource, &slot);
    if (status != TURBOWASM_OK)
        return status;
    *out_state = slot->state;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_tcp_mark_closed(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource) {
    turbowasm_wasi02_tcp_slot *slot;
    turbowasm_status status;

    status = tcp_slot_get(sockets, socket_resource, &slot);
    if (status != TURBOWASM_OK)
        return status;
    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return TURBOWASM_OK;
    if (slot->state != TURBOWASM_WASI02_TCP_CONNECTED)
        return TURBOWASM_INVALID_ARGUMENT;
    slot->state = TURBOWASM_WASI02_TCP_CLOSED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_network_drop(
    turbowasm_wasi02_sockets *sockets,
    uint32_t network_resource) {
    turbowasm_value rep = {0};
    turbowasm_status status;

    status = network_rep_get(sockets, network_resource, &rep);
    if (status != TURBOWASM_OK)
        return status;
    status = sockets->provider.network_drop(
        sockets->provider.context, rep);
    if (status != TURBOWASM_OK)
        return status;
    return turbowasm_component_resource_drop(
        &sockets->networks,
        network_resource,
        TW_WASI02_NETWORK_ID,
        NULL,
        NULL);
}

turbowasm_status turbowasm_wasi02_tcp_drop(
    turbowasm_wasi02_sockets *sockets,
    uint32_t socket_resource) {
    turbowasm_wasi02_tcp_slot *slot;
    turbowasm_status status;

    status = tcp_slot_get(sockets, socket_resource, &slot);
    if (status != TURBOWASM_OK)
        return status;
    status = sockets->provider.tcp_drop(
        sockets->provider.context, slot->provider_rep);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_component_resource_drop(
        &sockets->tcp_resources,
        socket_resource,
        TW_WASI02_TCP_SOCKET_ID,
        NULL,
        NULL);
    if (status != TURBOWASM_OK)
        return status;
    release_tcp_slot(sockets, slot);
    return TURBOWASM_OK;
}

static turbowasm_status call_instance_network(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_value provider_rep = {0};
    uint32_t resource = 0u;
    turbowasm_status status;

    if (argument_count != 0u || arguments != NULL)
        return TURBOWASM_TYPE_MISMATCH;

    status = sockets->provider.instance_network(
        sockets->provider.context, &provider_rep);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_component_resource_new_owned(
        &sockets->networks,
        TW_WASI02_NETWORK_ID,
        provider_rep,
        &resource);
    if (status != TURBOWASM_OK) {
        (void)sockets->provider.network_drop(
            sockets->provider.context, provider_rep);
        return status;
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    out->as.resource = resource;
    return TURBOWASM_OK;
}

static turbowasm_status call_create_tcp(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_ip_address_family family;
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_value provider_rep = {0};
    uint32_t resource = 0u;
    turbowasm_status status;

    if (arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_WASI02_VALUE_ENUM ||
        arguments[0].as.enum_index > 1u)
        return TURBOWASM_TYPE_MISMATCH;

    family = arguments[0].as.enum_index == 0u
        ? TURBOWASM_WASI02_IP_ADDRESS_IPV4
        : TURBOWASM_WASI02_IP_ADDRESS_IPV6;
    status = sockets->provider.tcp_create(
        sockets->provider.context,
        family,
        &provider_rep,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return result_error(out, error);

    status = tcp_resource_new(
        sockets,
        provider_rep,
        family,
        TURBOWASM_WASI02_TCP_UNBOUND,
        &resource);
    if (status != TURBOWASM_OK) {
        (void)sockets->provider.tcp_drop(
            sockets->provider.context, provider_rep);
        return status;
    }
    return result_resource_ok(out, resource);
}

static turbowasm_status get_self(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_tcp_slot **out_slot,
    uint32_t *out_resource) {
    turbowasm_status status;

    if (arguments == NULL || argument_count == 0u ||
        arguments[0].kind != TURBOWASM_WASI02_VALUE_RESOURCE ||
        out_slot == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    status = tcp_slot_get(
        sockets, arguments[0].as.resource, out_slot);
    if (status != TURBOWASM_OK)
        return status;
    if (out_resource != NULL)
        *out_resource = arguments[0].as.resource;
    return TURBOWASM_OK;
}

static turbowasm_status call_start_bind(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_ip_socket_address address;
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_value network_rep = {0};
    turbowasm_status status;

    if (argument_count != 3u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_RESOURCE)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state != TURBOWASM_WASI02_TCP_UNBOUND)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);

    status = address_from_value(&arguments[2], &address);
    if (status != TURBOWASM_OK)
        return status;
    if (address.family != slot->family)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    status = network_rep_get(
        sockets, arguments[1].as.resource, &network_rep);
    if (status != TURBOWASM_OK)
        return status;
    if (sockets->provider.tcp_start_bind == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_start_bind(
        sockets->provider.context,
        slot->provider_rep,
        network_rep,
        &address,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return result_error(out, error);

    slot->state = TURBOWASM_WASI02_TCP_BIND_IN_PROGRESS;
    return result_unit_ok(out);
}

static turbowasm_status call_finish_bind(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (slot->state != TURBOWASM_WASI02_TCP_BIND_IN_PROGRESS)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS);
    if (sockets->provider.tcp_finish_bind == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_finish_bind(
        sockets->provider.context, slot->provider_rep, &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error == TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK)
        return result_error(out, error);
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        slot->state = TURBOWASM_WASI02_TCP_UNBOUND;
        return result_error(out, error);
    }

    slot->state = TURBOWASM_WASI02_TCP_BOUND;
    return result_unit_ok(out);
}

static turbowasm_status call_start_connect(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_ip_socket_address address;
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_value network_rep = {0};
    turbowasm_status status;

    if (argument_count != 3u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_RESOURCE)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state != TURBOWASM_WASI02_TCP_UNBOUND &&
        slot->state != TURBOWASM_WASI02_TCP_BOUND)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);

    status = address_from_value(&arguments[2], &address);
    if (status != TURBOWASM_OK)
        return status;
    if (address.family != slot->family) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    }
    status = network_rep_get(
        sockets, arguments[1].as.resource, &network_rep);
    if (status != TURBOWASM_OK)
        return status;
    if (sockets->provider.tcp_start_connect == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_start_connect(
        sockets->provider.context,
        slot->provider_rep,
        network_rep,
        &address,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return result_error(out, error);
    }

    slot->state = TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS;
    return result_unit_ok(out);
}

static turbowasm_status call_finish_connect(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_value input_rep = {0};
    turbowasm_value output_rep = {0};
    uint32_t resources[2] = {0u, 0u};
    turbowasm_status status;

    if (slot->state != TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS);
    if (sockets->provider.tcp_finish_connect == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (sockets->streams == NULL ||
        sockets->streams->provider.input_drop == NULL ||
        sockets->streams->provider.output_drop == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_finish_connect(
        sockets->provider.context,
        slot->provider_rep,
        &input_rep,
        &output_rep,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error == TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK)
        return result_error(out, error);
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return result_error(out, error);
    }

    status = wrap_stream_pair(
        sockets, input_rep, output_rep,
        &resources[0], &resources[1]);
    if (status != TURBOWASM_OK) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return status;
    }

    status = make_result_tuple_resources(resources, 2u, out);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi02_stream_resource_drop(
            sockets->streams, resources[0]);
        (void)turbowasm_wasi02_stream_resource_drop(
            sockets->streams, resources[1]);
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return status;
    }
    slot->state = TURBOWASM_WASI02_TCP_CONNECTED;
    return TURBOWASM_OK;
}

static turbowasm_status call_start_listen(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (slot->state != TURBOWASM_WASI02_TCP_BOUND)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (sockets->provider.tcp_start_listen == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_start_listen(
        sockets->provider.context, slot->provider_rep, &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return result_error(out, error);
    }

    slot->state = TURBOWASM_WASI02_TCP_LISTEN_IN_PROGRESS;
    return result_unit_ok(out);
}

static turbowasm_status call_finish_listen(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (slot->state != TURBOWASM_WASI02_TCP_LISTEN_IN_PROGRESS)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS);
    if (sockets->provider.tcp_finish_listen == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_finish_listen(
        sockets->provider.context, slot->provider_rep, &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error == TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK)
        return result_error(out, error);
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE) {
        slot->state = TURBOWASM_WASI02_TCP_CLOSED;
        return result_error(out, error);
    }

    slot->state = TURBOWASM_WASI02_TCP_LISTENING;
    return result_unit_ok(out);
}

static turbowasm_status call_accept(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *listener,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_value child_rep = {0};
    turbowasm_value input_rep = {0};
    turbowasm_value output_rep = {0};
    uint32_t resources[3] = {0u, 0u, 0u};
    turbowasm_status status;

    if (listener->state != TURBOWASM_WASI02_TCP_LISTENING)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (sockets->provider.tcp_accept == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (sockets->streams == NULL ||
        sockets->streams->provider.input_drop == NULL ||
        sockets->streams->provider.output_drop == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_accept(
        sockets->provider.context,
        listener->provider_rep,
        &child_rep,
        &input_rep,
        &output_rep,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return result_error(out, error);

    status = tcp_resource_new(
        sockets,
        child_rep,
        listener->family,
        TURBOWASM_WASI02_TCP_CONNECTED,
        &resources[0]);
    if (status != TURBOWASM_OK) {
        (void)sockets->provider.tcp_drop(
            sockets->provider.context, child_rep);
        (void)stream_reps_drop(sockets, input_rep, output_rep);
        return status;
    }

    status = wrap_stream_pair(
        sockets, input_rep, output_rep,
        &resources[1], &resources[2]);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi02_tcp_drop(
            sockets, resources[0]);
        return status;
    }

    status = make_result_tuple_resources(resources, 3u, out);
    if (status != TURBOWASM_OK) {
        (void)turbowasm_wasi02_tcp_drop(
            sockets, resources[0]);
        (void)turbowasm_wasi02_stream_resource_drop(
            sockets->streams, resources[1]);
        (void)turbowasm_wasi02_stream_resource_drop(
            sockets->streams, resources[2]);
        return status;
    }
    return TURBOWASM_OK;
}

static bool local_address_state(
    turbowasm_wasi02_tcp_state state) {
    return state == TURBOWASM_WASI02_TCP_BOUND ||
           state == TURBOWASM_WASI02_TCP_LISTEN_IN_PROGRESS ||
           state == TURBOWASM_WASI02_TCP_LISTENING ||
           state == TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS ||
           state == TURBOWASM_WASI02_TCP_CONNECTED;
}

static turbowasm_status call_get_address(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    bool remote,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_wasi02_ip_socket_address address;
    turbowasm_wasi02_tcp_get_address_fn fn =
        remote
            ? sockets->provider.tcp_remote_address
            : sockets->provider.tcp_local_address;
    turbowasm_status status;

    if ((remote &&
         slot->state != TURBOWASM_WASI02_TCP_CONNECTED) ||
        (!remote && !local_address_state(slot->state)))
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;

    memset(&address, 0, sizeof(address));
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        &address,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (!socket_error_valid(error))
        return TURBOWASM_TRAPPED;
    if (error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return result_error(out, error);
    if (!family_valid(address.family) ||
        address.family != slot->family)
        return TURBOWASM_TRAPPED;
    return result_address_ok(out, &address);
}

static turbowasm_status call_get_bool(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_get_bool_fn fn,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    bool value = false;
    turbowasm_status status;

    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        &value,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_bool_ok(out, value);
}

static turbowasm_status call_set_bool(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_set_bool_fn fn,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (argument_count != 2u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_BOOL)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        arguments[1].as.boolean,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_unit_ok(out);
}

static turbowasm_status call_get_u8(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_get_u8_fn fn,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    uint8_t value = 0u;
    turbowasm_status status;

    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        &value,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_u8_ok(out, value);
}

static turbowasm_status call_get_u32(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_get_u32_fn fn,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    uint32_t value = 0u;
    turbowasm_status status;

    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        &value,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_u32_ok(out, value);
}

static turbowasm_status call_get_u64(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_get_u64_fn fn,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    uint64_t value = 0u;
    turbowasm_status status;

    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        &value,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_u64_ok(out, value);
}

static turbowasm_status call_set_u8(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_set_u8_fn fn,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (argument_count != 2u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_U8)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (arguments[1].as.u8 == 0u)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        arguments[1].as.u8,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_unit_ok(out);
}

static turbowasm_status call_set_u32(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_set_u32_fn fn,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (argument_count != 2u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_U32)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (arguments[1].as.u32 == 0u)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        arguments[1].as.u32,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_unit_ok(out);
}

static turbowasm_status call_set_u64(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_tcp_set_u64_fn fn,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    bool reject_connect_states,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_status status;

    if (argument_count != 2u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_U64)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state == TURBOWASM_WASI02_TCP_CLOSED ||
        (reject_connect_states &&
         (slot->state == TURBOWASM_WASI02_TCP_CONNECT_IN_PROGRESS ||
          slot->state == TURBOWASM_WASI02_TCP_CONNECTED)))
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (arguments[1].as.u64 == 0u)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT);
    if (fn == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = fn(
        sockets->provider.context,
        slot->provider_rep,
        arguments[1].as.u64,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_unit_ok(out);
}

static turbowasm_status call_subscribe(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    turbowasm_wasi02_value *out) {
    turbowasm_value rep = {0};
    uint32_t resource = 0u;
    turbowasm_status status;

    if (sockets->provider.tcp_subscribe == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (sockets->poll == NULL ||
        sockets->poll->provider.drop == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = sockets->provider.tcp_subscribe(
        sockets->provider.context,
        slot->provider_rep,
        &rep);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_wasi02_pollable_new(
        sockets->poll, rep, &resource);
    if (status != TURBOWASM_OK) {
        (void)sockets->poll->provider.drop(
            sockets->poll->provider.context, rep);
        return status;
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    out->as.resource = resource;
    return TURBOWASM_OK;
}

static turbowasm_status call_shutdown(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_wasi02_tcp_slot *slot,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_socket_error error =
        TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    turbowasm_wasi02_tcp_shutdown_type how;
    turbowasm_status status;

    if (argument_count != 2u ||
        arguments[1].kind != TURBOWASM_WASI02_VALUE_ENUM ||
        arguments[1].as.enum_index > 2u)
        return TURBOWASM_TYPE_MISMATCH;
    if (slot->state != TURBOWASM_WASI02_TCP_CONNECTED)
        return result_error(
            out, TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
    if (sockets->provider.tcp_shutdown == NULL)
        return TURBOWASM_UNSUPPORTED;

    how = (turbowasm_wasi02_tcp_shutdown_type)
        arguments[1].as.enum_index;
    status = sockets->provider.tcp_shutdown(
        sockets->provider.context,
        slot->provider_rep,
        how,
        &error);
    status = semantic_result(status, error, out);
    if (status != TURBOWASM_OK ||
        error != TURBOWASM_WASI02_SOCKET_ERROR_NONE)
        return status;
    return result_unit_ok(out);
}

static turbowasm_status call_tcp(
    turbowasm_wasi02_sockets *sockets,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_tcp_slot *slot;
    turbowasm_status status;

    status = get_self(
        sockets, arguments, argument_count, &slot, NULL);
    if (status != TURBOWASM_OK)
        return status;

    if (strcmp(function_name, "[method]tcp-socket.start-bind") == 0)
        return call_start_bind(
            sockets, slot, arguments, argument_count, out);
    if (strcmp(function_name, "[method]tcp-socket.finish-bind") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_finish_bind(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.start-connect") == 0)
        return call_start_connect(
            sockets, slot, arguments, argument_count, out);
    if (strcmp(function_name, "[method]tcp-socket.finish-connect") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_finish_connect(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.start-listen") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_start_listen(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.finish-listen") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_finish_listen(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.accept") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_accept(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.local-address") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_address(sockets, slot, false, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.remote-address") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_address(sockets, slot, true, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.is-listening") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        memset(out, 0, sizeof(*out));
        out->kind = TURBOWASM_WASI02_VALUE_BOOL;
        out->as.boolean =
            slot->state == TURBOWASM_WASI02_TCP_LISTENING;
        return TURBOWASM_OK;
    }
    if (strcmp(function_name, "[method]tcp-socket.address-family") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        memset(out, 0, sizeof(*out));
        out->kind = TURBOWASM_WASI02_VALUE_ENUM;
        out->as.enum_index = (uint32_t)slot->family;
        return TURBOWASM_OK;
    }
    if (strcmp(function_name, "[method]tcp-socket.set-listen-backlog-size") == 0)
        return call_set_u64(
            sockets, slot,
            sockets->provider.tcp_set_listen_backlog_size,
            arguments, argument_count, true, out);
    if (strcmp(function_name, "[method]tcp-socket.keep-alive-enabled") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_bool(
            sockets, slot,
            sockets->provider.tcp_keep_alive_enabled, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-keep-alive-enabled") == 0)
        return call_set_bool(
            sockets, slot,
            sockets->provider.tcp_set_keep_alive_enabled,
            arguments, argument_count, out);
    if (strcmp(function_name, "[method]tcp-socket.keep-alive-idle-time") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u64(
            sockets, slot,
            sockets->provider.tcp_keep_alive_idle_time, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-keep-alive-idle-time") == 0)
        return call_set_u64(
            sockets, slot,
            sockets->provider.tcp_set_keep_alive_idle_time,
            arguments, argument_count, false, out);
    if (strcmp(function_name, "[method]tcp-socket.keep-alive-interval") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u64(
            sockets, slot,
            sockets->provider.tcp_keep_alive_interval, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-keep-alive-interval") == 0)
        return call_set_u64(
            sockets, slot,
            sockets->provider.tcp_set_keep_alive_interval,
            arguments, argument_count, false, out);
    if (strcmp(function_name, "[method]tcp-socket.keep-alive-count") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u32(
            sockets, slot,
            sockets->provider.tcp_keep_alive_count, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-keep-alive-count") == 0)
        return call_set_u32(
            sockets, slot,
            sockets->provider.tcp_set_keep_alive_count,
            arguments, argument_count, out);
    if (strcmp(function_name, "[method]tcp-socket.hop-limit") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u8(
            sockets, slot,
            sockets->provider.tcp_hop_limit, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-hop-limit") == 0)
        return call_set_u8(
            sockets, slot,
            sockets->provider.tcp_set_hop_limit,
            arguments, argument_count, out);
    if (strcmp(function_name, "[method]tcp-socket.receive-buffer-size") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u64(
            sockets, slot,
            sockets->provider.tcp_receive_buffer_size, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-receive-buffer-size") == 0)
        return call_set_u64(
            sockets, slot,
            sockets->provider.tcp_set_receive_buffer_size,
            arguments, argument_count, false, out);
    if (strcmp(function_name, "[method]tcp-socket.send-buffer-size") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_get_u64(
            sockets, slot,
            sockets->provider.tcp_send_buffer_size, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.set-send-buffer-size") == 0)
        return call_set_u64(
            sockets, slot,
            sockets->provider.tcp_set_send_buffer_size,
            arguments, argument_count, false, out);
    if (strcmp(function_name, "[method]tcp-socket.subscribe") == 0) {
        if (argument_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return call_subscribe(sockets, slot, out);
    }
    if (strcmp(function_name, "[method]tcp-socket.shutdown") == 0)
        return call_shutdown(
            sockets, slot, arguments, argument_count, out);

    return TURBOWASM_UNSUPPORTED;
}

turbowasm_status turbowasm_wasi02_sockets_call(
    turbowasm_wasi02_sockets *sockets,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    if (sockets == NULL || !sockets->initialized ||
        interface_name == NULL || function_name == NULL ||
        out_result == NULL ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_result, 0, sizeof(*out_result));

    if (strcmp(interface_name, "instance-network") == 0 &&
        strcmp(function_name, "instance-network") == 0)
        return call_instance_network(
            sockets, arguments, argument_count, out_result);

    if (strcmp(interface_name, "tcp-create-socket") == 0 &&
        strcmp(function_name, "create-tcp-socket") == 0)
        return call_create_tcp(
            sockets, arguments, argument_count, out_result);

    if (strcmp(interface_name, "tcp") == 0)
        return call_tcp(
            sockets, function_name,
            arguments, argument_count, out_result);

    return TURBOWASM_UNSUPPORTED;
}


static bool socket_component_name_is(
    turbowasm_component_name name,
    const char *text) {
    size_t size;

    if (text == NULL)
        return false;
    size = strlen(text);
    return name.size == size &&
           (size == 0u ||
            (name.bytes != NULL &&
             memcmp(name.bytes, text, size) == 0));
}

static const turbowasm_wasi02_type_desc *socket_wasi_type_base(
    const turbowasm_wasi02_type_desc *type) {
    uint32_t depth = 0u;

    while (type != NULL &&
           type->kind == TURBOWASM_WASI02_TYPE_ALIAS) {
        if (++depth > 32u)
            return NULL;
        type = type->as.alias.target;
    }
    return type;
}

static bool socket_wasi_type_contains_resource(
    const turbowasm_wasi02_type_desc *type,
    uint32_t depth) {
    const turbowasm_wasi02_type_desc *base;
    uint32_t i;

    if (depth > 64u)
        return true;
    base = socket_wasi_type_base(type);
    if (base == NULL)
        return true;

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_RESOURCE:
            return true;
        case TURBOWASM_WASI02_TYPE_LIST:
            return socket_wasi_type_contains_resource(
                base->as.list.element, depth + 1u);
        case TURBOWASM_WASI02_TYPE_TUPLE:
            for (i = 0u; i < base->as.tuple.count; ++i) {
                if (socket_wasi_type_contains_resource(
                        base->as.tuple.elements[i], depth + 1u))
                    return true;
            }
            return false;
        case TURBOWASM_WASI02_TYPE_RECORD:
            for (i = 0u; i < base->as.record.count; ++i) {
                if (socket_wasi_type_contains_resource(
                        base->as.record.fields[i].type, depth + 1u))
                    return true;
            }
            return false;
        case TURBOWASM_WASI02_TYPE_OPTION:
            return socket_wasi_type_contains_resource(
                base->as.option.payload, depth + 1u);
        case TURBOWASM_WASI02_TYPE_RESULT:
            return (base->as.result.ok != NULL &&
                    socket_wasi_type_contains_resource(
                        base->as.result.ok, depth + 1u)) ||
                   (base->as.result.error != NULL &&
                    socket_wasi_type_contains_resource(
                        base->as.result.error, depth + 1u));
        case TURBOWASM_WASI02_TYPE_VARIANT:
            for (i = 0u; i < base->as.variant.count; ++i) {
                if (base->as.variant.cases[i].payload != NULL &&
                    socket_wasi_type_contains_resource(
                        base->as.variant.cases[i].payload,
                        depth + 1u))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static const turbowasm_component_type *socket_component_type_from_ref(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type *inline_storage) {
    if (graph == NULL || inline_storage == NULL)
        return NULL;
    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        memset(inline_storage, 0, sizeof(*inline_storage));
        inline_storage->kind = ref.as.inline_type;
        return inline_storage;
    }
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return NULL;
    return turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
}

static bool socket_bind_identity(
    uint64_t *identity,
    bool *bound,
    uint64_t candidate) {
    if (identity == NULL || bound == NULL ||
        candidate == 0u)
        return false;
    if (*bound && *identity != candidate)
        return false;
    *identity = candidate;
    *bound = true;
    return true;
}

static bool socket_bind_resource_type(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_wasi02_type_desc *wasi_type,
    turbowasm_component_type_kind expected_handle_kind) {
    const turbowasm_wasi02_type_desc *base =
        socket_wasi_type_base(wasi_type);
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;
    uint64_t identity;

    if (sockets == NULL || graph == NULL || base == NULL ||
        base->kind != TURBOWASM_WASI02_TYPE_RESOURCE ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (handle_type == NULL ||
        handle_type->kind != expected_handle_kind)
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity == 0u)
        return false;
    identity = resource_type->as.resource.identity;

    if (strcmp(base->as.resource.package_name, "wasi:sockets") == 0 &&
        strcmp(base->as.resource.interface_name, "network") == 0 &&
        strcmp(base->as.resource.resource_name, "network") == 0)
        return socket_bind_identity(
            &sockets->component_network_identity,
            &sockets->component_network_identity_bound,
            identity);

    if (strcmp(base->as.resource.package_name, "wasi:sockets") == 0 &&
        strcmp(base->as.resource.interface_name, "tcp") == 0 &&
        strcmp(base->as.resource.resource_name, "tcp-socket") == 0)
        return socket_bind_identity(
            &sockets->component_tcp_identity,
            &sockets->component_tcp_identity_bound,
            identity);

    if (strcmp(base->as.resource.package_name, "wasi:io") == 0 &&
        strcmp(base->as.resource.interface_name, "streams") == 0 &&
        strcmp(base->as.resource.resource_name, "input-stream") == 0) {
        if (sockets->streams == NULL ||
            !sockets->streams->initialized)
            return false;
        return socket_bind_identity(
            &sockets->streams->component_input_identity,
            &sockets->streams->component_input_identity_bound,
            identity);
    }

    if (strcmp(base->as.resource.package_name, "wasi:io") == 0 &&
        strcmp(base->as.resource.interface_name, "streams") == 0 &&
        strcmp(base->as.resource.resource_name, "output-stream") == 0) {
        if (sockets->streams == NULL ||
            !sockets->streams->initialized)
            return false;
        return socket_bind_identity(
            &sockets->streams->component_output_identity,
            &sockets->streams->component_output_identity_bound,
            identity);
    }

    if (strcmp(base->as.resource.package_name, "wasi:io") == 0 &&
        strcmp(base->as.resource.interface_name, "poll") == 0 &&
        strcmp(base->as.resource.resource_name, "pollable") == 0) {
        if (sockets->poll == NULL ||
            !sockets->poll->initialized)
            return false;
        return socket_bind_identity(
            &sockets->poll->pollable_identity,
            &sockets->poll->pollable_identity_bound,
            identity);
    }

    return false;
}

static bool socket_component_type_matches(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_wasi02_type_desc *wasi_type,
    turbowasm_component_type_kind resource_handle_kind,
    uint32_t depth) {
    turbowasm_component_type inline_storage;
    const turbowasm_component_type *type;
    const turbowasm_wasi02_type_desc *base;
    uint32_t i;

    if (depth > 64u || graph == NULL)
        return false;

    base = socket_wasi_type_base(wasi_type);
    if (base == NULL)
        return false;
    if (!socket_wasi_type_contains_resource(base, 0u))
        return turbowasm_wasi02_component_type_matches(
            graph, ref, base);

    type = socket_component_type_from_ref(
        graph, ref, &inline_storage);
    if (type == NULL)
        return false;

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_RESOURCE:
            return socket_bind_resource_type(
                sockets, graph, ref, base,
                resource_handle_kind);

        case TURBOWASM_WASI02_TYPE_LIST:
            return type->kind == TURBOWASM_COMPONENT_TYPE_LIST &&
                   socket_component_type_matches(
                       sockets, graph,
                       type->as.list.element_type,
                       base->as.list.element,
                       resource_handle_kind,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_TUPLE ||
                type->as.tuple.count != base->as.tuple.count)
                return false;
            for (i = 0u; i < base->as.tuple.count; ++i) {
                if (!socket_component_type_matches(
                        sockets, graph,
                        type->as.tuple.elements[i],
                        base->as.tuple.elements[i],
                        resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_RECORD:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_RECORD ||
                type->as.record.count != base->as.record.count)
                return false;
            for (i = 0u; i < base->as.record.count; ++i) {
                const turbowasm_component_record_field *field =
                    &type->as.record.fields[i];
                const turbowasm_wasi02_record_field *wasi_field =
                    &base->as.record.fields[i];
                size_t name_size;

                if (wasi_field->name == NULL)
                    return false;
                name_size = strlen(wasi_field->name);
                if (field->name == NULL ||
                    field->name_size != name_size ||
                    memcmp(field->name, wasi_field->name, name_size) != 0 ||
                    !socket_component_type_matches(
                        sockets, graph,
                        field->type,
                        wasi_field->type,
                        resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_OPTION:
            return type->kind == TURBOWASM_COMPONENT_TYPE_OPTION &&
                   socket_component_type_matches(
                       sockets, graph,
                       type->as.option.payload,
                       base->as.option.payload,
                       resource_handle_kind,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_RESULT:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_RESULT ||
                type->as.result.has_ok !=
                    (base->as.result.ok != NULL) ||
                type->as.result.has_error !=
                    (base->as.result.error != NULL))
                return false;
            if (type->as.result.has_ok &&
                !socket_component_type_matches(
                    sockets, graph,
                    type->as.result.ok,
                    base->as.result.ok,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            if (type->as.result.has_error &&
                !socket_component_type_matches(
                    sockets, graph,
                    type->as.result.error,
                    base->as.result.error,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            return true;

        case TURBOWASM_WASI02_TYPE_VARIANT:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_VARIANT ||
                type->as.variant.cases == NULL ||
                base->as.variant.cases == NULL ||
                type->as.variant.count != base->as.variant.count)
                return false;
            for (i = 0u; i < base->as.variant.count; ++i) {
                const turbowasm_component_variant_case *ccase =
                    &type->as.variant.cases[i];
                const turbowasm_wasi02_variant_case *wcase =
                    &base->as.variant.cases[i];
                size_t name_size;

                if (wcase->name == NULL)
                    return false;
                name_size = strlen(wcase->name);
                if (ccase->name == NULL ||
                    ccase->name_size != name_size ||
                    memcmp(ccase->name, wcase->name, name_size) != 0 ||
                    ccase->has_payload !=
                        (wcase->payload != NULL))
                    return false;
                if (ccase->has_payload &&
                    !socket_component_type_matches(
                        sockets, graph,
                        ccase->payload,
                        wcase->payload,
                        resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        default:
            return false;
    }
}

static const turbowasm_wasi02_interface_desc *
socket_interface_by_component_name(
    turbowasm_component_name name) {
    if (socket_component_name_is(
            name, "wasi:sockets/network@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:sockets", "network");
    if (socket_component_name_is(
            name, "wasi:sockets/tcp@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:sockets", "tcp");
    if (socket_component_name_is(
            name, "wasi:sockets/tcp-create-socket@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:sockets", "tcp-create-socket");
    if (socket_component_name_is(
            name, "wasi:sockets/instance-network@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:sockets", "instance-network");
    return NULL;
}

static const turbowasm_wasi02_function_desc *
socket_function_by_component_name(
    const turbowasm_wasi02_interface_desc *iface,
    turbowasm_component_name name) {
    uint32_t i;

    if (iface == NULL ||
        (name.size != 0u && name.bytes == NULL))
        return NULL;

    for (i = 0u; i < iface->function_count; ++i) {
        const turbowasm_wasi02_function_desc *function =
            &iface->functions[i];
        size_t size = strlen(function->name);
        if (size == name.size &&
            (size == 0u ||
             memcmp(name.bytes, function->name, size) == 0))
            return function;
    }
    return NULL;
}

static bool socket_binding_matches_descriptor(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_wasi02_function_desc *function) {
    const turbowasm_component_type *function_type;
    uint32_t i;

    if (sockets == NULL || graph == NULL ||
        function == NULL)
        return false;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count !=
            function->param_count ||
        function_type->as.function.has_result !=
            (function->result != NULL))
        return false;

    for (i = 0u; i < function->param_count; ++i) {
        if (!socket_component_type_matches(
                sockets, graph,
                function_type->as.function.params[i],
                function->params[i].type,
                TURBOWASM_COMPONENT_TYPE_BORROW,
                0u))
            return false;
    }

    if (function->result != NULL &&
        !socket_component_type_matches(
            sockets, graph,
            function_type->as.function.result,
            function->result,
            TURBOWASM_COMPONENT_TYPE_OWN,
            0u))
        return false;

    return true;
}

static bool wasi02_sockets_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_sockets *sockets =
        (turbowasm_wasi02_sockets *)context;
    const turbowasm_wasi02_interface_desc *iface =
        socket_interface_by_component_name(instance_name);
    const turbowasm_wasi02_function_desc *function =
        socket_function_by_component_name(
            iface, function_name);

    return sockets != NULL && sockets->initialized &&
           socket_binding_matches_descriptor(
               sockets, graph, function_type, function);
}

typedef enum socket_component_resource_kind {
    SOCKET_COMPONENT_RESOURCE_NONE = 0,
    SOCKET_COMPONENT_RESOURCE_NETWORK,
    SOCKET_COMPONENT_RESOURCE_TCP
} socket_component_resource_kind;

static socket_component_resource_kind
socket_component_identity_kind(
    const turbowasm_wasi02_sockets *sockets,
    uint64_t identity) {
    if (sockets == NULL || identity == 0u)
        return SOCKET_COMPONENT_RESOURCE_NONE;
    if (sockets->component_network_identity_bound &&
        sockets->component_network_identity == identity)
        return SOCKET_COMPONENT_RESOURCE_NETWORK;
    if (sockets->component_tcp_identity_bound &&
        sockets->component_tcp_identity == identity)
        return SOCKET_COMPONENT_RESOURCE_TCP;
    return SOCKET_COMPONENT_RESOURCE_NONE;
}

static bool socket_imported_resource_identity(
    turbowasm_wasi02_sockets *sockets,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type_ref,
    turbowasm_component_type_kind *out_handle_kind,
    socket_component_resource_kind *out_resource_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;
    socket_component_resource_kind kind;

    if (sockets == NULL || graph == NULL ||
        type_ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, type_ref.as.indexed);
    if (handle_type == NULL ||
        (handle_type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
         handle_type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
        return false;

    kind = socket_component_identity_kind(
        sockets, resource_type->as.resource.identity);
    if (kind == SOCKET_COMPONENT_RESOURCE_NONE)
        return false;

    if (out_handle_kind != NULL)
        *out_handle_kind = handle_type->kind;
    if (out_resource_kind != NULL)
        *out_resource_kind = kind;
    return true;
}

static turbowasm_status wasi02_sockets_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_wasi02_sockets *sockets =
        (turbowasm_wasi02_sockets *)context;
    turbowasm_component_type_kind handle_kind;
    socket_component_resource_kind resource_kind;
    uint32_t handle;
    turbowasm_value rep = {0};
    turbowasm_wasi02_tcp_slot *slot;

    if (value == NULL || out_handle == NULL ||
        !socket_imported_resource_identity(
            sockets, graph, type,
            &handle_kind, &resource_kind) ||
        value->kind != handle_kind ||
        value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle = (uint32_t)value->as.resource_rep.as.i32;
    if (resource_kind == SOCKET_COMPONENT_RESOURCE_NETWORK) {
        if (turbowasm_component_resource_rep(
                &sockets->networks,
                handle,
                TW_WASI02_NETWORK_ID,
                &rep) != TURBOWASM_OK)
            return TURBOWASM_TRAPPED;
    } else if (tcp_slot_get(
                   sockets, handle, &slot) != TURBOWASM_OK) {
        return TURBOWASM_TRAPPED;
    }

    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_sockets_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_wasi02_sockets *sockets =
        (turbowasm_wasi02_sockets *)context;
    turbowasm_component_type_kind handle_kind;
    socket_component_resource_kind resource_kind;
    turbowasm_value rep = {0};
    turbowasm_wasi02_tcp_slot *slot;

    if (out == NULL ||
        !socket_imported_resource_identity(
            sockets, graph, type,
            &handle_kind, &resource_kind))
        return TURBOWASM_TYPE_MISMATCH;

    if (resource_kind == SOCKET_COMPONENT_RESOURCE_NETWORK) {
        if (turbowasm_component_resource_rep(
                &sockets->networks,
                handle,
                TW_WASI02_NETWORK_ID,
                &rep) != TURBOWASM_OK)
            return TURBOWASM_TRAPPED;
    } else if (tcp_slot_get(
                   sockets, handle, &slot) != TURBOWASM_OK) {
        return TURBOWASM_TRAPPED;
    }

    memset(out, 0, sizeof(*out));
    out->kind = handle_kind;
    out->as.resource_rep.kind = TURBOWASM_VALUE_I32;
    out->as.resource_rep.as.i32 = (int32_t)handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_sockets_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    turbowasm_wasi02_sockets *sockets =
        (turbowasm_wasi02_sockets *)context;
    socket_component_resource_kind kind =
        socket_component_identity_kind(
            sockets, resource_identity);

    if (kind == SOCKET_COMPONENT_RESOURCE_NETWORK)
        return turbowasm_wasi02_network_drop(
            sockets, handle);
    if (kind == SOCKET_COMPONENT_RESOURCE_TCP)
        return turbowasm_wasi02_tcp_drop(
            sockets, handle);
    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status wasi02_sockets_invoke(
    void *context,
    turbowasm_host_call *call,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    turbowasm_wasi02_sockets *sockets =
        (turbowasm_wasi02_sockets *)context;
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;
    turbowasm_wasi02_value
        wasi_arguments[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {{0}};
    turbowasm_wasi02_value wasi_result = {0};
    size_t i;
    turbowasm_status status = TURBOWASM_OK;

    (void)call;

    if (sockets == NULL || !sockets->initialized ||
        graph == NULL || trap == NULL ||
        !wasi02_sockets_can_bind(
            context,
            instance_name,
            function_name,
            graph,
            function_type))
        return TURBOWASM_TYPE_MISMATCH;

    iface = socket_interface_by_component_name(
        instance_name);
    function = socket_function_by_component_name(
        iface, function_name);
    if (iface == NULL || function == NULL ||
        argument_count != function->param_count ||
        argument_count >
            TURBOWASM_COMPONENT_MAX_FLAT_PARAMS ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;

    for (i = 0u; i < argument_count; ++i) {
        status =
            turbowasm_wasi02_component_value_to_wasi(
                function->params[i].type,
                &arguments[i],
                &wasi_arguments[i]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_wasi02_sockets_call(
        sockets,
        iface->interface_name,
        function->name,
        wasi_arguments,
        argument_count,
        &wasi_result);
    if (status != TURBOWASM_OK)
        goto done;

    if (function->result != NULL) {
        if (out_result == NULL) {
            status = TURBOWASM_INVALID_ARGUMENT;
            goto done;
        }
        status =
            turbowasm_wasi02_component_value_from_wasi(
                function->result,
                &wasi_result,
                out_result);
    }

done:
    for (i = 0u; i < argument_count; ++i)
        turbowasm_wasi02_value_destroy(
            &wasi_arguments[i]);
    turbowasm_wasi02_value_destroy(&wasi_result);
    return status;
}

turbowasm_status turbowasm_wasi02_sockets_imports(
    turbowasm_wasi02_sockets *sockets,
    turbowasm_component_exec_imports *out_imports) {
    if (sockets == NULL || !sockets->initialized ||
        out_imports == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_imports, 0, sizeof(*out_imports));
    out_imports->context = sockets;
    out_imports->can_bind = wasi02_sockets_can_bind;
    out_imports->invoke = wasi02_sockets_invoke;
    out_imports->resource_lower =
        wasi02_sockets_resource_lower;
    out_imports->resource_lift =
        wasi02_sockets_resource_lift;
    out_imports->resource_drop =
        wasi02_sockets_resource_drop;
    return TURBOWASM_OK;
}
