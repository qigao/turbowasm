#include "validate_element.h"

#include "validate_data.h"

#include <stdbool.h>
#include <stdint.h>

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
    turbowasm_validation_context *context) {
    uint8_t type;
    turbowasm_status status = turbowasm_validate_const_expr(
        reader, context, &type);

    if (status != TURBOWASM_OK)
        return status;
    return type == TURBOWASM_ELEMENT_I32
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
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
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        uint32_t function_index;
        if (!turbowasm_reader_uleb32(reader, &function_index))
            return TURBOWASM_MALFORMED_MODULE;
        if (function_index >= context->function_count)
            return TURBOWASM_MALFORMED_MODULE;
        if (!turbowasm_validation_context_declare_function_ref(
                context, function_index))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_validate_expr_vector(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    uint8_t reference_type) {
    uint32_t count;
    uint32_t index;

    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        uint8_t type;
        turbowasm_status status = turbowasm_validate_const_expr(
            reader, context, &type);
        if (status != TURBOWASM_OK)
            return status;
        if (type != reference_type)
            return TURBOWASM_MALFORMED_MODULE;
    }

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
        uint32_t table_index = 0u;
        uint8_t reference_type = TURBOWASM_ELEMENT_FUNCREF;
        bool active;
        bool expression_items;
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &flags))
            return TURBOWASM_MALFORMED_MODULE;
        if (flags > 7u)
            return TURBOWASM_UNSUPPORTED;

        active = flags == 0u || flags == 2u ||
                 flags == 4u || flags == 6u;
        expression_items = flags >= 4u;

        if (flags == 2u || flags == 6u) {
            if (!turbowasm_reader_uleb32(section, &table_index))
                return TURBOWASM_MALFORMED_MODULE;
        }

        if (active) {
            status = turbowasm_element_validate_offset(
                section, context);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (flags == 1u || flags == 2u || flags == 3u) {
            status = turbowasm_element_read_elemkind(
                section, &reference_type);
            if (status != TURBOWASM_OK)
                return status;
        } else if (flags == 5u || flags == 6u || flags == 7u) {
            status = turbowasm_element_read_reftype(
                section, &reference_type);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (active) {
            status = turbowasm_element_validate_table(
                context, table_index, reference_type);
            if (status != TURBOWASM_OK)
                return status;
        }

        status = expression_items
            ? turbowasm_element_validate_expr_vector(
                  section, context, reference_type)
            : turbowasm_element_validate_funcidx_vector(
                  section, context);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->element_count = count;
    return TURBOWASM_OK;
}
