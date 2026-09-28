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
    turbowasm_validation_value_type *out) {
    int64_t heap;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_validation_read_s33(reader, &heap))
        return TURBOWASM_MALFORMED_MODULE;

    if (heap >= 0) {
        if ((uint64_t)heap > UINT32_MAX)
            return TURBOWASM_MALFORMED_MODULE;
        out->carrier = 0x70u;
        out->is_reference = true;
        out->heap_kind = TURBOWASM_VALIDATION_HEAP_TYPE_INDEX;
        out->type_index = (uint32_t)heap;
        return TURBOWASM_OK;
    }

    switch (heap) {
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
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_validation_read_reftype(
    turbowasm_reader *reader,
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

    if (first == 0x70u || first == 0x6fu) {
        *out = turbowasm_validation_value_type_legacy(first);
        return TURBOWASM_OK;
    }

    if (first != 0x63u && first != 0x64u)
        return TURBOWASM_UNSUPPORTED;

    out->nullable = first == 0x63u;
    status = turbowasm_validation_read_heaptype(reader, out);
    if (status != TURBOWASM_OK)
        return status;

    if (out_generalized != NULL)
        *out_generalized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validation_read_valtype(
    turbowasm_reader *reader,
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
        reader, out, out_generalized);
}
