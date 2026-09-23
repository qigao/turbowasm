#include "validate.h"
#include "validate_data.h"
#include "validate_element.h"
#include "validate_instr.h"
#include "validate_linkage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_SECTION_CUSTOM = 0u,
    TURBOWASM_SECTION_TYPE = 1u,
    TURBOWASM_SECTION_IMPORT = 2u,
    TURBOWASM_SECTION_FUNCTION = 3u,
    TURBOWASM_SECTION_TABLE = 4u,
    TURBOWASM_SECTION_MEMORY = 5u,
    TURBOWASM_SECTION_GLOBAL = 6u,
    TURBOWASM_SECTION_EXPORT = 7u,
    TURBOWASM_SECTION_START = 8u,
    TURBOWASM_SECTION_ELEMENT = 9u,
    TURBOWASM_SECTION_CODE = 10u,
    TURBOWASM_SECTION_DATA = 11u,
    TURBOWASM_SECTION_DATA_COUNT = 12u
};

static unsigned turbowasm_section_rank(uint8_t id) {
    switch (id) {
        case TURBOWASM_SECTION_TYPE: return 1u;
        case TURBOWASM_SECTION_IMPORT: return 2u;
        case TURBOWASM_SECTION_FUNCTION: return 3u;
        case TURBOWASM_SECTION_TABLE: return 4u;
        case TURBOWASM_SECTION_MEMORY: return 5u;
        case TURBOWASM_SECTION_GLOBAL: return 6u;
        case TURBOWASM_SECTION_EXPORT: return 7u;
        case TURBOWASM_SECTION_START: return 8u;
        case TURBOWASM_SECTION_ELEMENT: return 9u;
        case TURBOWASM_SECTION_DATA_COUNT: return 10u;
        case TURBOWASM_SECTION_CODE: return 11u;
        case TURBOWASM_SECTION_DATA: return 12u;
        default: return 0u;
    }
}

static bool turbowasm_valtype_supported(uint8_t type) {
    switch (type) {
        case 0x7fu: /* i32 */
        case 0x7eu: /* i64 */
        case 0x7du: /* f32 */
        case 0x7cu: /* f64 */
        case 0x7bu: /* v128 */
        case 0x70u: /* funcref */
        case 0x6fu: /* externref */
            return true;
        default:
            return false;
    }
}

static turbowasm_status turbowasm_read_valtype(
    turbowasm_reader *reader,
    uint8_t *out_type) {
    uint8_t type;

    if (!turbowasm_reader_u8(reader, &type))
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_valtype_supported(type))
        return TURBOWASM_UNSUPPORTED;
    if (out_type != NULL)
        *out_type = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_valtypes(
    turbowasm_reader *reader,
    uint8_t *values,
    uint32_t count) {
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        turbowasm_status status =
            turbowasm_read_valtype(reader, &values[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_type_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_validation_context_allocate_types(context, count))
        return TURBOWASM_OUT_OF_MEMORY;

    for (index = 0u; index < count; ++index) {
        uint8_t form;
        uint8_t *params = NULL;
        uint32_t param_count;
        uint32_t result_count;
        turbowasm_validation_func_type *type;
        turbowasm_status status;

        if (!turbowasm_reader_u8(section, &form))
            return TURBOWASM_MALFORMED_MODULE;
        if (form != 0x60u)
            return TURBOWASM_UNSUPPORTED;

        if (!turbowasm_reader_uleb32(section, &param_count))
            return TURBOWASM_MALFORMED_MODULE;
        if (param_count != 0u) {
            params = (uint8_t *)malloc((size_t)param_count);
            if (params == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }

        status = turbowasm_read_valtypes(
            section, params, param_count);
        if (status != TURBOWASM_OK) {
            free(params);
            return status;
        }

        if (!turbowasm_reader_uleb32(section, &result_count)) {
            free(params);
            return TURBOWASM_MALFORMED_MODULE;
        }

        if (!turbowasm_validation_context_define_type(
                context, index, param_count, result_count)) {
            free(params);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        type = turbowasm_validation_context_type_mut(context, index);
        if (type == NULL) {
            free(params);
            return TURBOWASM_MALFORMED_MODULE;
        }

        if (param_count != 0u)
            memcpy(type->params, params, (size_t)param_count);
        free(params);

        status = turbowasm_read_valtypes(
            section, type->results, result_count);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->type_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_function_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;
    uint32_t type_index;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        if (!turbowasm_reader_uleb32(section, &type_index))
            return TURBOWASM_MALFORMED_MODULE;
        if (type_index >= summary->type_count)
            return TURBOWASM_MALFORMED_MODULE;
        if (!turbowasm_validation_context_append_function(
                context, type_index, false))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->function_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_code_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    if (count != summary->function_count)
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        uint32_t body_size;
        uint32_t function_index =
            summary->imported_function_count + index;
        turbowasm_reader body;
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &body_size) ||
            !turbowasm_reader_slice(section, body_size, &body))
            return TURBOWASM_MALFORMED_MODULE;

        status = turbowasm_validate_function_body(
            &body, context, function_index);
        if (status != TURBOWASM_OK)
            return status;
        if (turbowasm_reader_remaining(&body) != 0u)
            return TURBOWASM_MALFORMED_MODULE;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->code_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_section_payload(
    uint8_t id,
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    switch (id) {
        case TURBOWASM_SECTION_TYPE:
            return turbowasm_validate_type_section(
                section, summary, context);
        case TURBOWASM_SECTION_IMPORT:
            return turbowasm_validate_import_section(
                section, summary, context);
        case TURBOWASM_SECTION_FUNCTION:
            return turbowasm_validate_function_section(
                section, summary, context);
        case TURBOWASM_SECTION_TABLE:
            return turbowasm_validate_table_section(
                section, summary, context);
        case TURBOWASM_SECTION_MEMORY:
            return turbowasm_validate_memory_section(
                section, summary, context);
        case TURBOWASM_SECTION_GLOBAL:
            return turbowasm_validate_global_section(
                section, summary, context);
        case TURBOWASM_SECTION_EXPORT:
            return turbowasm_validate_export_section(
                section, summary, context);
        case TURBOWASM_SECTION_START:
            return turbowasm_validate_start_section(
                section, summary, context);
        case TURBOWASM_SECTION_ELEMENT:
            return turbowasm_validate_element_section(
                section, summary, context);
        case TURBOWASM_SECTION_DATA_COUNT:
            return turbowasm_validate_data_count_section(
                section, summary, context);
        case TURBOWASM_SECTION_CODE:
            return turbowasm_validate_code_section(
                section, summary, context);
        case TURBOWASM_SECTION_DATA:
            return turbowasm_validate_data_section(
                section, summary, context);
        default:
            /* A bounded payload is not the same as a validated payload.
             * Standard sections without a semantic validator fail closed until
             * their #3 slice is implemented. */
            (void)section;
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_validate_sections(
    turbowasm_reader *reader,
    turbowasm_module_summary *summary,
    turbowasm_validation_context *context) {
    uint32_t seen = 0u;
    unsigned last_rank = 0u;

    if (reader == NULL || summary == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    while (turbowasm_reader_remaining(reader) != 0u) {
        uint8_t id;
        uint32_t payload_size;
        unsigned rank;
        turbowasm_reader section;
        turbowasm_status status;

        if (!turbowasm_reader_u8(reader, &id) ||
            !turbowasm_reader_uleb32(reader, &payload_size) ||
            !turbowasm_reader_slice(reader, payload_size, &section))
            return TURBOWASM_MALFORMED_MODULE;

        if (id == TURBOWASM_SECTION_CUSTOM) {
            ++summary->custom_section_count;
            continue;
        }

        rank = turbowasm_section_rank(id);
        if (rank == 0u)
            return TURBOWASM_UNSUPPORTED;
        if (rank < last_rank)
            return TURBOWASM_MALFORMED_MODULE;
        if ((seen & (UINT32_C(1) << id)) != 0u)
            return TURBOWASM_MALFORMED_MODULE;

        seen |= UINT32_C(1) << id;
        last_rank = rank;
        summary->standard_section_mask = seen;

        status = turbowasm_validate_section_payload(
            id, &section, summary, context);
        if (status != TURBOWASM_OK)
            return status;
        if (turbowasm_reader_remaining(&section) != 0u)
            return TURBOWASM_MALFORMED_MODULE;
    }

    if (summary->code_count != summary->function_count)
        return TURBOWASM_MALFORMED_MODULE;
    if (summary->has_data_count &&
        summary->data_count != summary->data_segment_count)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}
