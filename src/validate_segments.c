#include "validate_segments.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_TYPE_I32 = 0x7f,
    TURBOWASM_TYPE_I64 = 0x7e,
    TURBOWASM_TYPE_F32 = 0x7d,
    TURBOWASM_TYPE_F64 = 0x7c,
    TURBOWASM_TYPE_V128 = 0x7b,
    TURBOWASM_TYPE_FUNCREF = 0x70,
    TURBOWASM_TYPE_EXTERNREF = 0x6f
};

static bool turbowasm_segment_valtype_supported(uint8_t type) {
    switch (type) {
        case TURBOWASM_TYPE_I32:
        case TURBOWASM_TYPE_I64:
        case TURBOWASM_TYPE_F32:
        case TURBOWASM_TYPE_F64:
        case TURBOWASM_TYPE_V128:
        case TURBOWASM_TYPE_FUNCREF:
        case TURBOWASM_TYPE_EXTERNREF:
            return true;
        default:
            return false;
    }
}

static turbowasm_status turbowasm_read_global_type(
    turbowasm_reader *reader,
    turbowasm_ir_global *out) {
    uint8_t type;
    uint8_t mutability;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &type) ||
        !turbowasm_reader_u8(reader, &mutability))
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_segment_valtype_supported(type))
        return TURBOWASM_UNSUPPORTED;
    if (mutability > 1u)
        return TURBOWASM_MALFORMED_MODULE;

    out->value_type = type;
    out->mutable_value = mutability != 0u;
    out->imported = false;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_ref_type(
    turbowasm_reader *reader,
    uint8_t *out) {
    uint8_t type;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &type))
        return TURBOWASM_MALFORMED_MODULE;
    if (type != TURBOWASM_TYPE_FUNCREF &&
        type != TURBOWASM_TYPE_EXTERNREF)
        return TURBOWASM_UNSUPPORTED;
    *out = type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_const_expr(
    turbowasm_reader *reader,
    const turbowasm_module_ir *ir,
    uint8_t *out_type) {
    uint8_t opcode;
    uint8_t result_type = 0u;

    if (reader == NULL || ir == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (opcode) {
        case 0x41u: {
            int32_t value;
            if (!turbowasm_reader_sleb32(reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            (void)value;
            result_type = TURBOWASM_TYPE_I32;
            break;
        }
        case 0x42u: {
            int64_t value;
            if (!turbowasm_reader_sleb64(reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            (void)value;
            result_type = TURBOWASM_TYPE_I64;
            break;
        }
        case 0x43u: {
            turbowasm_reader immediate;
            if (!turbowasm_reader_slice(reader, 4u, &immediate))
                return TURBOWASM_MALFORMED_MODULE;
            result_type = TURBOWASM_TYPE_F32;
            break;
        }
        case 0x44u: {
            turbowasm_reader immediate;
            if (!turbowasm_reader_slice(reader, 8u, &immediate))
                return TURBOWASM_MALFORMED_MODULE;
            result_type = TURBOWASM_TYPE_F64;
            break;
        }
        case 0x23u: {
            uint32_t index;
            const turbowasm_ir_global *global;
            if (!turbowasm_reader_uleb32(reader, &index))
                return TURBOWASM_MALFORMED_MODULE;
            if (index >= ir->global_count)
                return TURBOWASM_MALFORMED_MODULE;
            global = &ir->globals[index];
            if (!global->imported || global->mutable_value)
                return TURBOWASM_MALFORMED_MODULE;
            result_type = global->value_type;
            break;
        }
        case 0xd0u: {
            turbowasm_status status =
                turbowasm_read_ref_type(reader, &result_type);
            if (status != TURBOWASM_OK)
                return status;
            break;
        }
        case 0xd2u: {
            uint32_t function_index;
            if (!turbowasm_reader_uleb32(reader, &function_index))
                return TURBOWASM_MALFORMED_MODULE;
            if (function_index >= ir->function_count)
                return TURBOWASM_MALFORMED_MODULE;
            result_type = TURBOWASM_TYPE_FUNCREF;
            break;
        }
        case 0xfdu: {
            uint32_t subopcode;
            turbowasm_reader immediate;
            if (!turbowasm_reader_uleb32(reader, &subopcode))
                return TURBOWASM_MALFORMED_MODULE;
            if (subopcode != 12u)
                return TURBOWASM_UNSUPPORTED;
            if (!turbowasm_reader_slice(reader, 16u, &immediate))
                return TURBOWASM_MALFORMED_MODULE;
            result_type = TURBOWASM_TYPE_V128;
            break;
        }
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!turbowasm_reader_u8(reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;
    if (opcode != 0x0bu)
        return TURBOWASM_UNSUPPORTED;

    *out_type = result_type;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_expected_const_expr(
    turbowasm_reader *reader,
    const turbowasm_module_ir *ir,
    uint8_t expected_type) {
    uint8_t actual_type;
    turbowasm_status status =
        turbowasm_validate_const_expr(reader, ir, &actual_type);
    if (status != TURBOWASM_OK)
        return status;
    return actual_type == expected_type
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

turbowasm_status turbowasm_validate_global_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    turbowasm_module_ir *ir) {
    uint32_t count;
    uint32_t index;

    if (section == NULL || summary == NULL || ir == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (index = 0u; index < count; ++index) {
        turbowasm_ir_global global = {0};
        turbowasm_status status = turbowasm_read_global_type(
            section, &global);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_validate_expected_const_expr(
            section, ir, global.value_type);
        if (status != TURBOWASM_OK)
            return status;

        if (!turbowasm_module_ir_append_global(ir, global))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->global_count = count;
    return TURBOWASM_OK;
}

static bool turbowasm_table_accepts(
    const turbowasm_module_ir *ir,
    uint32_t table_index,
    uint8_t reference_type) {
    return ir != NULL &&
           table_index < ir->table_count &&
           ir->tables[table_index].reference_type == reference_type;
}

static turbowasm_status turbowasm_validate_function_index_vector(
    turbowasm_reader *section,
    const turbowasm_module_ir *ir) {
    uint32_t count;
    uint32_t index;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    for (index = 0u; index < count; ++index) {
        uint32_t function_index;
        if (!turbowasm_reader_uleb32(section, &function_index))
            return TURBOWASM_MALFORMED_MODULE;
        if (function_index >= ir->function_count)
            return TURBOWASM_MALFORMED_MODULE;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_element_expr_vector(
    turbowasm_reader *section,
    const turbowasm_module_ir *ir,
    uint8_t reference_type) {
    uint32_t count;
    uint32_t index;

    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;
    for (index = 0u; index < count; ++index) {
        turbowasm_status status = turbowasm_validate_expected_const_expr(
            section, ir, reference_type);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_validate_elemkind(
    turbowasm_reader *section) {
    uint8_t kind;
    if (!turbowasm_reader_u8(section, &kind))
        return TURBOWASM_MALFORMED_MODULE;
    return kind == 0x00u
        ? TURBOWASM_OK
        : TURBOWASM_UNSUPPORTED;
}

turbowasm_status turbowasm_validate_element_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_module_ir *ir) {
    uint32_t count;
    uint32_t segment;

    if (section == NULL || summary == NULL || ir == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (segment = 0u; segment < count; ++segment) {
        uint32_t flags;
        uint32_t table_index = 0u;
        uint8_t reference_type = TURBOWASM_TYPE_FUNCREF;
        bool active = false;
        bool expressions = false;
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &flags))
            return TURBOWASM_MALFORMED_MODULE;
        if (flags > 7u)
            return TURBOWASM_UNSUPPORTED;

        active = flags == 0u || flags == 2u ||
                 flags == 4u || flags == 6u;
        expressions = flags >= 4u;

        if (flags == 2u || flags == 6u) {
            if (!turbowasm_reader_uleb32(section, &table_index))
                return TURBOWASM_MALFORMED_MODULE;
        }

        if (active) {
            if (table_index >= ir->table_count)
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_validate_expected_const_expr(
                section, ir, TURBOWASM_TYPE_I32);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (flags == 1u || flags == 2u || flags == 3u) {
            status = turbowasm_validate_elemkind(section);
            if (status != TURBOWASM_OK)
                return status;
        } else if (flags == 5u || flags == 6u || flags == 7u) {
            status = turbowasm_read_ref_type(section, &reference_type);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (active &&
            !turbowasm_table_accepts(ir, table_index, reference_type))
            return TURBOWASM_MALFORMED_MODULE;

        status = expressions
            ? turbowasm_validate_element_expr_vector(
                  section, ir, reference_type)
            : turbowasm_validate_function_index_vector(section, ir);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->element_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_skip_byte_vector(
    turbowasm_reader *section) {
    uint32_t size;
    turbowasm_reader bytes;

    if (!turbowasm_reader_uleb32(section, &size) ||
        !turbowasm_reader_slice(section, size, &bytes))
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_data_section(
    turbowasm_reader *section,
    turbowasm_module_summary *summary,
    const turbowasm_module_ir *ir) {
    uint32_t count;
    uint32_t segment;

    if (section == NULL || summary == NULL || ir == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (segment = 0u; segment < count; ++segment) {
        uint32_t flags;
        uint32_t memory_index = 0u;
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &flags))
            return TURBOWASM_MALFORMED_MODULE;
        if (flags > 2u)
            return TURBOWASM_UNSUPPORTED;

        if (flags == 2u) {
            if (!turbowasm_reader_uleb32(section, &memory_index))
                return TURBOWASM_MALFORMED_MODULE;
        }

        if (flags == 0u || flags == 2u) {
            if (memory_index >= ir->memory_count)
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_validate_expected_const_expr(
                section, ir, TURBOWASM_TYPE_I32);
            if (status != TURBOWASM_OK)
                return status;
        }

        status = turbowasm_skip_byte_vector(section);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->data_segment_count = count;
    if (summary->has_data_count && summary->data_count != count)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}
