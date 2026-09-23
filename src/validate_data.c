#include "validate_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_VAL_I32 = 0x7f,
    TURBOWASM_VAL_I64 = 0x7e,
    TURBOWASM_VAL_F32 = 0x7d,
    TURBOWASM_VAL_F64 = 0x7c,
    TURBOWASM_VAL_V128 = 0x7b,
    TURBOWASM_VAL_FUNCREF = 0x70,
    TURBOWASM_VAL_EXTERNREF = 0x6f
};

typedef struct turbowasm_global_type {
    uint8_t value_type;
    bool is_mutable;
} turbowasm_global_type;

static bool turbowasm_const_valtype_supported(uint8_t type) {
    switch (type) {
        case TURBOWASM_VAL_I32:
        case TURBOWASM_VAL_I64:
        case TURBOWASM_VAL_F32:
        case TURBOWASM_VAL_F64:
        case TURBOWASM_VAL_V128:
        case TURBOWASM_VAL_FUNCREF:
        case TURBOWASM_VAL_EXTERNREF:
            return true;
        default:
            return false;
    }
}

static turbowasm_status turbowasm_read_global_type(
    turbowasm_reader *reader,
    turbowasm_global_type *out) {
    uint8_t value_type;
    uint8_t mutability;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &value_type))
        return TURBOWASM_MALFORMED_MODULE;
    if (!turbowasm_const_valtype_supported(value_type))
        return TURBOWASM_UNSUPPORTED;
    if (!turbowasm_reader_u8(reader, &mutability))
        return TURBOWASM_MALFORMED_MODULE;
    if (mutability > 1u)
        return TURBOWASM_MALFORMED_MODULE;

    out->value_type = value_type;
    out->is_mutable = mutability != 0u;
    return TURBOWASM_OK;
}

static bool turbowasm_function_index_valid(
    uint32_t index,
    const turbowasm_validation_context *context) {
    return context != NULL && index < context->function_count;
}

static bool turbowasm_memory_index_valid(
    uint32_t index,
    const turbowasm_validation_context *context) {
    return context != NULL && index < context->memory_count;
}

turbowasm_status turbowasm_validate_const_expr(
    turbowasm_reader *reader,
    turbowasm_validation_context *context,
    uint8_t *out_type) {
    uint8_t opcode;
    uint8_t end;
    turbowasm_status status = TURBOWASM_OK;

    if (reader == NULL || context == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (opcode) {
        case 0x41u: {
            int32_t value;
            if (!turbowasm_reader_sleb32(reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            (void)value;
            *out_type = TURBOWASM_VAL_I32;
            break;
        }
        case 0x42u: {
            int64_t value;
            if (!turbowasm_reader_sleb64(reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            (void)value;
            *out_type = TURBOWASM_VAL_I64;
            break;
        }
        case 0x43u: {
            turbowasm_reader bytes;
            if (!turbowasm_reader_slice(reader, 4u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            *out_type = TURBOWASM_VAL_F32;
            break;
        }
        case 0x44u: {
            turbowasm_reader bytes;
            if (!turbowasm_reader_slice(reader, 8u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            *out_type = TURBOWASM_VAL_F64;
            break;
        }
        case 0xd0u: {
            uint8_t reference_type;
            if (!turbowasm_reader_u8(reader, &reference_type))
                return TURBOWASM_MALFORMED_MODULE;
            if (reference_type != TURBOWASM_VAL_FUNCREF &&
                reference_type != TURBOWASM_VAL_EXTERNREF)
                return TURBOWASM_UNSUPPORTED;
            *out_type = reference_type;
            break;
        }
        case 0xd2u: {
            uint32_t function_index;
            if (!turbowasm_reader_uleb32(reader, &function_index))
                return TURBOWASM_MALFORMED_MODULE;
            if (!turbowasm_function_index_valid(function_index, context))
                return TURBOWASM_MALFORMED_MODULE;
            if (!turbowasm_validation_context_declare_function_ref(
                    context, function_index))
                return TURBOWASM_OUT_OF_MEMORY;
            *out_type = TURBOWASM_VAL_FUNCREF;
            break;
        }
        case 0xfdu: {
            uint32_t subopcode;
            turbowasm_reader bytes;
            if (!turbowasm_reader_uleb32(reader, &subopcode))
                return TURBOWASM_MALFORMED_MODULE;
            if (subopcode != 0x0cu)
                return TURBOWASM_UNSUPPORTED;
            if (!turbowasm_reader_slice(reader, 16u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            *out_type = TURBOWASM_VAL_V128;
            break;
        }
        case 0x23u: {
            uint32_t global_index;
            const turbowasm_validation_global *global;
            if (!turbowasm_reader_uleb32(reader, &global_index))
                return TURBOWASM_MALFORMED_MODULE;
            global = turbowasm_validation_context_global(
                context, global_index);
            if (global == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            if (!global->imported || global->mutable_value)
                return TURBOWASM_MALFORMED_MODULE;
            *out_type = global->value_type;
            break;
        }
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!turbowasm_reader_u8(reader, &end))
        return TURBOWASM_MALFORMED_MODULE;
    if (end != 0x0bu)
        status = TURBOWASM_UNSUPPORTED;

    return status;
}

turbowasm_status turbowasm_validate_global_section(
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
        turbowasm_global_type type;
        uint8_t expression_type;
        const uint8_t *initializer_start;
        size_t initializer_size;
        turbowasm_status status = turbowasm_read_global_type(
            section, &type);

        if (status != TURBOWASM_OK)
            return status;
        initializer_start = section->cursor;
        status = turbowasm_validate_const_expr(
            section, context, &expression_type);
        if (status != TURBOWASM_OK)
            return status;
        initializer_size = (size_t)(section->cursor - initializer_start);
        if (initializer_size > UINT32_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        if (expression_type != type.value_type)
            return TURBOWASM_MALFORMED_MODULE;
        if (!turbowasm_validation_context_append_global(
                context, type.value_type, type.is_mutable, false,
                initializer_start, (uint32_t)initializer_size))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    summary->global_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_byte_vector(
    turbowasm_reader *reader,
    const uint8_t **out_bytes,
    uint32_t *out_size) {
    uint32_t size;
    turbowasm_reader bytes;

    if (out_bytes == NULL || out_size == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &size) ||
        !turbowasm_reader_slice(reader, size, &bytes))
        return TURBOWASM_MALFORMED_MODULE;

    *out_bytes = bytes.cursor;
    *out_size = size;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_read_i32_offset_expr(
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
    if (type != TURBOWASM_VAL_I32)
        return TURBOWASM_MALFORMED_MODULE;

    size = (size_t)(reader->cursor - start);
    if (size > UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    out->bytes = start;
    out->size = (uint32_t)size;
    out->result_type = type;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_data_section(
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
        uint32_t mode;
        turbowasm_validation_data_segment segment = {0};
        turbowasm_status status;

        if (!turbowasm_reader_uleb32(section, &mode))
            return TURBOWASM_MALFORMED_MODULE;

        switch (mode) {
            case 0u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_ACTIVE;
                segment.memory_index = 0u;
                if (!turbowasm_memory_index_valid(0u, context))
                    return TURBOWASM_MALFORMED_MODULE;
                status = turbowasm_read_i32_offset_expr(
                    section, context, &segment.offset);
                if (status != TURBOWASM_OK) return status;
                break;
            case 1u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_PASSIVE;
                break;
            case 2u:
                segment.mode = TURBOWASM_VALIDATION_SEGMENT_ACTIVE;
                if (!turbowasm_reader_uleb32(
                        section, &segment.memory_index))
                    return TURBOWASM_MALFORMED_MODULE;
                if (!turbowasm_memory_index_valid(
                        segment.memory_index, context))
                    return TURBOWASM_MALFORMED_MODULE;
                status = turbowasm_read_i32_offset_expr(
                    section, context, &segment.offset);
                if (status != TURBOWASM_OK) return status;
                break;
            default:
                return TURBOWASM_UNSUPPORTED;
        }

        status = turbowasm_read_byte_vector(
            section, &segment.data, &segment.data_size);
        if (status != TURBOWASM_OK)
            return status;

        if (!turbowasm_validation_context_append_data_segment(
                context, segment))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (turbowasm_reader_remaining(section) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    summary->data_segment_count = count;
    if (summary->has_data_count &&
        summary->data_count != summary->data_segment_count)
        return TURBOWASM_MALFORMED_MODULE;

    return TURBOWASM_OK;
}
