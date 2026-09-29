#include "validate_linkage.h"
#include "validate_data.h"
#include "validate_type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool turbowasm_utf8_cont(uint8_t byte) {
    return byte >= 0x80u && byte <= 0xbfu;
}

static bool turbowasm_utf8_valid(const uint8_t *bytes, size_t size) {
    size_t i = 0u;

    while (i < size) {
        uint8_t a = bytes[i++];

        if (a <= 0x7fu)
            continue;

        if (a >= 0xc2u && a <= 0xdfu) {
            if (i >= size || !turbowasm_utf8_cont(bytes[i]))
                return false;
            ++i;
            continue;
        }

        if (a == 0xe0u) {
            if (i + 1u >= size || bytes[i] < 0xa0u || bytes[i] > 0xbfu ||
                !turbowasm_utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if ((a >= 0xe1u && a <= 0xecu) ||
            (a >= 0xeeu && a <= 0xefu)) {
            if (i + 1u >= size ||
                !turbowasm_utf8_cont(bytes[i]) ||
                !turbowasm_utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == 0xedu) {
            if (i + 1u >= size || bytes[i] < 0x80u || bytes[i] > 0x9fu ||
                !turbowasm_utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == 0xf0u) {
            if (i + 2u >= size || bytes[i] < 0x90u || bytes[i] > 0xbfu ||
                !turbowasm_utf8_cont(bytes[i + 1u]) ||
                !turbowasm_utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a >= 0xf1u && a <= 0xf3u) {
            if (i + 2u >= size ||
                !turbowasm_utf8_cont(bytes[i]) ||
                !turbowasm_utf8_cont(bytes[i + 1u]) ||
                !turbowasm_utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a == 0xf4u) {
            if (i + 2u >= size || bytes[i] < 0x80u || bytes[i] > 0x8fu ||
                !turbowasm_utf8_cont(bytes[i + 1u]) ||
                !turbowasm_utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        return false;
    }

    return true;
}

static turbowasm_status turbowasm_read_name(
    turbowasm_reader *reader,
    turbowasm_name *out) {
    uint32_t size;
    turbowasm_reader bytes;

    if (!turbowasm_reader_uleb32(reader, &size) ||
        !turbowasm_reader_slice(reader, size, &bytes))
        return TURBOWASM_MALFORMED_MODULE;

    if (!turbowasm_utf8_valid(bytes.cursor, size))
        return TURBOWASM_MALFORMED_MODULE;

    if (out != NULL) {
        out->bytes = bytes.cursor;
        out->size = size;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_limits(
    turbowasm_reader *reader,
    uint32_t maximum_bound,
    turbowasm_validation_limits *out) {
    uint8_t flags;
    uint32_t minimum;
    uint32_t maximum = 0u;

    if (!turbowasm_reader_u8(reader, &flags))
        return TURBOWASM_MALFORMED_MODULE;
    if (flags != 0x00u && flags != 0x01u)
        return TURBOWASM_UNSUPPORTED;

    if (!turbowasm_reader_uleb32(reader, &minimum))
        return TURBOWASM_MALFORMED_MODULE;
    if (maximum_bound != UINT32_MAX && minimum > maximum_bound)
        return TURBOWASM_MALFORMED_MODULE;

    if (flags == 0x01u) {
        if (!turbowasm_reader_uleb32(reader, &maximum))
            return TURBOWASM_MALFORMED_MODULE;
        if (maximum < minimum)
            return TURBOWASM_MALFORMED_MODULE;
        if (maximum_bound != UINT32_MAX && maximum > maximum_bound)
            return TURBOWASM_MALFORMED_MODULE;
    }

    if (out != NULL) {
        out->minimum = minimum;
        out->maximum = maximum;
        out->has_maximum = flags == 0x01u;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_table_type(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    turbowasm_validation_value_type *out_reference_type,
    turbowasm_validation_limits *out_limits) {
    turbowasm_validation_value_type reference_type;
    bool generalized = false;
    turbowasm_status status;

    status = turbowasm_validation_read_reftype(
        reader, &reference_type, &generalized);
    if (status != TURBOWASM_OK)
        return status;

    if (reference_type.heap_kind ==
            TURBOWASM_VALIDATION_HEAP_TYPE_INDEX &&
        (context == NULL ||
         reference_type.type_index >= context->type_count))
        return TURBOWASM_MALFORMED_MODULE;

    if (out_reference_type != NULL)
        *out_reference_type = reference_type;
    return turbowasm_read_limits(
        reader, UINT32_MAX, out_limits);
}

static turbowasm_status turbowasm_read_memory_type(
    turbowasm_reader *reader,
    turbowasm_validation_limits *out_limits,
    uint32_t *out_page_size,
    bool *out_shared) {
    uint8_t flags;
    uint32_t minimum;
    uint32_t maximum = 0u;
    uint32_t page_exponent = 16u;
    uint32_t page_size;
    uint32_t maximum_pages;
    bool has_maximum;
    bool shared;
    bool memory64;

    if (reader == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &flags))
        return TURBOWASM_MALFORMED_MODULE;

    /*
     * Memory limit flag bits:
     *   0x01 maximum present
     *   0x02 shared (threads)
     *   0x04 memory64
     *   0x08 custom page size
     *
     * T1 retains shared memory32 metadata only. memory64 remains explicitly
     * unsupported, and executable shared memory remains fail-closed until T2
     * establishes a host-language data-race-safe backing/access contract.
     */
    memory64 = (flags & 0x04u) != 0u;
    if (memory64)
        return TURBOWASM_UNSUPPORTED;
    if ((flags & (uint8_t)~0x0bu) != 0u)
        return TURBOWASM_UNSUPPORTED;

    has_maximum = (flags & 0x01u) != 0u;
    shared = (flags & 0x02u) != 0u;
    if (shared && !has_maximum)
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_reader_uleb32(reader, &minimum))
        return TURBOWASM_MALFORMED_MODULE;

    if (has_maximum) {
        if (!turbowasm_reader_uleb32(reader, &maximum))
            return TURBOWASM_MALFORMED_MODULE;
        if (maximum < minimum)
            return TURBOWASM_MALFORMED_MODULE;
    }

    if ((flags & 0x08u) != 0u) {
        if (!turbowasm_reader_uleb32(reader, &page_exponent))
            return TURBOWASM_MALFORMED_MODULE;

        /*
         * The current proposal validates exactly 1-byte and 64-KiB pages.
         * Keep other exponents malformed rather than silently generalizing
         * beyond the proposal's current semantic surface.
         */
        if (page_exponent != 0u && page_exponent != 16u)
            return TURBOWASM_MALFORMED_MODULE;
    }

    page_size = page_exponent == 0u
        ? UINT32_C(1)
        : UINT32_C(65536);
    maximum_pages = page_size == UINT32_C(1)
        ? UINT32_MAX
        : UINT32_C(65536);

    if (minimum > maximum_pages ||
        (has_maximum && maximum > maximum_pages))
        return TURBOWASM_MALFORMED_MODULE;

    if (out_limits != NULL) {
        out_limits->minimum = minimum;
        out_limits->maximum = maximum;
        out_limits->has_maximum = has_maximum;
    }
    if (out_page_size != NULL)
        *out_page_size = page_size;
    if (out_shared != NULL)
        *out_shared = shared;

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_tag_type(
    turbowasm_reader *reader,
    const turbowasm_validation_context *context,
    uint32_t *out_type_index) {
    uint8_t attribute;
    uint32_t type_index;
    const turbowasm_validation_func_type *type;

    if (reader == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &attribute) ||
        !turbowasm_reader_uleb32(reader, &type_index))
        return TURBOWASM_MALFORMED_MODULE;
    if (attribute != 0x00u)
        return TURBOWASM_MALFORMED_MODULE;

    type = turbowasm_validation_context_type(context, type_index);
    if (type == NULL || !type->defined || type->result_count != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    if (out_type_index != NULL)
        *out_type_index = type_index;
    return TURBOWASM_OK;
}

static bool turbowasm_index_in_total(
    uint32_t index,
    uint32_t imported,
    uint32_t defined) {
    uint64_t total = (uint64_t)imported + (uint64_t)defined;
    return (uint64_t)index < total;
}

turbowasm_status turbowasm_validate_import_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_import_desc import_desc = {0};
        uint8_t kind;
        uint32_t type_index;
        turbowasm_status status;

        import_desc.type_index = UINT32_MAX;
        status = turbowasm_read_name(
            section, &import_desc.module_name);
        if (status != TURBOWASM_OK) return status;
        status = turbowasm_read_name(
            section, &import_desc.name);
        if (status != TURBOWASM_OK) return status;
        if (!turbowasm_reader_u8(section, &kind))
            return TURBOWASM_MALFORMED_MODULE;
        import_desc.kind = (turbowasm_external_kind)kind;

        switch (kind) {
            case 0x00u:
                if (!turbowasm_reader_uleb32(section, &type_index))
                    return TURBOWASM_MALFORMED_MODULE;
                if (type_index >= summary->type_count)
                    return TURBOWASM_MALFORMED_MODULE;
                import_desc.item_index =
                    summary->imported_function_count;
                import_desc.type_index = type_index;
                if (!turbowasm_validation_context_append_function(
                        context, type_index, true))
                    return TURBOWASM_OUT_OF_MEMORY;
                ++summary->imported_function_count;
                break;
            case 0x01u: {
                turbowasm_validation_value_type reference_type;
                turbowasm_validation_limits limits = {0};
                import_desc.item_index =
                    summary->imported_table_count;
                status = turbowasm_read_table_type(
                    section, context, &reference_type, &limits);
                if (status != TURBOWASM_OK) return status;
                if (!turbowasm_validation_context_append_table_semantic(
                        context, reference_type, limits, true))
                    return TURBOWASM_OUT_OF_MEMORY;
                ++summary->imported_table_count;
                break;
            }
            case 0x02u: {
                turbowasm_validation_limits limits = {0};
                uint32_t page_size = 0u;
                bool shared = false;
                import_desc.item_index =
                    summary->imported_memory_count;
                status = turbowasm_read_memory_type(
                    section, &limits, &page_size, &shared);
                if (status != TURBOWASM_OK) return status;
                if (!turbowasm_validation_context_append_memory(
                        context, limits, page_size, shared, true))
                    return TURBOWASM_OUT_OF_MEMORY;
                ++summary->imported_memory_count;
                break;
            }
            case 0x03u: {
                turbowasm_validation_value_type value_type;
                bool mutable_value;
                import_desc.item_index =
                    summary->imported_global_count;
                status = turbowasm_validation_read_globaltype(
                    section, context, &value_type, &mutable_value);
                if (status != TURBOWASM_OK) return status;
                if (!turbowasm_validation_context_append_global_semantic(
                        context, value_type, mutable_value, true,
                        NULL, 0u))
                    return TURBOWASM_OUT_OF_MEMORY;
                ++summary->imported_global_count;
                break;
            }
            case 0x04u: {
                import_desc.item_index =
                    summary->imported_tag_count;
                status = turbowasm_read_tag_type(
                    section, context, &type_index);
                if (status != TURBOWASM_OK)
                    return status;
                import_desc.type_index = type_index;
                if (!turbowasm_validation_context_append_tag(
                        context, type_index, true))
                    return TURBOWASM_OUT_OF_MEMORY;
                ++summary->imported_tag_count;
                break;
            }
            default:
                return TURBOWASM_UNSUPPORTED;
        }

        if (!turbowasm_validation_context_append_import(
                context, import_desc))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    return turbowasm_reader_remaining(section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

turbowasm_status turbowasm_validate_table_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_validation_value_type reference_type;
        turbowasm_validation_limits limits = {0};
        turbowasm_status status;
        bool explicit_initializer = false;

        if (turbowasm_reader_remaining(section) != 0u &&
            *section->cursor == 0x40u) {
            uint8_t marker;
            uint8_t zero;
            const uint8_t *initializer_start;
            size_t initializer_size;
            turbowasm_validation_value_type initializer_type;

            if (!turbowasm_reader_u8(section, &marker) ||
                !turbowasm_reader_u8(section, &zero) ||
                marker != 0x40u || zero != 0x00u)
                return TURBOWASM_MALFORMED_MODULE;

            status = turbowasm_read_table_type(
                section, context, &reference_type, &limits);
            if (status != TURBOWASM_OK)
                return status;

            initializer_start = section->cursor;
            status = turbowasm_validate_const_expr_semantic(
                section, context, &initializer_type);
            if (status != TURBOWASM_OK)
                return status;
            if (!turbowasm_validation_value_type_matches(
                    &initializer_type, &reference_type))
                return TURBOWASM_MALFORMED_MODULE;

            initializer_size =
                (size_t)(section->cursor - initializer_start);
            if (initializer_size == 0u ||
                initializer_size > UINT32_MAX)
                return TURBOWASM_MALFORMED_MODULE;

            if (!turbowasm_validation_context_append_table_semantic_initialized(
                    context, reference_type, limits,
                    initializer_start,
                    (uint32_t)initializer_size))
                return TURBOWASM_OUT_OF_MEMORY;
            explicit_initializer = true;
        }

        if (explicit_initializer)
            continue;

        status = turbowasm_read_table_type(
            section, context, &reference_type, &limits);
        if (status != TURBOWASM_OK)
            return status;

        /*
         * Without an explicit initializer, the binary format synthesizes
         * ref.null(heaptype). Non-nullable tables therefore require the
         * explicit 0x40 0x00 form above.
         */
        if (reference_type.is_reference && !reference_type.nullable)
            return TURBOWASM_MALFORMED_MODULE;

        if (!turbowasm_validation_context_append_table_semantic(
                context, reference_type, limits, false))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->table_count = count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_memory_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_validation_limits limits = {0};
        uint32_t page_size = 0u;
        bool shared = false;
        turbowasm_status status = turbowasm_read_memory_type(
            section, &limits, &page_size, &shared);
        if (status != TURBOWASM_OK) return status;
        if (!turbowasm_validation_context_append_memory(
                context, limits, page_size, shared, false))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->memory_count = count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_tag_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        uint32_t type_index;
        turbowasm_status status = turbowasm_read_tag_type(
            section, context, &type_index);
        if (status != TURBOWASM_OK)
            return status;
        if (!turbowasm_validation_context_append_tag(
                context, type_index, false))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->tag_count = count;
    return TURBOWASM_OK;
}

static bool turbowasm_export_index_valid(
    uint8_t kind,
    uint32_t index,
    const turbowasm_module_summary *summary) {
    switch (kind) {
        case 0x00u:
            return turbowasm_index_in_total(
                index, summary->imported_function_count,
                summary->function_count);
        case 0x01u:
            return turbowasm_index_in_total(
                index, summary->imported_table_count,
                summary->table_count);
        case 0x02u:
            return turbowasm_index_in_total(
                index, summary->imported_memory_count,
                summary->memory_count);
        case 0x03u:
            return turbowasm_index_in_total(
                index, summary->imported_global_count,
                summary->global_count);
        case 0x04u:
            return turbowasm_index_in_total(
                index, summary->imported_tag_count,
                summary->tag_count);
        default:
            return false;
    }
}

turbowasm_status turbowasm_validate_export_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_export_desc export_desc = {0};
        uint8_t kind;
        uint32_t item_index;
        uint32_t previous;
        turbowasm_status status = turbowasm_read_name(
            section, &export_desc.name);

        if (status != TURBOWASM_OK)
            return status;

        for (previous = 0u;
             previous < context->export_count;
             ++previous) {
            const turbowasm_export_desc *retained =
                &context->exports[previous];
            if (retained->name.size == export_desc.name.size &&
                memcmp(retained->name.bytes,
                       export_desc.name.bytes,
                       export_desc.name.size) == 0)
                return TURBOWASM_MALFORMED_MODULE;
        }

        if (!turbowasm_reader_u8(section, &kind) ||
            !turbowasm_reader_uleb32(section, &item_index))
            return TURBOWASM_MALFORMED_MODULE;
        if (kind > 0x04u)
            return TURBOWASM_UNSUPPORTED;
        if (!turbowasm_export_index_valid(
                kind, item_index, summary))
            return TURBOWASM_MALFORMED_MODULE;

        export_desc.kind = (turbowasm_external_kind)kind;
        export_desc.item_index = item_index;
        if (!turbowasm_validation_context_append_export(
                context, export_desc))
            return TURBOWASM_OUT_OF_MEMORY;

        if (kind == 0x00u &&
            !turbowasm_validation_context_declare_function_ref(
                context, item_index))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->export_count = count;
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_validate_start_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_validation_context *context) {
    uint32_t function_index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &function_index) ||
        turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    {
        const turbowasm_validation_func_type *type =
            turbowasm_validation_context_function_type(
                context, function_index);
        if (type == NULL)
            return TURBOWASM_MALFORMED_MODULE;
        if (type->param_count != 0u || type->result_count != 0u)
            return TURBOWASM_MALFORMED_MODULE;
    }

    summary->has_start = true;
    summary->start_function_index = function_index;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_data_count_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count) ||
        turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->has_data_count = true;
    summary->data_count = count;
    context->has_data_count = true;
    context->data_count = count;
    return TURBOWASM_OK;
}
