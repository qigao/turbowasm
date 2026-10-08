#include "component_buffer.h"
#include "runtime_alloc.h"

#include <string.h>

turbowasm_status turbowasm_component_buffer_validate(const turbowasm_component_buffer *buffer,
    const turbowasm_component_type_graph *graph, bool has_payload,
    turbowasm_component_type_ref payload, bool destination) {
    uint32_t i;
    if (buffer == NULL || buffer->leased ||
        (buffer->kind != TURBOWASM_COMPONENT_BUFFER_HOST && buffer->kind != TURBOWASM_COMPONENT_BUFFER_GUEST))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!has_payload || buffer->length == 0u) return TURBOWASM_OK;
    if (buffer->kind == TURBOWASM_COMPONENT_BUFFER_GUEST) {
        const turbowasm_component_canonical_memory *memory = &buffer->guest.memory;
        uint32_t features;
        uint64_t stride;
        if (!turbowasm_component_value_type_equal(graph, payload, buffer->guest.graph, buffer->guest.type) ||
            !turbowasm_component_transfer_type_features(buffer->guest.graph, buffer->guest.type, &features))
            return TURBOWASM_TYPE_MISMATCH;
        if (((features & TURBOWASM_COMPONENT_VALUE_RESOURCES) != 0u &&
             (destination ? memory->resource_lower == NULL : memory->resource_lift == NULL)) ||
            ((features & TURBOWASM_COMPONENT_VALUE_ENDPOINTS) != 0u &&
             (destination ? memory->endpoint_lower == NULL : memory->endpoint_lift == NULL)) ||
            (destination && (features & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u &&
             memory->guest_realloc == NULL) ||
            (destination && (features & (TURBOWASM_COMPONENT_VALUE_RESOURCES | TURBOWASM_COMPONENT_VALUE_ENDPOINTS)) != 0u &&
             (buffer->guest.commit == NULL || buffer->guest.rollback == NULL)))
            return TURBOWASM_UNSUPPORTED;
        if ((buffer->guest.commit == NULL) != (buffer->guest.rollback == NULL) ||
            (buffer->guest.begin_copy == NULL) != (buffer->guest.end_copy == NULL))
            return TURBOWASM_INVALID_ARGUMENT;
        return turbowasm_component_canonical_validate_range(buffer->guest.graph, buffer->guest.type,
            memory, buffer->guest.address, buffer->length, &stride);
    }
    if (buffer->values == NULL || (size_t)buffer->length > SIZE_MAX / sizeof(*buffer->values))
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < buffer->length; ++i) {
        if (destination) {
            if (buffer->values[i].kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || buffer->values[i].release != NULL)
                return TURBOWASM_INVALID_ARGUMENT;
        } else {
            turbowasm_status status = turbowasm_component_canonical_validate_value(graph, payload, &buffer->values[i]);
            if (status != TURBOWASM_OK) return status;
        }
    }
    return TURBOWASM_OK;
}

static turbowasm_status guest_stride(const turbowasm_component_buffer *buffer, uint64_t *out) {
    return turbowasm_component_canonical_validate_range(buffer->guest.graph, buffer->guest.type,
        &buffer->guest.memory, buffer->guest.address, buffer->length, out);
}

static turbowasm_status copy_values(turbowasm_component_buffer *source,
    turbowasm_component_buffer *destination, bool has_payload, uint32_t count) {
    turbowasm_component_value *batch = NULL;
    turbowasm_status status = TURBOWASM_OK;
    bool allocated = false;
    uint32_t i;
    uint64_t source_stride = 0u, destination_stride = 0u;
    if (source == NULL || destination == NULL || count == 0u ||
        source->progress > source->length || destination->progress > destination->length ||
        count > source->length - source->progress || count > destination->length - destination->progress)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!has_payload) {
        source->progress += count; destination->progress += count;
        return TURBOWASM_OK;
    }
    if (source->kind == TURBOWASM_COMPONENT_BUFFER_GUEST) {
        status = guest_stride(source, &source_stride);
        if (status != TURBOWASM_OK) return status;
    }
    if (destination->kind == TURBOWASM_COMPONENT_BUFFER_GUEST) {
        status = guest_stride(destination, &destination_stride);
        if (status != TURBOWASM_OK) return status;
    }
    if (source->kind == TURBOWASM_COMPONENT_BUFFER_GUEST) {
        if ((size_t)count > SIZE_MAX / sizeof(*batch)) return TURBOWASM_OUT_OF_MEMORY;
        batch = turbowasm_rt_calloc(count, sizeof(*batch));
        if (batch == NULL) return TURBOWASM_OUT_OF_MEMORY;
        allocated = true;
        for (i = 0u; i < count; ++i) {
            status = turbowasm_component_canonical_lift_value(source->guest.graph, source->guest.type,
                &source->guest.memory, source->guest.address + ((uint64_t)source->progress + i) * source_stride,
                &batch[i]);
            if (status != TURBOWASM_OK) goto cleanup;
        }
        source->progress += count;
    } else batch = source->values + source->progress;

    if (destination->kind == TURBOWASM_COMPONENT_BUFFER_GUEST) {
        for (i = 0u; i < count; ++i) {
            status = turbowasm_component_canonical_lower_value(destination->guest.graph, destination->guest.type,
                &destination->guest.memory,
                destination->guest.address + ((uint64_t)destination->progress + i) * destination_stride, &batch[i]);
            if (status != TURBOWASM_OK) break;
        }
        if (status == TURBOWASM_OK && destination->guest.commit != NULL)
            status = destination->guest.commit(destination->guest.context, batch, count);
        if (status != TURBOWASM_OK) {
            if (destination->guest.rollback != NULL) {
                /* Preserve the copy's primary trap; rollback must release all
                 * reservations even if a secondary cleanup reports failure. */
                (void)destination->guest.rollback(destination->guest.context);
            }
            goto cleanup;
        }
        destination->progress += count;
        if (!allocated) source->progress += count;
        for (i = 0u; i < count; ++i) {
            turbowasm_status release = turbowasm_component_value_destroy(&batch[i]);
            if (status == TURBOWASM_OK) status = release;
        }
    } else {
        if (destination->receive != NULL) {
            status = destination->receive(destination->receive_context, batch, count);
            if (status != TURBOWASM_OK) goto cleanup;
        }
        for (i = 0u; i < count; ++i) {
            destination->values[destination->progress + i] = batch[i];
            memset(&batch[i], 0, sizeof(batch[i]));
        }
        destination->progress += count;
        if (!allocated) source->progress += count;
    }
cleanup:
    if (allocated) {
        for (i = 0u; i < count; ++i) {
            turbowasm_status release = turbowasm_component_value_destroy(&batch[i]);
            if (status == TURBOWASM_OK) status = release;
        }
        turbowasm_rt_free(batch);
    }
    return status;
}

turbowasm_status turbowasm_component_buffer_copy(turbowasm_component_buffer *source,
    turbowasm_component_buffer *destination, bool has_payload, uint32_t count,
    struct turbowasm_component_task *driver) {
    turbowasm_status status;
    bool source_entered = false, destination_entered = false;
    if (source == NULL || destination == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (has_payload && source->kind == TURBOWASM_COMPONENT_BUFFER_GUEST && source->guest.begin_copy != NULL) {
        status = source->guest.begin_copy(source->guest.context, driver);
        if (status != TURBOWASM_OK) return status;
        source_entered = true;
    }
    if (has_payload && destination->kind == TURBOWASM_COMPONENT_BUFFER_GUEST && destination->guest.begin_copy != NULL) {
        status = destination->guest.begin_copy(destination->guest.context, driver);
        if (status != TURBOWASM_OK) goto cleanup;
        destination_entered = true;
    }
    status = copy_values(source, destination, has_payload, count);
cleanup:
    if (destination_entered) destination->guest.end_copy(destination->guest.context);
    if (source_entered) source->guest.end_copy(source->guest.context);
    return status;
}
