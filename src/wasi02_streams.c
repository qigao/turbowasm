#include "wasi02_streams.h"

#include "runtime_alloc.h"
#include "wasi02_descriptor.h"

#include <limits.h>
#include <string.h>

#define TW_WASI02_INPUT_STREAM_ID UINT64_C(0x7761736932696e70)
#define TW_WASI02_OUTPUT_STREAM_ID UINT64_C(0x77617369326f7574)
#define TW_WASI02_IO_ERROR_ID UINT64_C(0x7761736932657272)

static uint64_t identity_for_kind(
    turbowasm_wasi02_stream_slot_kind kind) {
    switch (kind) {
        case TURBOWASM_WASI02_STREAM_SLOT_INPUT:
            return TW_WASI02_INPUT_STREAM_ID;
        case TURBOWASM_WASI02_STREAM_SLOT_OUTPUT:
            return TW_WASI02_OUTPUT_STREAM_ID;
        case TURBOWASM_WASI02_STREAM_SLOT_ERROR:
            return TW_WASI02_IO_ERROR_ID;
        default:
            return 0u;
    }
}

static turbowasm_wasi02_stream_slot *reserve_slot(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_stream_slot_kind kind,
    turbowasm_value provider_rep,
    uint32_t *out_index) {
    uint32_t index;
    turbowasm_wasi02_stream_slot *slot;

    if (streams == NULL || out_index == NULL ||
        streams->free_count == 0u ||
        kind == TURBOWASM_WASI02_STREAM_SLOT_NONE)
        return NULL;

    index = streams->free_indices[--streams->free_count];
    slot = &streams->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->kind = kind;
    slot->provider_rep = provider_rep;
    *out_index = index;
    return slot;
}

static void release_slot(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_stream_slot *slot) {
    uint32_t index;

    if (streams == NULL || slot == NULL || !slot->active)
        return;
    index = (uint32_t)(slot - streams->slots);
    memset(slot, 0, sizeof(*slot));
    streams->free_indices[streams->free_count++] = index;
}

static turbowasm_status new_resource(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_stream_slot_kind kind,
    turbowasm_value provider_rep,
    uint32_t *out_resource) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_value rep = {0};
    uint32_t index = 0u;
    uint64_t identity;
    turbowasm_status status;

    if (streams == NULL || !streams->initialized ||
        out_resource == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    identity = identity_for_kind(kind);
    if (identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    slot = reserve_slot(
        streams, kind, provider_rep, &index);
    if (slot == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    rep.kind = TURBOWASM_VALUE_I32;
    rep.as.i32 = (int32_t)(index + 1u);
    status = turbowasm_component_resource_new_owned(
        &streams->resources,
        identity,
        rep,
        out_resource);
    if (status != TURBOWASM_OK)
        release_slot(streams, slot);
    return status;
}

static turbowasm_status slot_from_resource(
    turbowasm_wasi02_streams *streams,
    uint32_t resource,
    turbowasm_wasi02_stream_slot_kind kind,
    turbowasm_wasi02_stream_slot **out_slot) {
    turbowasm_value rep = {0};
    uint64_t identity;
    uint32_t encoded;
    uint32_t index;
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_status status;

    if (streams == NULL || !streams->initialized ||
        out_slot == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    identity = identity_for_kind(kind);
    if (identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        &streams->resources,
        resource,
        identity,
        &rep);
    if (status != TURBOWASM_OK ||
        rep.kind != TURBOWASM_VALUE_I32 ||
        rep.as.i32 <= 0)
        return TURBOWASM_TRAPPED;

    encoded = (uint32_t)rep.as.i32;
    index = encoded - 1u;
    if (index >= streams->capacity)
        return TURBOWASM_TRAPPED;

    slot = &streams->slots[index];
    if (!slot->active || slot->kind != kind)
        return TURBOWASM_TRAPPED;

    *out_slot = slot;
    return TURBOWASM_OK;
}

static turbowasm_status clear_transient_pollable(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_stream_slot *slot) {
    turbowasm_status status;

    if (streams == NULL || slot == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!slot->transient_pollable_valid)
        return TURBOWASM_OK;
    if (streams->poll == NULL)
        return TURBOWASM_TRAPPED;

    status = turbowasm_wasi02_pollable_drop(
        streams->poll,
        slot->transient_pollable);
    if (status != TURBOWASM_OK)
        return status;

    slot->transient_pollable_valid = false;
    slot->transient_pollable = 0u;
    return TURBOWASM_OK;
}

static turbowasm_status resource_destructor(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;
    turbowasm_wasi02_stream_slot *slot;
    uint32_t index;

    if (streams == NULL || !streams->initialized ||
        rep.kind != TURBOWASM_VALUE_I32 ||
        rep.as.i32 <= 0)
        return TURBOWASM_TRAPPED;

    index = (uint32_t)rep.as.i32 - 1u;
    if (index >= streams->capacity)
        return TURBOWASM_TRAPPED;
    slot = &streams->slots[index];
    if (!slot->active ||
        identity_for_kind(slot->kind) != resource_identity)
        return TURBOWASM_TRAPPED;

    {
        turbowasm_status status =
            clear_transient_pollable(streams, slot);
        if (status != TURBOWASM_OK)
            return status;
    }

    switch (slot->kind) {
        case TURBOWASM_WASI02_STREAM_SLOT_INPUT:
            if (streams->provider.input_drop != NULL)
                streams->provider.input_drop(
                    streams->provider.context,
                    slot->provider_rep);
            break;
        case TURBOWASM_WASI02_STREAM_SLOT_OUTPUT:
            if (streams->provider.output_drop != NULL)
                streams->provider.output_drop(
                    streams->provider.context,
                    slot->provider_rep);
            break;
        case TURBOWASM_WASI02_STREAM_SLOT_ERROR:
            if (streams->provider.error_drop != NULL)
                streams->provider.error_drop(
                    streams->provider.context,
                    slot->provider_rep);
            break;
        default:
            return TURBOWASM_TRAPPED;
    }

    release_slot(streams, slot);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_streams_init(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_stream_provider *provider,
    uint32_t max_resources) {
    uint32_t i;

    if (streams == NULL || provider == NULL ||
        max_resources == 0u ||
        max_resources > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        streams->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(streams, 0, sizeof(*streams));
    streams->slots = (turbowasm_wasi02_stream_slot *)
        turbowasm_rt_calloc(
            max_resources, sizeof(*streams->slots));
    streams->free_indices = (uint32_t *)turbowasm_rt_malloc(
        (size_t)max_resources * sizeof(*streams->free_indices));
    if (streams->slots == NULL ||
        streams->free_indices == NULL) {
        turbowasm_rt_free(streams->free_indices);
        turbowasm_rt_free(streams->slots);
        memset(streams, 0, sizeof(*streams));
        return TURBOWASM_OUT_OF_MEMORY;
    }

    if (!turbowasm_component_resource_table_init(
            &streams->resources, max_resources)) {
        turbowasm_rt_free(streams->free_indices);
        turbowasm_rt_free(streams->slots);
        memset(streams, 0, sizeof(*streams));
        return TURBOWASM_INVALID_ARGUMENT;
    }

    streams->provider = *provider;
    streams->capacity = max_resources;
    streams->free_count = max_resources;
    for (i = 0u; i < max_resources; ++i)
        streams->free_indices[i] = max_resources - 1u - i;
    streams->initialized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_streams_destroy(
    turbowasm_wasi02_streams *streams) {
    if (streams == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!streams->initialized)
        return TURBOWASM_OK;
    if (streams->resources.live_count != 0u ||
        streams->free_count != streams->capacity)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_component_resource_table_destroy(
        &streams->resources);
    turbowasm_rt_free(streams->free_indices);
    turbowasm_rt_free(streams->slots);
    memset(streams, 0, sizeof(*streams));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_streams_attach_poll(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_poll *poll) {
    if (streams == NULL || !streams->initialized ||
        poll == NULL || !poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    streams->poll = poll;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_input_stream_new(
    turbowasm_wasi02_streams *streams,
    turbowasm_value provider_rep,
    uint32_t *out_resource) {
    return new_resource(
        streams,
        TURBOWASM_WASI02_STREAM_SLOT_INPUT,
        provider_rep,
        out_resource);
}

turbowasm_status turbowasm_wasi02_output_stream_new(
    turbowasm_wasi02_streams *streams,
    turbowasm_value provider_rep,
    uint32_t *out_resource) {
    return new_resource(
        streams,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        provider_rep,
        out_resource);
}

turbowasm_status turbowasm_wasi02_stream_resource_drop(
    turbowasm_wasi02_streams *streams,
    uint32_t resource) {
    static const turbowasm_wasi02_stream_slot_kind kinds[] = {
        TURBOWASM_WASI02_STREAM_SLOT_INPUT,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        TURBOWASM_WASI02_STREAM_SLOT_ERROR
    };
    size_t i;

    if (streams == NULL || !streams->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        turbowasm_value rep = {0};
        uint64_t identity = identity_for_kind(kinds[i]);

        if (turbowasm_component_resource_rep(
                &streams->resources,
                resource,
                identity,
                &rep) == TURBOWASM_OK) {
            return turbowasm_component_resource_drop(
                &streams->resources,
                resource,
                identity,
                resource_destructor,
                streams);
        }
    }
    return TURBOWASM_TRAPPED;
}

static turbowasm_status make_result_unit_ok(
    turbowasm_wasi02_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    return TURBOWASM_OK;
}

static turbowasm_status make_result_u64_ok(
    uint64_t value,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *payload;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    payload = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*payload));
    if (payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    payload->kind = TURBOWASM_WASI02_VALUE_U64;
    payload->as.u64 = value;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.value = payload;
    return TURBOWASM_OK;
}

static turbowasm_status make_result_bytes_ok(
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *result_payload = NULL;
    turbowasm_wasi02_value *items = NULL;
    size_t i;

    if (out == NULL || (size != 0u && data == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    result_payload = (turbowasm_wasi02_value *)
        turbowasm_rt_calloc(1u, sizeof(*result_payload));
    if (result_payload == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if (size != 0u) {
        if (size > SIZE_MAX / sizeof(*items)) {
            turbowasm_rt_free(result_payload);
            return TURBOWASM_OUT_OF_MEMORY;
        }
        items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
            size, sizeof(*items));
        if (items == NULL) {
            turbowasm_rt_free(result_payload);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    for (i = 0u; i < size; ++i) {
        items[i].kind = TURBOWASM_WASI02_VALUE_U8;
        items[i].as.u8 = data[i];
    }

    result_payload->kind = TURBOWASM_WASI02_VALUE_LIST;
    result_payload->as.list.items = items;
    result_payload->as.list.count = size;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.value = result_payload;
    return TURBOWASM_OK;
}

static turbowasm_status make_stream_error(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_stream_error *error,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *variant = NULL;
    turbowasm_wasi02_value *resource_value = NULL;
    uint32_t error_resource = 0u;
    turbowasm_status status;

    if (streams == NULL || error == NULL || out == NULL ||
        error->kind == TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return TURBOWASM_INVALID_ARGUMENT;

    variant = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
        1u, sizeof(*variant));
    if (variant == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    variant->kind = TURBOWASM_WASI02_VALUE_VARIANT;

    if (error->kind ==
        TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED) {
        status = new_resource(
            streams,
            TURBOWASM_WASI02_STREAM_SLOT_ERROR,
            error->error_rep,
            &error_resource);
        if (status != TURBOWASM_OK) {
            turbowasm_rt_free(variant);
            return status;
        }

        resource_value = (turbowasm_wasi02_value *)
            turbowasm_rt_calloc(1u, sizeof(*resource_value));
        if (resource_value == NULL) {
            (void)turbowasm_wasi02_stream_resource_drop(
                streams, error_resource);
            turbowasm_rt_free(variant);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        resource_value->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
        resource_value->as.resource = error_resource;
        variant->as.variant.case_index = 0u;
        variant->as.variant.value = resource_value;
    } else if (error->kind ==
               TURBOWASM_WASI02_STREAM_ERROR_CLOSED) {
        variant->as.variant.case_index = 1u;
    } else {
        turbowasm_rt_free(variant);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESULT;
    out->as.result.is_error = true;
    out->as.result.value = variant;
    return TURBOWASM_OK;
}

static turbowasm_status copy_debug_string(
    turbowasm_wasi02_string_view view,
    turbowasm_wasi02_value *out) {
    uint8_t *copy = NULL;

    if (out == NULL ||
        (view.size != 0u && view.data == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (view.size != 0u) {
        copy = (uint8_t *)turbowasm_rt_malloc(view.size);
        if (copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(copy, view.data, view.size);
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_STRING;
    out->as.string.data = copy;
    out->as.string.size = view.size;
    return TURBOWASM_OK;
}

static turbowasm_status call_input_read(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    const uint8_t *data = NULL;
    size_t size = 0u;
    uint64_t max_bytes = arguments[1].as.u64;
    turbowasm_wasi02_stream_error error = {0};
    turbowasm_status status;

    if (streams->provider.input_read == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_INPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = streams->provider.input_read(
        streams->provider.context,
        slot->provider_rep,
        max_bytes,
        &data,
        &size,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (error.kind != TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return make_stream_error(streams, &error, out);
    if ((uint64_t)size > max_bytes ||
        (size != 0u && data == NULL))
        return TURBOWASM_TRAPPED;

    return make_result_bytes_ok(data, size, out);
}

static turbowasm_status call_input_skip(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    uint64_t skipped = 0u;
    uint64_t max_bytes = arguments[1].as.u64;
    turbowasm_wasi02_stream_error error = {0};
    turbowasm_status status;

    if (streams->provider.input_skip == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_INPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = streams->provider.input_skip(
        streams->provider.context,
        slot->provider_rep,
        max_bytes,
        &skipped,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (error.kind != TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return make_stream_error(streams, &error, out);
    if (skipped > max_bytes)
        return TURBOWASM_TRAPPED;
    return make_result_u64_ok(skipped, out);
}

static turbowasm_status call_output_check_write(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    uint64_t permit = 0u;
    turbowasm_wasi02_stream_error error = {0};
    turbowasm_status status;

    if (streams->provider.output_check_write == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = streams->provider.output_check_write(
        streams->provider.context,
        slot->provider_rep,
        &permit,
        &error);
    if (status != TURBOWASM_OK)
        return status;

    slot->write_permit_valid = false;
    slot->write_permit = 0u;
    if (error.kind != TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return make_stream_error(streams, &error, out);

    slot->write_permit = permit;
    slot->write_permit_valid = true;
    return make_result_u64_ok(permit, out);
}

static turbowasm_status list_to_bytes(
    const turbowasm_wasi02_value *list,
    uint8_t **out_data,
    size_t *out_size) {
    uint8_t *data = NULL;
    size_t i;

    if (list == NULL || out_data == NULL || out_size == NULL ||
        list->kind != TURBOWASM_WASI02_VALUE_LIST ||
        (list->as.list.count != 0u &&
         list->as.list.items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    if (list->as.list.count != 0u) {
        data = (uint8_t *)turbowasm_rt_malloc(
            list->as.list.count);
        if (data == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < list->as.list.count; ++i) {
        if (list->as.list.items[i].kind !=
            TURBOWASM_WASI02_VALUE_U8) {
            turbowasm_rt_free(data);
            return TURBOWASM_TYPE_MISMATCH;
        }
        data[i] = list->as.list.items[i].as.u8;
    }

    *out_data = data;
    *out_size = list->as.list.count;
    return TURBOWASM_OK;
}

static turbowasm_status call_output_write(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    bool zeroes,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_wasi02_stream_error error = {0};
    uint8_t *data = NULL;
    size_t size = 0u;
    uint64_t requested;
    turbowasm_status status;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    if (zeroes) {
        if (streams->provider.output_write_zeroes == NULL)
            return TURBOWASM_UNSUPPORTED;
        requested = arguments[1].as.u64;
    } else {
        if (streams->provider.output_write == NULL)
            return TURBOWASM_UNSUPPORTED;
        status = list_to_bytes(
            &arguments[1], &data, &size);
        if (status != TURBOWASM_OK)
            return status;
        requested = (uint64_t)size;
    }

    if (!slot->write_permit_valid ||
        requested > slot->write_permit) {
        turbowasm_rt_free(data);
        return TURBOWASM_TRAPPED;
    }

    /*
     * check-write permits exactly the next write-like call. Consume it before
     * entering provider code, including provider-reported stream errors.
     */
    slot->write_permit_valid = false;
    slot->write_permit = 0u;

    if (zeroes) {
        status = streams->provider.output_write_zeroes(
            streams->provider.context,
            slot->provider_rep,
            requested,
            &error);
    } else {
        status = streams->provider.output_write(
            streams->provider.context,
            slot->provider_rep,
            data,
            size,
            &error);
    }
    turbowasm_rt_free(data);

    if (status != TURBOWASM_OK)
        return status;
    if (error.kind != TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return make_stream_error(streams, &error, out);
    return make_result_unit_ok(out);
}

static turbowasm_status call_output_flush(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_wasi02_stream_error error = {0};
    turbowasm_status status;

    if (streams->provider.output_flush == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    slot->write_permit_valid = false;
    slot->write_permit = 0u;
    status = streams->provider.output_flush(
        streams->provider.context,
        slot->provider_rep,
        &error);
    if (status != TURBOWASM_OK)
        return status;
    if (error.kind != TURBOWASM_WASI02_STREAM_ERROR_NONE)
        return make_stream_error(streams, &error, out);
    return make_result_unit_ok(out);
}

static turbowasm_status create_subscription_resource(
    turbowasm_wasi02_streams *streams,
    uint32_t stream_resource,
    turbowasm_wasi02_stream_slot_kind kind,
    uint32_t *out_pollable_resource) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_wasi02_stream_subscribe_fn subscribe;
    turbowasm_value pollable_rep = {0};
    turbowasm_status status;

    if (streams == NULL || streams->poll == NULL ||
        out_pollable_resource == NULL)
        return TURBOWASM_UNSUPPORTED;

    subscribe = kind == TURBOWASM_WASI02_STREAM_SLOT_INPUT
        ? streams->provider.input_subscribe
        : streams->provider.output_subscribe;
    if (subscribe == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = slot_from_resource(
        streams,
        stream_resource,
        kind,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = subscribe(
        streams->provider.context,
        slot->provider_rep,
        &pollable_rep);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_wasi02_pollable_new(
        streams->poll,
        pollable_rep,
        out_pollable_resource);
    if (status != TURBOWASM_OK) {
        if (streams->poll->provider.drop != NULL) {
            turbowasm_status drop_status =
                streams->poll->provider.drop(
                    streams->poll->provider.context,
                    pollable_rep);
            if (drop_status != TURBOWASM_OK)
                return drop_status;
        }
        return status;
    }

    return TURBOWASM_OK;
}

static turbowasm_status call_subscribe(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_stream_slot_kind kind,
    turbowasm_wasi02_value *out) {
    uint32_t pollable_resource = 0u;
    turbowasm_status status;

    status = create_subscription_resource(
        streams,
        arguments[0].as.resource,
        kind,
        &pollable_resource);
    if (status != TURBOWASM_OK)
        return status;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    out->as.resource = pollable_resource;
    return TURBOWASM_OK;
}

static turbowasm_status wait_stream_once(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    uint32_t stream_resource,
    turbowasm_wasi02_stream_slot_kind kind) {
    turbowasm_wasi02_stream_slot *slot;
    uint32_t pollable_resource = 0u;
    turbowasm_status status;
    turbowasm_status cleanup_status;

    if (streams == NULL ||
        (kind != TURBOWASM_WASI02_STREAM_SLOT_INPUT &&
         kind != TURBOWASM_WASI02_STREAM_SLOT_OUTPUT))
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Fail before provider subscribe when this invocation cannot suspend.
     */
    if (call == NULL ||
        !turbowasm_host_call_can_wait(call))
        return TURBOWASM_UNSUPPORTED;

    status = slot_from_resource(
        streams,
        stream_resource,
        kind,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = clear_transient_pollable(streams, slot);
    if (status != TURBOWASM_OK)
        return status;

    status = create_subscription_resource(
        streams,
        stream_resource,
        kind,
        &pollable_resource);
    if (status != TURBOWASM_OK)
        return status;

    slot->transient_pollable_valid = true;
    slot->transient_pollable = pollable_resource;

    status = turbowasm_wasi02_pollable_block(
        streams->poll,
        pollable_resource,
        call);

    cleanup_status = clear_transient_pollable(
        streams, slot);
    if (cleanup_status != TURBOWASM_OK)
        return cleanup_status;
    return status;
}

static bool read_result_has_progress(
    const turbowasm_wasi02_value *result,
    bool *out_error,
    bool *out_progress) {
    if (result == NULL || out_error == NULL ||
        out_progress == NULL ||
        result->kind != TURBOWASM_WASI02_VALUE_RESULT)
        return false;

    *out_error = result->as.result.is_error;
    if (*out_error) {
        *out_progress = true;
        return true;
    }

    if (result->as.result.value == NULL ||
        result->as.result.value->kind !=
            TURBOWASM_WASI02_VALUE_LIST)
        return false;

    *out_progress =
        result->as.result.value->as.list.count != 0u;
    return true;
}

static bool skip_result_has_progress(
    const turbowasm_wasi02_value *result,
    bool *out_error,
    bool *out_progress) {
    if (result == NULL || out_error == NULL ||
        out_progress == NULL ||
        result->kind != TURBOWASM_WASI02_VALUE_RESULT)
        return false;

    *out_error = result->as.result.is_error;
    if (*out_error) {
        *out_progress = true;
        return true;
    }

    if (result->as.result.value == NULL ||
        result->as.result.value->kind !=
            TURBOWASM_WASI02_VALUE_U64)
        return false;

    *out_progress = result->as.result.value->as.u64 != 0u;
    return true;
}

static turbowasm_status call_input_blocking(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments,
    bool skip,
    turbowasm_wasi02_value *out) {
    bool is_error = false;
    bool progress = false;
    uint64_t requested = arguments[1].as.u64;
    turbowasm_status status;

    status = skip
        ? call_input_skip(streams, arguments, out)
        : call_input_read(streams, arguments, out);
    if (status != TURBOWASM_OK)
        return status;

    if (!(skip
            ? skip_result_has_progress(
                out, &is_error, &progress)
            : read_result_has_progress(
                out, &is_error, &progress))) {
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_MALFORMED_MODULE;
    }

    if (is_error || progress || requested == 0u)
        return TURBOWASM_OK;

    turbowasm_wasi02_value_destroy(out);

    status = wait_stream_once(
        streams,
        call,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_INPUT);
    if (status != TURBOWASM_OK)
        return status;

    status = skip
        ? call_input_skip(streams, arguments, out)
        : call_input_read(streams, arguments, out);
    if (status != TURBOWASM_OK)
        return status;

    if (!(skip
            ? skip_result_has_progress(
                out, &is_error, &progress)
            : read_result_has_progress(
                out, &is_error, &progress))) {
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_MALFORMED_MODULE;
    }

    if (!is_error && !progress) {
        /*
         * A completed input subscription promises readable data or terminal
         * stream state. A second empty success is therefore a provider
         * contract violation rather than a reason to spin.
         */
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_TRAPPED;
    }

    return TURBOWASM_OK;
}

static bool stream_result_error_state(
    const turbowasm_wasi02_value *result,
    bool *out_error) {
    if (result == NULL || out_error == NULL ||
        result->kind != TURBOWASM_WASI02_VALUE_RESULT)
        return false;
    *out_error = result->as.result.is_error;
    return true;
}

static bool stream_result_u64_state(
    const turbowasm_wasi02_value *result,
    bool *out_error,
    uint64_t *out_value) {
    if (!stream_result_error_state(result, out_error) ||
        out_value == NULL)
        return false;
    if (*out_error) {
        *out_value = 0u;
        return true;
    }
    if (result->as.result.value == NULL ||
        result->as.result.value->kind !=
            TURBOWASM_WASI02_VALUE_U64)
        return false;
    *out_value = result->as.result.value->as.u64;
    return true;
}

static turbowasm_status blocking_output_preflight(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call) {
    if (streams == NULL || streams->poll == NULL ||
        streams->provider.output_check_write == NULL ||
        streams->provider.output_flush == NULL ||
        streams->provider.output_subscribe == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (call == NULL ||
        !turbowasm_host_call_can_wait(call))
        return TURBOWASM_UNSUPPORTED;
    if (streams->poll->provider.arm == NULL &&
        streams->poll->provider.arm_routed == NULL)
        return TURBOWASM_UNSUPPORTED;
    return TURBOWASM_OK;
}

static turbowasm_status blocking_output_check_ready(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_stream_slot *slot,
    uint64_t *out_permit,
    bool *out_error,
    turbowasm_wasi02_value *out) {
    turbowasm_status status;

    if (slot == NULL || out_permit == NULL ||
        out_error == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = call_output_check_write(
        streams, arguments, out);
    if (status != TURBOWASM_OK)
        return status;
    if (!stream_result_u64_state(
            out, out_error, out_permit)) {
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (*out_error || *out_permit != 0u)
        return TURBOWASM_OK;

    /*
     * A zero permit means the output stream is not ready. Do not leak the
     * internal permit across a failed wait attempt.
     */
    turbowasm_wasi02_value_destroy(out);
    slot->write_permit_valid = false;
    slot->write_permit = 0u;

    status = wait_stream_once(
        streams,
        call,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT);
    if (status != TURBOWASM_OK)
        return status;

    status = call_output_check_write(
        streams, arguments, out);
    if (status != TURBOWASM_OK)
        return status;
    if (!stream_result_u64_state(
            out, out_error, out_permit)) {
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (!*out_error && *out_permit == 0u) {
        /*
         * A ready output subscription promises a positive permit or an error.
         * Returning zero again is a provider readiness contract violation.
         */
        turbowasm_wasi02_value_destroy(out);
        slot->write_permit_valid = false;
        slot->write_permit = 0u;
        return TURBOWASM_TRAPPED;
    }
    return TURBOWASM_OK;
}

static turbowasm_status call_output_blocking_flush(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    uint64_t permit = 0u;
    bool is_error = false;
    turbowasm_status status;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = blocking_output_preflight(streams, call);
    if (status != TURBOWASM_OK)
        return status;

    status = call_output_flush(
        streams, arguments, out);
    if (status != TURBOWASM_OK)
        return status;
    if (!stream_result_error_state(out, &is_error)) {
        turbowasm_wasi02_value_destroy(out);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (is_error)
        return TURBOWASM_OK;

    turbowasm_wasi02_value_destroy(out);

    status = blocking_output_check_ready(
        streams,
        call,
        arguments,
        slot,
        &permit,
        &is_error,
        out);
    if (status != TURBOWASM_OK)
        return status;
    if (is_error)
        return TURBOWASM_OK;

    turbowasm_wasi02_value_destroy(out);
    slot->write_permit_valid = false;
    slot->write_permit = 0u;
    return make_result_unit_ok(out);
}

static turbowasm_status call_output_blocking_write(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments,
    bool zeroes,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    uint64_t requested;
    uint64_t offset = 0u;
    turbowasm_status status;

    if (zeroes) {
        requested = arguments[1].as.u64;
    } else {
        if (arguments[1].kind != TURBOWASM_WASI02_VALUE_LIST ||
            (arguments[1].as.list.count != 0u &&
             arguments[1].as.list.items == NULL))
            return TURBOWASM_TYPE_MISMATCH;
        requested = (uint64_t)arguments[1].as.list.count;
    }

    if (requested > 4096u)
        return TURBOWASM_TRAPPED;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = blocking_output_preflight(streams, call);
    if (status != TURBOWASM_OK)
        return status;
    if ((zeroes &&
         streams->provider.output_write_zeroes == NULL) ||
        (!zeroes &&
         streams->provider.output_write == NULL))
        return TURBOWASM_UNSUPPORTED;

    while (offset < requested) {
        turbowasm_wasi02_value check_result = {0};
        turbowasm_wasi02_value write_args[2] = {{0}};
        uint64_t permit = 0u;
        uint64_t chunk;
        bool is_error = false;

        status = blocking_output_check_ready(
            streams,
            call,
            arguments,
            slot,
            &permit,
            &is_error,
            &check_result);
        if (status != TURBOWASM_OK)
            return status;
        if (is_error) {
            *out = check_result;
            return TURBOWASM_OK;
        }

        turbowasm_wasi02_value_destroy(&check_result);
        chunk = permit < (requested - offset)
            ? permit
            : (requested - offset);
        if (chunk == 0u)
            return TURBOWASM_TRAPPED;

        write_args[0] = arguments[0];
        if (zeroes) {
            write_args[1].kind = TURBOWASM_WASI02_VALUE_U64;
            write_args[1].as.u64 = chunk;
        } else {
            write_args[1].kind = TURBOWASM_WASI02_VALUE_LIST;
            write_args[1].as.list.items =
                arguments[1].as.list.items + (size_t)offset;
            write_args[1].as.list.count = (size_t)chunk;
        }

        status = call_output_write(
            streams, write_args, zeroes, out);
        if (status != TURBOWASM_OK)
            return status;
        if (!stream_result_error_state(out, &is_error)) {
            turbowasm_wasi02_value_destroy(out);
            return TURBOWASM_MALFORMED_MODULE;
        }
        if (is_error)
            return TURBOWASM_OK;

        turbowasm_wasi02_value_destroy(out);
        offset += chunk;
    }

    return call_output_blocking_flush(
        streams, call, arguments, out);
}

static void clear_output_write_permit(
    turbowasm_wasi02_stream_slot *slot) {
    if (slot == NULL)
        return;
    slot->write_permit_valid = false;
    slot->write_permit = 0u;
}

static turbowasm_status call_output_splice(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *output_slot;
    turbowasm_wasi02_stream_slot *input_slot;
    turbowasm_wasi02_value check_args[1] = {{0}};
    turbowasm_wasi02_value check_result = {0};
    turbowasm_wasi02_value read_args[2] = {{0}};
    turbowasm_wasi02_value read_result = {0};
    turbowasm_wasi02_value write_args[2] = {{0}};
    turbowasm_wasi02_value write_result = {0};
    turbowasm_wasi02_value *read_bytes;
    uint64_t permit = 0u;
    uint64_t requested = arguments[2].as.u64;
    uint64_t read_limit;
    uint64_t transferred;
    bool is_error = false;
    turbowasm_status status;

    /*
     * Splice is a pure composition of the already-qualified operations. Check
     * the whole provider/resource boundary before the first provider call so
     * an unsupported adapter cannot leave a hidden check-write permit behind.
     */
    if (streams->provider.output_check_write == NULL ||
        streams->provider.input_read == NULL ||
        streams->provider.output_write == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &output_slot);
    if (status != TURBOWASM_OK)
        return status;
    status = slot_from_resource(
        streams,
        arguments[1].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_INPUT,
        &input_slot);
    if (status != TURBOWASM_OK)
        return status;
    (void)input_slot;

    check_args[0] = arguments[0];
    status = call_output_check_write(
        streams, check_args, &check_result);
    if (status != TURBOWASM_OK)
        return status;
    if (!stream_result_u64_state(
            &check_result, &is_error, &permit)) {
        turbowasm_wasi02_value_destroy(&check_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (is_error) {
        *out = check_result;
        return TURBOWASM_OK;
    }
    turbowasm_wasi02_value_destroy(&check_result);

    read_limit = permit < requested ? permit : requested;
    if (read_limit == 0u) {
        /*
         * No transfer is possible. Avoid touching the input stream and do not
         * leak splice's internal check-write permit to a later external write.
         */
        clear_output_write_permit(output_slot);
        return make_result_u64_ok(0u, out);
    }

    read_args[0] = arguments[1];
    read_args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    read_args[1].as.u64 = read_limit;

    status = call_input_read(
        streams, read_args, &read_result);
    if (status != TURBOWASM_OK) {
        clear_output_write_permit(output_slot);
        return status;
    }
    if (!stream_result_error_state(
            &read_result, &is_error)) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (is_error) {
        clear_output_write_permit(output_slot);
        *out = read_result;
        return TURBOWASM_OK;
    }

    read_bytes = read_result.as.result.value;
    if (read_bytes == NULL ||
        read_bytes->kind != TURBOWASM_WASI02_VALUE_LIST ||
        (read_bytes->as.list.count != 0u &&
         read_bytes->as.list.items == NULL)) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }
    transferred = (uint64_t)read_bytes->as.list.count;
    if (transferred > read_limit) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_TRAPPED;
    }
    if (transferred == 0u) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return make_result_u64_ok(0u, out);
    }

    write_args[0] = arguments[0];
    write_args[1] = *read_bytes;
    status = call_output_write(
        streams, write_args, false, &write_result);
    if (status != TURBOWASM_OK) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return status;
    }
    if (!stream_result_error_state(
            &write_result, &is_error)) {
        turbowasm_wasi02_value_destroy(&write_result);
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }

    turbowasm_wasi02_value_destroy(&read_result);
    if (is_error) {
        *out = write_result;
        return TURBOWASM_OK;
    }

    turbowasm_wasi02_value_destroy(&write_result);
    return make_result_u64_ok(transferred, out);
}

static turbowasm_status blocking_splice_preflight(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments) {
    turbowasm_wasi02_stream_slot *slot;

    if (streams == NULL || streams->poll == NULL ||
        streams->provider.output_check_write == NULL ||
        streams->provider.output_write == NULL ||
        streams->provider.input_read == NULL ||
        streams->provider.output_subscribe == NULL ||
        streams->provider.input_subscribe == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (call == NULL ||
        !turbowasm_host_call_can_wait(call))
        return TURBOWASM_UNSUPPORTED;
    if (streams->poll->provider.arm == NULL &&
        streams->poll->provider.arm_routed == NULL)
        return TURBOWASM_UNSUPPORTED;

    if (slot_from_resource(
            streams,
            arguments[0].as.resource,
            TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
            &slot) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;
    if (slot_from_resource(
            streams,
            arguments[1].as.resource,
            TURBOWASM_WASI02_STREAM_SLOT_INPUT,
            &slot) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

static turbowasm_status call_output_blocking_splice(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *output_slot;
    turbowasm_wasi02_value check_result = {0};
    turbowasm_wasi02_value read_args[2] = {{0}};
    turbowasm_wasi02_value read_result = {0};
    turbowasm_wasi02_value write_args[2] = {{0}};
    turbowasm_wasi02_value write_result = {0};
    turbowasm_wasi02_value *read_bytes;
    uint64_t permit = 0u;
    uint64_t requested = arguments[2].as.u64;
    uint64_t read_limit;
    uint64_t transferred;
    bool is_error = false;
    turbowasm_status status;

    /*
     * Compose blocking-splice from the already-qualified blocking readiness
     * helpers. This preserves their post-wake contract checks instead of
     * merely waiting and then trusting a second nonblocking probe.
     */
    status = blocking_splice_preflight(
        streams, call, arguments);
    if (status != TURBOWASM_OK)
        return status;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
        &output_slot);
    if (status != TURBOWASM_OK)
        return status;

    status = blocking_output_check_ready(
        streams,
        call,
        arguments,
        output_slot,
        &permit,
        &is_error,
        &check_result);
    if (status != TURBOWASM_OK)
        return status;
    if (is_error) {
        *out = check_result;
        return TURBOWASM_OK;
    }
    turbowasm_wasi02_value_destroy(&check_result);

    read_limit = permit < requested ? permit : requested;
    if (read_limit == 0u) {
        clear_output_write_permit(output_slot);
        return make_result_u64_ok(0u, out);
    }

    read_args[0] = arguments[1];
    read_args[1].kind = TURBOWASM_WASI02_VALUE_U64;
    read_args[1].as.u64 = read_limit;
    status = call_input_blocking(
        streams,
        call,
        read_args,
        false,
        &read_result);
    if (status != TURBOWASM_OK) {
        clear_output_write_permit(output_slot);
        return status;
    }
    if (!stream_result_error_state(
            &read_result, &is_error)) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }
    if (is_error) {
        clear_output_write_permit(output_slot);
        *out = read_result;
        return TURBOWASM_OK;
    }

    read_bytes = read_result.as.result.value;
    if (read_bytes == NULL ||
        read_bytes->kind != TURBOWASM_WASI02_VALUE_LIST ||
        (read_bytes->as.list.count != 0u &&
         read_bytes->as.list.items == NULL)) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }
    transferred = (uint64_t)read_bytes->as.list.count;
    if (transferred == 0u || transferred > read_limit) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_TRAPPED;
    }

    write_args[0] = arguments[0];
    write_args[1] = *read_bytes;
    status = call_output_write(
        streams, write_args, false, &write_result);
    if (status != TURBOWASM_OK) {
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return status;
    }
    if (!stream_result_error_state(
            &write_result, &is_error)) {
        turbowasm_wasi02_value_destroy(&write_result);
        turbowasm_wasi02_value_destroy(&read_result);
        clear_output_write_permit(output_slot);
        return TURBOWASM_MALFORMED_MODULE;
    }

    turbowasm_wasi02_value_destroy(&read_result);
    if (is_error) {
        *out = write_result;
        return TURBOWASM_OK;
    }

    turbowasm_wasi02_value_destroy(&write_result);
    return make_result_u64_ok(transferred, out);
}

static turbowasm_status call_error_debug(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_wasi02_string_view view = {0};
    turbowasm_status status;

    if (streams->provider.error_debug == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
        TURBOWASM_WASI02_STREAM_SLOT_ERROR,
        &slot);
    if (status != TURBOWASM_OK)
        return status;

    status = streams->provider.error_debug(
        streams->provider.context,
        slot->provider_rep,
        &view);
    if (status != TURBOWASM_OK)
        return status;
    return copy_debug_string(view, out);
}

turbowasm_status turbowasm_wasi02_streams_call_with_host(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;
    size_t i;
    turbowasm_status status;

    if (streams == NULL || !streams->initialized ||
        interface_name == NULL || function_name == NULL ||
        out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    iface = turbowasm_wasi02_find_interface(
        "wasi:io", interface_name);
    function = turbowasm_wasi02_find_function(
        iface, function_name);
    if (function == NULL ||
        function->param_count != argument_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < argument_count; ++i) {
        if (!turbowasm_wasi02_value_matches_type(
                function->params[i].type,
                &arguments[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    memset(out_result, 0, sizeof(*out_result));

    if (strcmp(interface_name, "streams") == 0) {
        if (strcmp(
                function_name,
                "[method]input-stream.blocking-read") == 0) {
            status = call_input_blocking(
                streams, call, arguments, false, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]input-stream.blocking-skip") == 0) {
            status = call_input_blocking(
                streams, call, arguments, true, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]input-stream.read") == 0) {
            status = call_input_read(
                streams, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]input-stream.skip") == 0) {
            status = call_input_skip(
                streams, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]input-stream.subscribe") == 0) {
            status = call_subscribe(
                streams, arguments,
                TURBOWASM_WASI02_STREAM_SLOT_INPUT,
                out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.blocking-splice") == 0) {
            status = call_output_blocking_splice(
                streams, call, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.splice") == 0) {
            status = call_output_splice(
                streams, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.blocking-write-and-flush") == 0) {
            status = call_output_blocking_write(
                streams, call, arguments, false, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.blocking-flush") == 0) {
            status = call_output_blocking_flush(
                streams, call, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.blocking-write-zeroes-and-flush") == 0) {
            status = call_output_blocking_write(
                streams, call, arguments, true, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.check-write") == 0) {
            status = call_output_check_write(
                streams, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.write") == 0) {
            status = call_output_write(
                streams, arguments, false, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.flush") == 0) {
            status = call_output_flush(
                streams, arguments, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.write-zeroes") == 0) {
            status = call_output_write(
                streams, arguments, true, out_result);
        } else if (strcmp(
                       function_name,
                       "[method]output-stream.subscribe") == 0) {
            status = call_subscribe(
                streams, arguments,
                TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
                out_result);
        } else {
            return TURBOWASM_UNSUPPORTED;
        }
    } else if (strcmp(interface_name, "error") == 0 &&
               strcmp(
                   function_name,
                   "[method]error.to-debug-string") == 0) {
        status = call_error_debug(
            streams, arguments, out_result);
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    if (status != TURBOWASM_OK)
        return status;
    if (function->result != NULL &&
        !turbowasm_wasi02_value_matches_type(
            function->result, out_result)) {
        turbowasm_wasi02_value_destroy(out_result);
        return TURBOWASM_MALFORMED_MODULE;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_streams_call(
    turbowasm_wasi02_streams *streams,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    return turbowasm_wasi02_streams_call_with_host(
        streams,
        NULL,
        interface_name,
        function_name,
        arguments,
        argument_count,
        out_result);
}


static bool stream_component_name_is(
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

static const turbowasm_wasi02_type_desc *
stream_wasi_type_base(
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

static const turbowasm_wasi02_interface_desc *
stream_interface_by_component_name(
    turbowasm_component_name name) {
    if (stream_component_name_is(
            name, "wasi:io/streams@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:io", "streams");
    if (stream_component_name_is(
            name, "wasi:io/error@0.2.8"))
        return turbowasm_wasi02_find_interface(
            "wasi:io", "error");
    return NULL;
}

static const turbowasm_wasi02_function_desc *
stream_function_by_component_name(
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

static bool stream_bind_identity(
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

static bool stream_bind_resource_type(
    turbowasm_wasi02_streams *streams,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_wasi02_type_desc *wasi_type,
    turbowasm_component_type_kind expected_handle_kind) {
    const turbowasm_wasi02_type_desc *base =
        stream_wasi_type_base(wasi_type);
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;
    uint64_t identity;

    if (streams == NULL || graph == NULL || base == NULL ||
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

    if (strcmp(
            base->as.resource.package_name,
            "wasi:io") != 0)
        return false;

    if (strcmp(
            base->as.resource.interface_name,
            "streams") == 0 &&
        strcmp(
            base->as.resource.resource_name,
            "input-stream") == 0)
        return stream_bind_identity(
            &streams->component_input_identity,
            &streams->component_input_identity_bound,
            identity);

    if (strcmp(
            base->as.resource.interface_name,
            "streams") == 0 &&
        strcmp(
            base->as.resource.resource_name,
            "output-stream") == 0)
        return stream_bind_identity(
            &streams->component_output_identity,
            &streams->component_output_identity_bound,
            identity);

    if (strcmp(
            base->as.resource.interface_name,
            "error") == 0 &&
        strcmp(
            base->as.resource.resource_name,
            "error") == 0)
        return stream_bind_identity(
            &streams->component_error_identity,
            &streams->component_error_identity_bound,
            identity);

    if (strcmp(
            base->as.resource.interface_name,
            "poll") == 0 &&
        strcmp(
            base->as.resource.resource_name,
            "pollable") == 0) {
        turbowasm_wasi02_poll *poll = streams->poll;
        if (poll == NULL || !poll->initialized)
            return false;
        return stream_bind_identity(
            &poll->pollable_identity,
            &poll->pollable_identity_bound,
            identity);
    }

    return false;
}

static bool stream_component_type_matches_wasi(
    turbowasm_wasi02_streams *streams,
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

    base = stream_wasi_type_base(wasi_type);
    if (base == NULL)
        return false;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        memset(&inline_storage, 0, sizeof(inline_storage));
        inline_storage.kind = ref.as.inline_type;
        type = &inline_storage;
    } else if (ref.kind ==
               TURBOWASM_COMPONENT_TYPE_REF_INDEXED) {
        type = turbowasm_component_type_graph_get(
            graph, ref.as.indexed);
    } else {
        return false;
    }

    if (type == NULL)
        return false;

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
            return type->kind ==
                TURBOWASM_COMPONENT_TYPE_BOOL;
        case TURBOWASM_WASI02_TYPE_U8:
            return type->kind ==
                TURBOWASM_COMPONENT_TYPE_U8;
        case TURBOWASM_WASI02_TYPE_U32:
            return type->kind ==
                TURBOWASM_COMPONENT_TYPE_U32;
        case TURBOWASM_WASI02_TYPE_U64:
            return type->kind ==
                TURBOWASM_COMPONENT_TYPE_U64;
        case TURBOWASM_WASI02_TYPE_STRING:
            return type->kind ==
                TURBOWASM_COMPONENT_TYPE_STRING;

        case TURBOWASM_WASI02_TYPE_LIST:
            return type->kind ==
                       TURBOWASM_COMPONENT_TYPE_LIST &&
                   stream_component_type_matches_wasi(
                       streams,
                       graph,
                       type->as.list.element_type,
                       base->as.list.element,
                       resource_handle_kind,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_RESULT:
            if (type->kind !=
                    TURBOWASM_COMPONENT_TYPE_RESULT ||
                type->as.result.has_ok !=
                    (base->as.result.ok != NULL) ||
                type->as.result.has_error !=
                    (base->as.result.error != NULL))
                return false;
            if (type->as.result.has_ok &&
                !stream_component_type_matches_wasi(
                    streams,
                    graph,
                    type->as.result.ok,
                    base->as.result.ok,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            if (type->as.result.has_error &&
                !stream_component_type_matches_wasi(
                    streams,
                    graph,
                    type->as.result.error,
                    base->as.result.error,
                    resource_handle_kind,
                    depth + 1u))
                return false;
            return true;

        case TURBOWASM_WASI02_TYPE_VARIANT:
            if (type->kind !=
                    TURBOWASM_COMPONENT_TYPE_VARIANT ||
                type->as.variant.cases == NULL ||
                base->as.variant.cases == NULL ||
                type->as.variant.count !=
                    base->as.variant.count ||
                base->as.variant.count == 0u)
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
                    memcmp(
                        ccase->name,
                        wcase->name,
                        name_size) != 0 ||
                    ccase->has_payload !=
                        (wcase->payload != NULL))
                    return false;
                if (ccase->has_payload &&
                    !stream_component_type_matches_wasi(
                        streams,
                        graph,
                        ccase->payload,
                        wcase->payload,
                        resource_handle_kind,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_RESOURCE:
            return stream_bind_resource_type(
                streams,
                graph,
                ref,
                base,
                resource_handle_kind);

        case TURBOWASM_WASI02_TYPE_UNIT:
        case TURBOWASM_WASI02_TYPE_ALIAS:
        case TURBOWASM_WASI02_TYPE_TUPLE:
        case TURBOWASM_WASI02_TYPE_RECORD:
        case TURBOWASM_WASI02_TYPE_OPTION:
        case TURBOWASM_WASI02_TYPE_ENUM:
        case TURBOWASM_WASI02_TYPE_FLAGS:
        default:
            return false;
    }
}

static bool stream_binding_matches_descriptor(
    turbowasm_wasi02_streams *streams,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_wasi02_function_desc *function) {
    const turbowasm_component_type *function_type;
    uint32_t i;

    if (streams == NULL || graph == NULL ||
        function == NULL)
        return false;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind !=
            TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count !=
            function->param_count ||
        function_type->as.function.has_result !=
            (function->result != NULL))
        return false;

    for (i = 0u; i < function->param_count; ++i) {
        if (!stream_component_type_matches_wasi(
                streams,
                graph,
                function_type->as.function.params[i],
                function->params[i].type,
                TURBOWASM_COMPONENT_TYPE_BORROW,
                0u))
            return false;
    }

    if (function->result != NULL &&
        !stream_component_type_matches_wasi(
            streams,
            graph,
            function_type->as.function.result,
            function->result,
            TURBOWASM_COMPONENT_TYPE_OWN,
            0u))
        return false;

    return true;
}

static bool wasi02_streams_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;

    if (streams == NULL || !streams->initialized)
        return false;

    iface = stream_interface_by_component_name(
        instance_name);
    function = stream_function_by_component_name(
        iface, function_name);
    return stream_binding_matches_descriptor(
        streams, graph, function_type, function);
}

static bool stream_component_identity_kind(
    const turbowasm_wasi02_streams *streams,
    uint64_t identity,
    turbowasm_wasi02_stream_slot_kind *out_kind) {
    turbowasm_wasi02_stream_slot_kind kind =
        TURBOWASM_WASI02_STREAM_SLOT_NONE;

    if (streams == NULL || identity == 0u)
        return false;

    if (streams->component_input_identity_bound &&
        streams->component_input_identity == identity)
        kind = TURBOWASM_WASI02_STREAM_SLOT_INPUT;
    else if (streams->component_output_identity_bound &&
             streams->component_output_identity == identity)
        kind = TURBOWASM_WASI02_STREAM_SLOT_OUTPUT;
    else if (streams->component_error_identity_bound &&
             streams->component_error_identity == identity)
        kind = TURBOWASM_WASI02_STREAM_SLOT_ERROR;
    else
        return false;

    if (out_kind != NULL)
        *out_kind = kind;
    return true;
}

static bool stream_imported_resource_identity(
    turbowasm_wasi02_streams *streams,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type_ref,
    turbowasm_component_type_kind *out_handle_kind,
    turbowasm_wasi02_stream_slot_kind *out_slot_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;
    turbowasm_wasi02_stream_slot_kind slot_kind;

    if (streams == NULL || graph == NULL ||
        type_ref.kind !=
            TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, type_ref.as.indexed);
    if (handle_type == NULL ||
        (handle_type->kind !=
             TURBOWASM_COMPONENT_TYPE_OWN &&
         handle_type->kind !=
             TURBOWASM_COMPONENT_TYPE_BORROW))
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind !=
            TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        !stream_component_identity_kind(
            streams,
            resource_type->as.resource.identity,
            &slot_kind))
        return false;

    if (out_handle_kind != NULL)
        *out_handle_kind = handle_type->kind;
    if (out_slot_kind != NULL)
        *out_slot_kind = slot_kind;
    return true;
}

static turbowasm_status wasi02_streams_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;
    turbowasm_component_type_kind handle_kind;
    turbowasm_wasi02_stream_slot_kind slot_kind;
    turbowasm_wasi02_stream_slot *slot;
    uint32_t handle;

    if (value == NULL || out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!stream_imported_resource_identity(
            streams,
            graph,
            type,
            &handle_kind,
            &slot_kind))
        return TURBOWASM_TYPE_MISMATCH;
    if (value->kind != handle_kind ||
        value->as.resource_rep.kind !=
            TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle =
        (uint32_t)value->as.resource_rep.as.i32;
    if (slot_from_resource(
            streams,
            handle,
            slot_kind,
            &slot) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_streams_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;
    turbowasm_component_type_kind handle_kind;
    turbowasm_wasi02_stream_slot_kind slot_kind;
    turbowasm_wasi02_stream_slot *slot;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!stream_imported_resource_identity(
            streams,
            graph,
            type,
            &handle_kind,
            &slot_kind))
        return TURBOWASM_TYPE_MISMATCH;
    if (slot_from_resource(
            streams,
            handle,
            slot_kind,
            &slot) != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = handle_kind;
    out->as.resource_rep.kind = TURBOWASM_VALUE_I32;
    out->as.resource_rep.as.i32 = (int32_t)handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_streams_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;

    if (streams == NULL || !streams->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!stream_component_identity_kind(
            streams, resource_identity, NULL))
        return TURBOWASM_TYPE_MISMATCH;

    return turbowasm_wasi02_stream_resource_drop(
        streams, handle);
}

static turbowasm_status wasi02_streams_invoke(
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
    turbowasm_wasi02_streams *streams =
        (turbowasm_wasi02_streams *)context;
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;
    turbowasm_wasi02_value
        wasi_arguments[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {{0}};
    turbowasm_wasi02_value wasi_result = {0};
    size_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (streams == NULL || !streams->initialized ||
        graph == NULL || trap == NULL ||
        !wasi02_streams_can_bind(
            context,
            instance_name,
            function_name,
            graph,
            function_type))
        return TURBOWASM_TYPE_MISMATCH;

    iface = stream_interface_by_component_name(
        instance_name);
    function = stream_function_by_component_name(
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

    status = turbowasm_wasi02_streams_call_with_host(
        streams,
        call,
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

turbowasm_status turbowasm_wasi02_streams_imports(
    turbowasm_wasi02_streams *streams,
    turbowasm_component_exec_imports *out_imports) {
    if (streams == NULL || !streams->initialized ||
        out_imports == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_imports, 0, sizeof(*out_imports));
    out_imports->context = streams;
    out_imports->can_bind = wasi02_streams_can_bind;
    out_imports->invoke = wasi02_streams_invoke;
    out_imports->resource_lower =
        wasi02_streams_resource_lower;
    out_imports->resource_lift =
        wasi02_streams_resource_lift;
    out_imports->resource_drop =
        wasi02_streams_resource_drop;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_streams_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_streams *streams) {
    turbowasm_component_exec_imports imports;
    turbowasm_status status;

    if (exec == NULL || binary == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_wasi02_streams_imports(
        streams, &imports);
    if (status != TURBOWASM_OK)
        return status;

    return turbowasm_component_exec_init_with_imports(
        exec, binary, &imports);
}
