#include "validate.h"
#include "validate_linkage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static turbowasm_status turbowasm_read_valtype_vector_alloc(
    turbowasm_reader *reader,
    uint8_t **out_values,
    uint32_t *out_count) {
    uint32_t count;
    uint32_t index;
    uint8_t *values = NULL;
    turbowasm_status status;

    if (out_values == NULL || out_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &count))
        return TURBOWASM_MALFORMED_MODULE;

    if (count != 0u) {
        values = (uint8_t *)malloc((size_t)count);
        if (values == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < count; ++index) {
        status = turbowasm_read_valtype(reader, &values[index]);
        if (status != TURBOWASM_OK) {
            free(values);
            return status;
        }
    }

    *out_values = values;
    *out_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_type_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir) {
    uint32_t count;
    uint32_t index;
    uint8_t form;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_module_ir_allocate_types(ir, count))
        return TURBOWASM_OUT_OF_MEMORY;

    for (index = 0u; index < count; ++index) {
        turbowasm_ir_func_type *type = &ir->types[index];

        if (!turbowasm_reader_u8(section, &form))
            return TURBOWASM_MALFORMED_MODULE;
        if (form != 0x60u)
            return TURBOWASM_UNSUPPORTED;

        status = turbowasm_read_valtype_vector_alloc(
            section, &type->params, &type->param_count);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_read_valtype_vector_alloc(
            section, &type->results, &type->result_count);
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
    turbowasm_module_ir *ir) {
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
        if (!turbowasm_module_ir_append_function(
                ir, (turbowasm_ir_function){type_index, false}))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->function_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_local_decls(
    turbowasm_reader *body) {
    uint32_t group_count;
    uint32_t group_index;
    uint32_t local_count;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(body, &group_count))
        return TURBOWASM_MALFORMED_MODULE;

    for (group_index = 0u; group_index < group_count; ++group_index) {
        if (!turbowasm_reader_uleb32(body, &local_count))
            return TURBOWASM_MALFORMED_MODULE;
        (void)local_count;
        status = turbowasm_read_valtype(body, NULL);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_code_body(
    turbowasm_reader *body) {
    turbowasm_status status = turbowasm_validate_local_decls(body);
    if (status != TURBOWASM_OK)
        return status;

    if (turbowasm_reader_remaining(body) == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    /* Full instruction/control-stack validation is the next phase. For this
     * framing slice, require the function expression to terminate with END. */
    if (body->end[-1] != 0x0bu)
        return TURBOWASM_MALFORMED_MODULE;

    body->cursor = body->end;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_code_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary) {
    uint32_t count;
    uint32_t index;
    uint32_t body_size;
    turbowasm_reader body;
    turbowasm_status status;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    if (count != summary->function_count)
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        if (!turbowasm_reader_uleb32(section, &body_size) ||
            !turbowasm_reader_slice(section, body_size, &body))
            return TURBOWASM_MALFORMED_MODULE;
        status = turbowasm_validate_code_body(&body);
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
    turbowasm_module_ir *ir) {
    switch (id) {
        case TURBOWASM_SECTION_TYPE:
            return turbowasm_validate_type_section(section, summary, ir);
        case TURBOWASM_SECTION_IMPORT:
            return turbowasm_validate_import_section(section, summary, ir);
        case TURBOWASM_SECTION_FUNCTION:
            return turbowasm_validate_function_section(section, summary, ir);
        case TURBOWASM_SECTION_TABLE:
            return turbowasm_validate_table_section(section, summary, ir);
        case TURBOWASM_SECTION_MEMORY:
            return turbowasm_validate_memory_section(section, summary, ir);
        case TURBOWASM_SECTION_EXPORT:
            return turbowasm_validate_export_section(section, summary);
        case TURBOWASM_SECTION_DATA_COUNT:
            return turbowasm_validate_data_count_section(section, summary);
        case TURBOWASM_SECTION_CODE:
            return turbowasm_validate_code_section(section, summary);
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
    turbowasm_module_ir *ir) {
    uint32_t seen = 0u;
    unsigned last_rank = 0u;

    if (reader == NULL || summary == NULL || ir == NULL)
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

        status = turbowasm_validate_section_payload(id, &section, summary, ir);
        if (status != TURBOWASM_OK)
            return status;
        if (turbowasm_reader_remaining(&section) != 0u)
            return TURBOWASM_MALFORMED_MODULE;
    }

    if (summary->code_count != summary->function_count)
        return TURBOWASM_MALFORMED_MODULE;
    if (summary->has_data_count &&
        (summary->standard_section_mask &
         (UINT32_C(1) << TURBOWASM_SECTION_DATA)) == 0u &&
        summary->data_count != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}
