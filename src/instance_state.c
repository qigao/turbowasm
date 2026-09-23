#include "instance_internal.h"

#include "reader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static turbowasm_value_kind turbowasm_state_kind_from_valtype(
    uint8_t type) {
    switch (type) {
        case 0x7fu: return TURBOWASM_VALUE_I32;
        case 0x7eu: return TURBOWASM_VALUE_I64;
        case 0x7du: return TURBOWASM_VALUE_F32;
        case 0x7cu: return TURBOWASM_VALUE_F64;
        case 0x7bu: return TURBOWASM_VALUE_V128;
        case 0x70u: return TURBOWASM_VALUE_FUNCREF;
        default: return (turbowasm_value_kind)0;
    }
}

static turbowasm_status turbowasm_eval_value_expr(
    const turbowasm_validation_expr_span *expression,
    turbowasm_value *out) {
    turbowasm_reader reader;
    uint8_t opcode;
    uint8_t end;

    if (expression == NULL || out == NULL ||
        expression->bytes == NULL || expression->size == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    turbowasm_reader_init(
        &reader, expression->bytes, expression->size);

    if (!turbowasm_reader_u8(&reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (opcode) {
        case 0x41u:
            out->kind = TURBOWASM_VALUE_I32;
            if (!turbowasm_reader_sleb32(
                    &reader, &out->as.i32))
                return TURBOWASM_MALFORMED_MODULE;
            break;
        case 0x42u:
            out->kind = TURBOWASM_VALUE_I64;
            if (!turbowasm_reader_sleb64(
                    &reader, &out->as.i64))
                return TURBOWASM_MALFORMED_MODULE;
            break;
        case 0x43u: {
            uint32_t bits;
            out->kind = TURBOWASM_VALUE_F32;
            if (!turbowasm_reader_u32le(&reader, &bits))
                return TURBOWASM_MALFORMED_MODULE;
            memcpy(&out->as.f32, &bits, sizeof(bits));
            break;
        }
        case 0x44u: {
            turbowasm_reader bytes;
            uint64_t bits = 0u;
            uint32_t index;
            out->kind = TURBOWASM_VALUE_F64;
            if (!turbowasm_reader_slice(
                    &reader, 8u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            for (index = 0u; index < 8u; ++index)
                bits |= (uint64_t)bytes.cursor[index] <<
                        (8u * index);
            memcpy(&out->as.f64, &bits, sizeof(bits));
            break;
        }
        case 0xfdu: {
            uint32_t subopcode;
            turbowasm_reader bytes;
            out->kind = TURBOWASM_VALUE_V128;
            out->as.v128.shape = TURBOWASM_V128_RAW;
            if (!turbowasm_reader_uleb32(
                    &reader, &subopcode) ||
                subopcode != 0x0cu ||
                !turbowasm_reader_slice(
                    &reader, 16u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            salts_simd_v128_load(
                &out->as.v128.bits, bytes.cursor);
            break;
        }
        case 0x23u:
            /* Imported immutable global.get needs host binding. */
            return TURBOWASM_UNSUPPORTED;
        case 0xd0u: {
            uint8_t reference_type;
            if (!turbowasm_reader_u8(&reader, &reference_type))
                return TURBOWASM_MALFORMED_MODULE;
            if (reference_type != 0x70u)
                return TURBOWASM_UNSUPPORTED;
            out->kind = TURBOWASM_VALUE_FUNCREF;
            out->as.funcref.is_null = true;
            out->as.funcref.function_index = UINT32_MAX;
            break;
        }
        case 0xd2u:
            out->kind = TURBOWASM_VALUE_FUNCREF;
            out->as.funcref.is_null = false;
            if (!turbowasm_reader_uleb32(
                    &reader, &out->as.funcref.function_index))
                return TURBOWASM_MALFORMED_MODULE;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!turbowasm_reader_u8(&reader, &end) ||
        end != 0x0bu ||
        turbowasm_reader_remaining(&reader) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_eval_i32_expr(
    const turbowasm_validation_expr_span *expression,
    uint32_t *out) {
    turbowasm_value value;
    turbowasm_status status = turbowasm_eval_value_expr(
        expression, &value);

    if (status != TURBOWASM_OK)
        return status;
    if (value.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_MALFORMED_MODULE;
    *out = (uint32_t)value.as.i32;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_eval_funcref_expr(
    const turbowasm_validation_expr_span *expression,
    turbowasm_instance_table_entry *out) {
    turbowasm_reader reader;
    uint8_t opcode;
    uint8_t end;

    if (expression == NULL || out == NULL ||
        expression->bytes == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_reader_init(
        &reader, expression->bytes, expression->size);
    if (!turbowasm_reader_u8(&reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    if (opcode == 0xd0u) {
        uint8_t reference_type;
        if (!turbowasm_reader_u8(
                &reader, &reference_type) ||
            reference_type != 0x70u)
            return TURBOWASM_MALFORMED_MODULE;
        out->is_null = true;
        out->function_index = UINT32_MAX;
    } else if (opcode == 0xd2u) {
        if (!turbowasm_reader_uleb32(
                &reader, &out->function_index))
            return TURBOWASM_MALFORMED_MODULE;
        out->is_null = false;
    } else if (opcode == 0x23u) {
        return TURBOWASM_UNSUPPORTED;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    if (!turbowasm_reader_u8(&reader, &end) ||
        end != 0x0bu ||
        turbowasm_reader_remaining(&reader) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_allocate_globals(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t index;

    if (context->global_count == 0u)
        return TURBOWASM_OK;

    if ((uint64_t)context->global_count *
            sizeof(*instance->globals) >
        (uint64_t)SIZE_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    instance->globals = (turbowasm_value *)calloc(
        (size_t)context->global_count,
        sizeof(*instance->globals));
    if (instance->globals == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    instance->global_count = context->global_count;

    for (index = 0u; index < context->global_count; ++index) {
        const turbowasm_validation_global *global =
            &context->globals[index];
        turbowasm_validation_expr_span expression;
        turbowasm_status status;

        if (global->imported)
            return TURBOWASM_UNSUPPORTED;

        expression.bytes = global->initializer;
        expression.size = global->initializer_size;
        expression.result_type = global->value_type;
        status = turbowasm_eval_value_expr(
            &expression, &instance->globals[index]);
        if (status != TURBOWASM_OK)
            return status;
        if (instance->globals[index].kind !=
            turbowasm_state_kind_from_valtype(
                global->value_type))
            return TURBOWASM_MALFORMED_MODULE;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_allocate_memories(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t index;

    if (context->memory_count == 0u)
        return TURBOWASM_OK;

    instance->memories = (turbowasm_instance_memory *)calloc(
        (size_t)context->memory_count,
        sizeof(*instance->memories));
    if (instance->memories == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    instance->memory_count = context->memory_count;

    for (index = 0u; index < context->memory_count; ++index) {
        const turbowasm_validation_memory *source =
            &context->memories[index];
        turbowasm_instance_memory *memory =
            &instance->memories[index];
        uint64_t bytes =
            (uint64_t)source->limits.minimum *
            TURBOWASM_WASM_PAGE_SIZE;

        if (source->imported)
            return TURBOWASM_UNSUPPORTED;
        if (bytes > (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;

        memory->pages = source->limits.minimum;
        memory->maximum_pages = source->limits.maximum;
        memory->has_maximum = source->limits.has_maximum;

        if (bytes != 0u) {
            memory->data = (uint8_t *)calloc(
                (size_t)bytes, 1u);
            if (memory->data == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_allocate_tables(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t index;

    if (context->table_count == 0u)
        return TURBOWASM_OK;

    instance->tables = (turbowasm_instance_table *)calloc(
        (size_t)context->table_count,
        sizeof(*instance->tables));
    if (instance->tables == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    instance->table_count = context->table_count;

    for (index = 0u; index < context->table_count; ++index) {
        const turbowasm_validation_table *source =
            &context->tables[index];
        turbowasm_instance_table *table =
            &instance->tables[index];
        uint32_t item;

        if (source->imported)
            return TURBOWASM_UNSUPPORTED;
        if (source->reference_type != 0x70u)
            return TURBOWASM_UNSUPPORTED;

        table->size = source->limits.minimum;
        table->maximum = source->limits.maximum;
        table->has_maximum = source->limits.has_maximum;
        table->reference_type = source->reference_type;

        if (table->size != 0u) {
            if ((uint64_t)table->size *
                    sizeof(*table->entries) >
                (uint64_t)SIZE_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            table->entries =
                (turbowasm_instance_table_entry *)calloc(
                    (size_t)table->size,
                    sizeof(*table->entries));
            if (table->entries == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }

        for (item = 0u; item < table->size; ++item) {
            table->entries[item].is_null = true;
            table->entries[item].function_index = UINT32_MAX;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_allocate_segment_lifecycle(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t index;

    instance->data_segment_count = context->data_segment_count;
    if (context->data_segment_count != 0u) {
        instance->data_segment_dropped = (uint8_t *)calloc(
            (size_t)context->data_segment_count, 1u);
        if (instance->data_segment_dropped == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        for (index = 0u; index < context->data_segment_count; ++index) {
            if (context->data_segments[index].mode !=
                TURBOWASM_VALIDATION_SEGMENT_PASSIVE)
                instance->data_segment_dropped[index] = 1u;
        }
    }

    instance->element_segment_count = context->element_segment_count;
    if (context->element_segment_count != 0u) {
        instance->element_segment_dropped = (uint8_t *)calloc(
            (size_t)context->element_segment_count, 1u);
        if (instance->element_segment_dropped == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        for (index = 0u; index < context->element_segment_count; ++index) {
            if (context->element_segments[index].mode !=
                TURBOWASM_VALIDATION_SEGMENT_PASSIVE)
                instance->element_segment_dropped[index] = 1u;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_apply_data_segments(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t index;

    for (index = 0u; index < context->data_segment_count; ++index) {
        const turbowasm_validation_data_segment *segment =
            &context->data_segments[index];
        uint32_t offset;
        uint8_t *destination;
        turbowasm_status status;

        if (segment->mode !=
            TURBOWASM_VALIDATION_SEGMENT_ACTIVE)
            continue;

        status = turbowasm_eval_i32_expr(
            &segment->offset, &offset);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_instance_memory_bounds(
            instance,
            segment->memory_index,
            offset,
            0u,
            segment->data_size,
            &destination);
        if (status != TURBOWASM_OK)
            return TURBOWASM_TRAPPED;
        if (segment->data_size != 0u)
            memcpy(destination,
                   segment->data,
                   segment->data_size);
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_item_value(
    const turbowasm_validation_element_item *item,
    turbowasm_instance_table_entry *out) {
    if (item == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (item->kind ==
        TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX) {
        out->is_null = false;
        out->function_index = item->function_index;
        return TURBOWASM_OK;
    }

    return turbowasm_eval_funcref_expr(
        &item->expression, out);
}

static turbowasm_status turbowasm_apply_element_segments(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_context *context) {
    uint32_t segment_index;

    for (segment_index = 0u;
         segment_index < context->element_segment_count;
         ++segment_index) {
        const turbowasm_validation_element_segment *segment =
            &context->element_segments[segment_index];
        turbowasm_instance_table *table;
        uint32_t offset;
        uint32_t item_index;
        uint64_t end;
        turbowasm_status status;

        if (segment->mode !=
            TURBOWASM_VALIDATION_SEGMENT_ACTIVE)
            continue;
        if (segment->table_index >= instance->table_count)
            return TURBOWASM_TRAPPED;

        table = &instance->tables[segment->table_index];
        if (table->reference_type != segment->reference_type)
            return TURBOWASM_MALFORMED_MODULE;

        status = turbowasm_eval_i32_expr(
            &segment->offset, &offset);
        if (status != TURBOWASM_OK)
            return status;

        end = (uint64_t)offset + segment->item_count;
        if (end > table->size)
            return TURBOWASM_TRAPPED;

        for (item_index = 0u;
             item_index < segment->item_count;
             ++item_index) {
            turbowasm_instance_table_entry value;

            status = turbowasm_element_item_value(
                &segment->items[item_index], &value);
            if (status != TURBOWASM_OK)
                return status;
            table->entries[offset + item_index] = value;
        }
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_state_init(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    turbowasm_status status;

    if (instance == NULL || module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_allocate_globals(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_allocate_memories(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_allocate_tables(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_allocate_segment_lifecycle(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_apply_data_segments(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_apply_element_segments(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        goto fail;

    return TURBOWASM_OK;

fail:
    turbowasm_instance_state_destroy(instance);
    return status;
}

void turbowasm_instance_state_destroy(
    turbowasm_instance_impl *instance) {
    uint32_t index;

    if (instance == NULL)
        return;

    free(instance->globals);
    instance->globals = NULL;
    instance->global_count = 0u;

    for (index = 0u; index < instance->memory_count; ++index)
        free(instance->memories[index].data);
    free(instance->memories);
    instance->memories = NULL;
    instance->memory_count = 0u;

    for (index = 0u; index < instance->table_count; ++index)
        free(instance->tables[index].entries);
    free(instance->tables);
    instance->tables = NULL;
    instance->table_count = 0u;

    free(instance->data_segment_dropped);
    instance->data_segment_dropped = NULL;
    instance->data_segment_count = 0u;

    free(instance->element_segment_dropped);
    instance->element_segment_dropped = NULL;
    instance->element_segment_count = 0u;
}

turbowasm_status turbowasm_instance_global_get(
    const turbowasm_instance_impl *instance,
    uint32_t index,
    turbowasm_value *out) {
    if (instance == NULL || out == NULL ||
        index >= instance->global_count)
        return TURBOWASM_INVALID_ARGUMENT;
    *out = instance->globals[index];
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_global_set(
    turbowasm_instance_impl *instance,
    uint32_t index,
    turbowasm_value value) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_global *global;

    if (instance == NULL || index >= instance->global_count)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL || index >= module->validation.global_count)
        return TURBOWASM_INVALID_ARGUMENT;
    global = &module->validation.globals[index];

    if (!global->mutable_value)
        return TURBOWASM_TYPE_MISMATCH;
    if (value.kind !=
        turbowasm_state_kind_from_valtype(global->value_type))
        return TURBOWASM_TYPE_MISMATCH;

    instance->globals[index] = value;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_bounds(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    size_t width,
    uint8_t **out) {
    const turbowasm_instance_memory *memory;
    uint64_t effective;
    uint64_t size;

    if (instance == NULL || out == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = &instance->memories[memory_index];
    effective = (uint64_t)address + offset;
    size = (uint64_t)memory->pages *
           TURBOWASM_WASM_PAGE_SIZE;

    if (effective > size ||
        (uint64_t)width > size - effective)
        return TURBOWASM_TRAPPED;

    *out = memory->data == NULL
        ? NULL
        : memory->data + (size_t)effective;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_size(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t *out_pages) {
    if (instance == NULL || out_pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_pages = instance->memories[memory_index].pages;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *out_previous_pages) {
    turbowasm_instance_memory *memory;
    uint64_t next_pages;
    uint64_t next_bytes;
    uint64_t previous_bytes;
    uint8_t *grown;

    if (instance == NULL || out_previous_pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = &instance->memories[memory_index];
    *out_previous_pages = memory->pages;

    next_pages = (uint64_t)memory->pages + delta_pages;
    if (next_pages > UINT32_C(65536) ||
        (memory->has_maximum &&
         next_pages > memory->maximum_pages)) {
        *out_previous_pages = UINT32_MAX;
        return TURBOWASM_OK;
    }

    next_bytes = next_pages * TURBOWASM_WASM_PAGE_SIZE;
    previous_bytes =
        (uint64_t)memory->pages * TURBOWASM_WASM_PAGE_SIZE;
    if (next_bytes > (uint64_t)SIZE_MAX) {
        *out_previous_pages = UINT32_MAX;
        return TURBOWASM_OK;
    }

    if (next_bytes == 0u) {
        memory->pages = (uint32_t)next_pages;
        return TURBOWASM_OK;
    }

    grown = (uint8_t *)realloc(
        memory->data, (size_t)next_bytes);
    if (grown == NULL) {
        *out_previous_pages = UINT32_MAX;
        return TURBOWASM_OK;
    }

    if (next_bytes > previous_bytes)
        memset(grown + (size_t)previous_bytes,
               0,
               (size_t)(next_bytes - previous_bytes));

    memory->data = grown;
    memory->pages = (uint32_t)next_pages;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_lookup(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_instance_table_entry *out) {
    if (instance == NULL || out == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (element_index >= instance->tables[table_index].size)
        return TURBOWASM_TRAPPED;
    *out = instance->tables[table_index].entries[element_index];
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_get_value(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value *out) {
    turbowasm_instance_table_entry entry;
    turbowasm_status status;

    if (instance == NULL || out == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (instance->tables[table_index].reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;

    status = turbowasm_instance_table_lookup(
        instance, table_index, element_index, &entry);
    if (status != TURBOWASM_OK)
        return status;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_VALUE_FUNCREF;
    out->as.funcref.is_null = entry.is_null;
    out->as.funcref.function_index = entry.function_index;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_set_value(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value value) {
    turbowasm_instance_table *table;
    const turbowasm_module_impl *module;

    if (instance == NULL || table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = &instance->tables[table_index];
    if (table->reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;
    if (value.kind != TURBOWASM_VALUE_FUNCREF)
        return TURBOWASM_TYPE_MISMATCH;
    if (element_index >= table->size)
        return TURBOWASM_TRAPPED;

    if (!value.as.funcref.is_null) {
        module = turbowasm_module_impl_get(instance->module);
        if (module == NULL ||
            value.as.funcref.function_index >=
                module->validation.function_count)
            return TURBOWASM_INVALID_ARGUMENT;
    }

    table->entries[element_index].is_null =
        value.as.funcref.is_null;
    table->entries[element_index].function_index =
        value.as.funcref.is_null
            ? UINT32_MAX
            : value.as.funcref.function_index;
    return TURBOWASM_OK;
}

static bool turbowasm_range_fits(
    uint32_t offset,
    uint32_t length,
    uint64_t size) {
    return (uint64_t)offset <= size &&
           (uint64_t)length <= size - (uint64_t)offset;
}

turbowasm_status turbowasm_instance_memory_init(
    turbowasm_instance_impl *instance,
    uint32_t data_index,
    uint32_t memory_index,
    uint32_t destination,
    uint32_t source,
    uint32_t length) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_data_segment *segment;
    uint64_t source_size;
    uint8_t *destination_bytes;
    turbowasm_status status;

    if (instance == NULL ||
        data_index >= instance->data_segment_count)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        data_index >= module->validation.data_segment_count)
        return TURBOWASM_INVALID_ARGUMENT;

    segment = &module->validation.data_segments[data_index];
    source_size = instance->data_segment_dropped[data_index]
        ? 0u
        : segment->data_size;

    if (!turbowasm_range_fits(source, length, source_size))
        return TURBOWASM_TRAPPED;

    status = turbowasm_instance_memory_bounds(
        instance, memory_index,
        destination, 0u, length,
        &destination_bytes);
    if (status != TURBOWASM_OK)
        return status;

    if (length != 0u)
        memcpy(destination_bytes, segment->data + source, length);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_data_drop(
    turbowasm_instance_impl *instance,
    uint32_t data_index) {
    if (instance == NULL ||
        data_index >= instance->data_segment_count)
        return TURBOWASM_INVALID_ARGUMENT;
    instance->data_segment_dropped[data_index] = 1u;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_copy(
    turbowasm_instance_impl *instance,
    uint32_t destination_memory,
    uint32_t source_memory,
    uint32_t destination,
    uint32_t source,
    uint32_t length) {
    uint8_t *destination_bytes;
    uint8_t *source_bytes;
    turbowasm_status status;

    status = turbowasm_instance_memory_bounds(
        instance, destination_memory,
        destination, 0u, length,
        &destination_bytes);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_instance_memory_bounds(
        instance, source_memory,
        source, 0u, length,
        &source_bytes);
    if (status != TURBOWASM_OK)
        return status;

    if (length != 0u)
        memmove(destination_bytes, source_bytes, length);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_fill(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t destination,
    uint8_t value,
    uint32_t length) {
    uint8_t *destination_bytes;
    turbowasm_status status =
        turbowasm_instance_memory_bounds(
            instance, memory_index,
            destination, 0u, length,
            &destination_bytes);

    if (status != TURBOWASM_OK)
        return status;
    if (length != 0u)
        memset(destination_bytes, value, length);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_init(
    turbowasm_instance_impl *instance,
    uint32_t element_index,
    uint32_t table_index,
    uint32_t destination,
    uint32_t source,
    uint32_t length) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_element_segment *segment;
    turbowasm_instance_table *table;
    turbowasm_instance_table_entry *values = NULL;
    uint64_t source_size;
    uint32_t index;
    turbowasm_status status = TURBOWASM_OK;

    if (instance == NULL ||
        element_index >= instance->element_segment_count ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        element_index >= module->validation.element_segment_count)
        return TURBOWASM_INVALID_ARGUMENT;

    segment = &module->validation.element_segments[element_index];
    table = &instance->tables[table_index];
    if (segment->reference_type != table->reference_type)
        return TURBOWASM_TYPE_MISMATCH;
    if (table->reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;

    source_size = instance->element_segment_dropped[element_index]
        ? 0u
        : segment->item_count;

    if (!turbowasm_range_fits(source, length, source_size) ||
        !turbowasm_range_fits(destination, length, table->size))
        return TURBOWASM_TRAPPED;

    if (length != 0u) {
        if ((uint64_t)length * sizeof(*values) >
            (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;
        values = (turbowasm_instance_table_entry *)calloc(
            (size_t)length, sizeof(*values));
        if (values == NULL)
            return TURBOWASM_OUT_OF_MEMORY;

        for (index = 0u; index < length; ++index) {
            status = turbowasm_element_item_value(
                &segment->items[source + index],
                &values[index]);
            if (status != TURBOWASM_OK)
                goto done;
        }

        memcpy(&table->entries[destination],
               values,
               (size_t)length * sizeof(*values));
    }

done:
    free(values);
    return status;
}

turbowasm_status turbowasm_instance_element_drop(
    turbowasm_instance_impl *instance,
    uint32_t element_index) {
    if (instance == NULL ||
        element_index >= instance->element_segment_count)
        return TURBOWASM_INVALID_ARGUMENT;
    instance->element_segment_dropped[element_index] = 1u;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_copy(
    turbowasm_instance_impl *instance,
    uint32_t destination_table,
    uint32_t source_table,
    uint32_t destination,
    uint32_t source,
    uint32_t length) {
    turbowasm_instance_table *destination_object;
    turbowasm_instance_table *source_object;

    if (instance == NULL ||
        destination_table >= instance->table_count ||
        source_table >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    destination_object = &instance->tables[destination_table];
    source_object = &instance->tables[source_table];
    if (destination_object->reference_type !=
        source_object->reference_type)
        return TURBOWASM_TYPE_MISMATCH;

    if (!turbowasm_range_fits(
            destination, length, destination_object->size) ||
        !turbowasm_range_fits(
            source, length, source_object->size))
        return TURBOWASM_TRAPPED;

    if (length != 0u)
        memmove(&destination_object->entries[destination],
                &source_object->entries[source],
                (size_t)length *
                    sizeof(*destination_object->entries));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_grow(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    turbowasm_value initial,
    uint32_t delta,
    uint32_t *out_previous_size) {
    turbowasm_instance_table *table;
    turbowasm_instance_table_entry entry;
    const turbowasm_module_impl *module;
    uint64_t next_size;
    turbowasm_instance_table_entry *grown;
    uint32_t index;

    if (instance == NULL || out_previous_size == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = &instance->tables[table_index];
    *out_previous_size = table->size;

    if (table->reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;
    if (initial.kind != TURBOWASM_VALUE_FUNCREF)
        return TURBOWASM_TYPE_MISMATCH;

    entry.is_null = initial.as.funcref.is_null;
    entry.function_index = initial.as.funcref.is_null
        ? UINT32_MAX
        : initial.as.funcref.function_index;

    if (!entry.is_null) {
        module = turbowasm_module_impl_get(instance->module);
        if (module == NULL ||
            entry.function_index >= module->validation.function_count)
            return TURBOWASM_INVALID_ARGUMENT;
    }

    next_size = (uint64_t)table->size + delta;
    if (next_size > UINT32_MAX ||
        (table->has_maximum && next_size > table->maximum) ||
        next_size * sizeof(*grown) > (uint64_t)SIZE_MAX) {
        *out_previous_size = UINT32_MAX;
        return TURBOWASM_OK;
    }

    if (delta == 0u)
        return TURBOWASM_OK;

    grown = (turbowasm_instance_table_entry *)realloc(
        table->entries, (size_t)next_size * sizeof(*grown));
    if (grown == NULL) {
        *out_previous_size = UINT32_MAX;
        return TURBOWASM_OK;
    }

    table->entries = grown;
    for (index = table->size; index < (uint32_t)next_size; ++index)
        table->entries[index] = entry;
    table->size = (uint32_t)next_size;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_size(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t *out_size) {
    if (instance == NULL || out_size == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_size = instance->tables[table_index].size;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_fill(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t destination,
    turbowasm_value value,
    uint32_t length) {
    turbowasm_instance_table *table;
    turbowasm_instance_table_entry entry;
    const turbowasm_module_impl *module;
    uint32_t index;

    if (instance == NULL || table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = &instance->tables[table_index];
    if (table->reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;
    if (value.kind != TURBOWASM_VALUE_FUNCREF)
        return TURBOWASM_TYPE_MISMATCH;
    if (!turbowasm_range_fits(destination, length, table->size))
        return TURBOWASM_TRAPPED;

    entry.is_null = value.as.funcref.is_null;
    entry.function_index = value.as.funcref.is_null
        ? UINT32_MAX
        : value.as.funcref.function_index;

    if (!entry.is_null) {
        module = turbowasm_module_impl_get(instance->module);
        if (module == NULL ||
            entry.function_index >= module->validation.function_count)
            return TURBOWASM_INVALID_ARGUMENT;
    }

    for (index = 0u; index < length; ++index)
        table->entries[destination + index] = entry;
    return TURBOWASM_OK;
}
