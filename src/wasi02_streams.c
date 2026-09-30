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

static turbowasm_status call_subscribe(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_value *arguments,
    turbowasm_wasi02_stream_slot_kind kind,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_stream_slot *slot;
    turbowasm_wasi02_stream_subscribe_fn subscribe;
    turbowasm_value pollable_rep = {0};
    uint32_t pollable_resource = 0u;
    turbowasm_status status;

    if (streams == NULL || streams->poll == NULL)
        return TURBOWASM_UNSUPPORTED;

    subscribe = kind == TURBOWASM_WASI02_STREAM_SLOT_INPUT
        ? streams->provider.input_subscribe
        : streams->provider.output_subscribe;
    if (subscribe == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = slot_from_resource(
        streams,
        arguments[0].as.resource,
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
        &pollable_resource);
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

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    out->as.resource = pollable_resource;
    return TURBOWASM_OK;
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

turbowasm_status turbowasm_wasi02_streams_call(
    turbowasm_wasi02_streams *streams,
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
