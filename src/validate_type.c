#include "validate_type.h"

#include <stdint.h>
#include <string.h>

static bool turbowasm_validation_read_s33(
    turbowasm_reader *reader,
    int64_t *out) {
    uint64_t value = 0u;
    unsigned shift = 0u;
    unsigned count = 0u;
    uint8_t byte = 0u;

    if (reader == NULL || out == NULL)
        return false;

    do {
        uint8_t payload;

        if (count == 5u || !turbowasm_reader_u8(reader, &byte))
            return false;

        payload = (uint8_t)(byte & 0x7fu);
        if (count == 4u) {
            const bool negative = (payload & 0x10u) != 0u;
            const uint8_t unused = (uint8_t)(payload & 0x60u);
            if ((!negative && unused != 0u) ||
                (negative && unused != 0x60u))
                return false;
        }

        value |= (uint64_t)payload << shift;
        shift += 7u;
        ++count;
    } while ((byte & 0x80u) != 0u);

    if (count == 5u) {
        if ((byte & 0x10u) != 0u)
            value |= UINT64_MAX << 33u;
    } else if ((byte & 0x40u) != 0u && shift < 64u) {
        value |= UINT64_MAX << shift;
    }

    *out = (int64_t)value;
    return true;
}

turbowasm_status turbowasm_validation_read_heaptype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out) {
    int64_t heap;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_validation_read_s33(reader, &heap))
        return TURBOWASM_MALFORMED_MODULE;

    out->definition = NULL;
    if (heap >= 0) {
        if ((uint64_t)heap > UINT32_MAX)
            return TURBOWASM_MALFORMED_MODULE;
        out->carrier = 0x70u;
        out->is_reference = true;
        out->heap_kind = TURBOWASM_VALIDATION_HEAP_TYPE_INDEX;
        out->type_index = (uint32_t)heap;
        if (context != NULL) {
            if (out->type_index >= context->type_count)
                return TURBOWASM_MALFORMED_MODULE;
            out->definition = &context->types[out->type_index];
            if (out->definition->defined &&
                out->definition->kind != TURBOWASM_TYPE_FUNCTION)
                out->carrier = 0x6eu;
        }
        return TURBOWASM_OK;
    }

    switch (heap) {
        case -18: case -19: case -20: case -21: case -22: case -15:
            out->carrier = 0x6eu;
            out->is_reference = true;
            out->type_index = UINT32_MAX;
            out->heap_kind = heap == -18 ? TURBOWASM_VALIDATION_HEAP_ANY :
                heap == -19 ? TURBOWASM_VALIDATION_HEAP_EQ :
                heap == -20 ? TURBOWASM_VALIDATION_HEAP_I31 :
                heap == -21 ? TURBOWASM_VALIDATION_HEAP_STRUCT :
                heap == -22 ? TURBOWASM_VALIDATION_HEAP_ARRAY :
                              TURBOWASM_VALIDATION_HEAP_BOTTOM;
            return TURBOWASM_OK;
        case -13:
        case -14:
            out->carrier = heap == -13 ? 0x70u : 0x6fu;
            out->is_reference = true;
            out->heap_kind = heap == -13
                ? TURBOWASM_VALIDATION_HEAP_NOFUNC
                : TURBOWASM_VALIDATION_HEAP_NOEXTERN;
            out->type_index = UINT32_MAX;
            return TURBOWASM_OK;
        case -16:
            out->carrier = 0x70u;
            out->is_reference = true;
            out->heap_kind = TURBOWASM_VALIDATION_HEAP_FUNC;
            out->type_index = UINT32_MAX;
            return TURBOWASM_OK;
        case -17:
            out->carrier = 0x6fu;
            out->is_reference = true;
            out->heap_kind = TURBOWASM_VALIDATION_HEAP_EXTERN;
            out->type_index = UINT32_MAX;
            return TURBOWASM_OK;
        case -23:
            out->carrier = 0x69u;
            out->is_reference = true;
            out->heap_kind = TURBOWASM_VALIDATION_HEAP_EXN;
            out->type_index = UINT32_MAX;
            return TURBOWASM_OK;
        case -12:
            out->carrier = 0x69u;
            out->is_reference = true;
            out->heap_kind = TURBOWASM_VALIDATION_HEAP_NOEXN;
            out->type_index = UINT32_MAX;
            return TURBOWASM_OK;
        default:
            return TURBOWASM_MALFORMED_MODULE;
    }
}

turbowasm_status turbowasm_validation_read_reftype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out,
    bool *out_generalized) {
    uint8_t first;
    turbowasm_status status;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->type_index = UINT32_MAX;
    if (out_generalized != NULL)
        *out_generalized = false;

    if (!turbowasm_reader_u8(reader, &first))
        return TURBOWASM_MALFORMED_MODULE;

    if (first == 0x70u || first == 0x6fu || first == 0x69u) {
        *out = turbowasm_validation_value_type_legacy(first);
        return TURBOWASM_OK;
    }

    if ((first >= 0x6au && first <= 0x6eu) ||
        (first >= 0x71u && first <= 0x74u)) {
        --reader->cursor;
        status = turbowasm_validation_read_heaptype(reader, context, out);
        if (status != TURBOWASM_OK)
            return status;
        out->nullable = true;
        return TURBOWASM_OK;
    }

    if (first != 0x63u && first != 0x64u)
        return TURBOWASM_MALFORMED_MODULE;

    out->nullable = first == 0x63u;
    status = turbowasm_validation_read_heaptype(reader, context, out);
    if (status != TURBOWASM_OK)
        return status;

    if (out_generalized != NULL)
        *out_generalized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validation_read_valtype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out,
    bool *out_generalized) {
    uint8_t first;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (turbowasm_reader_remaining(reader) == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    first = *reader->cursor;
    if (first == 0x7fu || first == 0x7eu ||
        first == 0x7du || first == 0x7cu ||
        first == 0x7bu) {
        if (!turbowasm_reader_u8(reader, &first))
            return TURBOWASM_MALFORMED_MODULE;
        *out = turbowasm_validation_value_type_legacy(first);
        if (out_generalized != NULL)
            *out_generalized = false;
        return TURBOWASM_OK;
    }

    return turbowasm_validation_read_reftype(
        reader, context, out, out_generalized);
}


turbowasm_status turbowasm_validation_read_globaltype(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out_type,
    bool *out_mutable) {
    turbowasm_validation_value_type type;
    bool generalized = false;
    uint8_t mutability;
    turbowasm_status status;

    if (reader == NULL || out_type == NULL || out_mutable == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_validation_read_valtype(
        reader, context, &type, &generalized);
    if (status != TURBOWASM_OK)
        return status;

    if (type.heap_kind == TURBOWASM_VALIDATION_HEAP_TYPE_INDEX &&
        (context == NULL || type.type_index >= context->type_count))
        return TURBOWASM_MALFORMED_MODULE;

    if (!turbowasm_reader_u8(reader, &mutability))
        return TURBOWASM_MALFORMED_MODULE;
    if (mutability > 1u)
        return TURBOWASM_MALFORMED_MODULE;

    *out_type = type;
    *out_mutable = mutability != 0u;
    return TURBOWASM_OK;
}
