#include "validate_element.h"

#include "validate_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    TURBOWASM_ELEMENT_I32 = 0x7f,
    TURBOWASM_ELEMENT_FUNCREF = 0x70,
    TURBOWASM_ELEMENT_EXTERNREF = 0x6f
};

static turbowasm_status turbowasm_element_read_reftype(
    turbowasm_reader *reader,
    uint8_t *out_type) {
    uint8_t type;

    if (reader == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &type))
        return TURBOWASM_MALFORMED_MODULE;
    if (type != TURBOWASM_ELEMENT_FUNCREF &&
        type != TURBOWASM_ELEMENT_EXTERNREF)
        return TURBOWASM_UNSUPPORTED;

    *out_type = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_read_elemkind(
    turbowasm_reader *reader,
    uint8_t *out_type) {
    uint8_t kind;

    if (reader == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &kind))
        return TURBOWASM_MALFORMED_MODULE;
    if (kind != 0x00u)
        return TURBOWASM_UNSUPPORTED;

    *out_type = TURBOWASM_ELEMENT_FUNCREF;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_validate_offset(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    turbowasm_validation_expr_span *out) {
    const uint8_t *start;
    size_t size;
    uint8_t type;
    turbowasm_status status;

    if (reader == NULL || context == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    start = reader->cursor;
    status = turbowasm_validate_const_expr(
        reader, context, &type);
    if (status != TURBOWASM_OK)
        return status;
    if (type != TURBOWASM_ELEMENT_I32)
        return TURBOWASM_MALFORMED_MODULE;

    size = (size_t)(reader->cursor - start);
    if (size > UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    out->bytes = start;
    out->size = (uint32_t)size;
    out->result_type = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_validate_table(
    const turbowasm_validation_context *context,
    uint32_t table_index,
    uint8_t reference_type) {
    if (context == NULL || table_index >= context->table_count)
        return TURBOWASM_MALFORMED_MODULE;
    return context->tables[table_index].reference_type == reference_type
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status turbowasm_element_validate_funcidx_vector(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    turbowasm_validation_element_item **out_items,
    uint32_t *out_count) {
    turbowasm_validation_element_item *items = NULL;
    uint32_t count;
    uint32_t index;

    if (out_items == NULL || out_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    if (count != 0u) {
        if ((uint64_t)count * sizeof(*items) > (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_validation_element_item *)calloc(
            (size_t)count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < count; ++index) {
        uint32_t function_index;
        if (!turbowasm_reader_uleb32(reader, &function_index)) {
            free(items);
            return TURBOWASM_MALFORMED_MODULE;
        }
        if (function_index >= context->function_count) {
            free(items);
            return TURBOWASM_MALFORMED_MODULE;
        }
        if (!turbowasm_validation_context_declare_function_ref(
                context, function_index)) {
            free(items);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        items[index].kind =
            TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX;
        items[index].function_index = function_index;
    }

    *out_items = items;
    *out_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_validate_expr_vector(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    uint8_t reference_type,
    turbowasm_validation_element_item **out_items,
    uint32_t *out_count) {
    turbowasm_validation_element_item *items = NULL;
    uint32_t count;
    uint32_t index;

    if (out_items == NULL || out_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    if (count != 0u) {
        if ((uint64_t)count * sizeof(*items) > (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_validation_element_item *)calloc(
            (size_t)count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < count; ++index) {
        const uint8_t *start = reader->cursor;
        size_t size;
        uint8_t type;
        turbowasm_status status = turbowasm_validate_const_expr(
            reader, context, &type);

        if (status != TURBOWASM_OK) {
            free(items);
            return status;
        }
        if (type != reference_type) {
            free(items);
            return TURBOWASM_MALFORMED_MODULE;
        }

        size = (size_t)(reader->cursor - start);
        if (size > UINT32_MAX) {
            free(items);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        items[index].kind =
            TURBOWASM_VALIDATION_ELEMENT_CONST_EXPR;
        items[index].expression.bytes = start;
        items[index].expression.size = (uint32_t)size;
        items[index].expression.result_type = type;
    }

    *out_items = items;
    *out_count = count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_element_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t segment;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (segment = 0u; segment < count; ++segment) {
        uint32_t flags;
        bool active;
        bool expression_items;
        turbowasm_validation_element_segment descriptor = {0};
        turbowasm_status status;

        descriptor.reference_type = TURBOWASM_ELEMENT_FUNCREF;

        if (!turbowasm_reader_uleb32(section, &flags))
            return TURBOWASM_MALFORMED_MODULE;
        if (flags > 7u)
            return TURBOWASM_UNSUPPORTED;

        active = flags == 0u || flags == 2u ||
                 flags == 4u || flags == 6u;
        expression_items = flags >= 4u;

        descriptor.mode = active
            ? TURBOWASM_VALIDATION_SEGMENT_ACTIVE
            : (flags == 3u || flags == 7u)
                ? TURBOWASM_VALIDATION_SEGMENT_DECLARATIVE
                : TURBOWASM_VALIDATION_SEGMENT_PASSIVE;

        if (flags == 2u || flags == 6u) {
            if (!turbowasm_reader_uleb32(
                    section, &descriptor.table_index))
                return TURBOWASM_MALFORMED_MODULE;
        }

        if (active) {
            status = turbowasm_element_validate_offset(
                section, context, &descriptor.offset);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (flags == 1u || flags == 2u || flags == 3u) {
            status = turbowasm_element_read_elemkind(
                section, &descriptor.reference_type);
            if (status != TURBOWASM_OK)
                return status;
        } else if (flags == 5u || flags == 6u || flags == 7u) {
            status = turbowasm_element_read_reftype(
                section, &descriptor.reference_type);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (active) {
            status = turbowasm_element_validate_table(
                context, descriptor.table_index,
                descriptor.reference_type);
            if (status != TURBOWASM_OK)
                return status;
        }

        status = expression_items
            ? turbowasm_element_validate_expr_vector(
                  section, context, descriptor.reference_type,
                  &descriptor.items, &descriptor.item_count)
            : turbowasm_element_validate_funcidx_vector(
                  section, context,
                  &descriptor.items, &descriptor.item_count);
        if (status != TURBOWASM_OK) {
            free(descriptor.items);
            return status;
        }

        if (!turbowasm_validation_context_append_element_segment(
                context, descriptor)) {
            free(descriptor.items);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->element_count = count;
    return TURBOWASM_OK;
}
