#include "instance_internal.h"

#include "module_internal.h"
#include "reader.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_PAGE_SIZE = 65536u,
    TURBOWASM_VAL_I32 = 0x7f,
    TURBOWASM_VAL_I64 = 0x7e,
    TURBOWASM_VAL_F32 = 0x7d,
    TURBOWASM_VAL_F64 = 0x7c,
    TURBOWASM_VAL_V128 = 0x7b,
    TURBOWASM_VAL_FUNCREF = 0x70,
    TURBOWASM_VAL_EXTERNREF = 0x6f,
    TURBOWASM_NULL_FUNCTION = UINT32_MAX
};

typedef enum turbowasm_runtime_const_kind {
    TURBOWASM_RUNTIME_CONST_VALUE = 0,
    TURBOWASM_RUNTIME_CONST_REF_NULL,
    TURBOWASM_RUNTIME_CONST_REF_FUNC
} turbowasm_runtime_const_kind;

typedef struct turbowasm_runtime_const {
    turbowasm_runtime_const_kind kind;
    uint8_t value_type;
    turbowasm_value value;
    uint32_t function_index;
} turbowasm_runtime_const;

static turbowasm_value_kind turbowasm_runtime_kind(uint8_t type) {
    switch (type) {
        case TURBOWASM_VAL_I32: return TURBOWASM_VALUE_I32;
        case TURBOWASM_VAL_I64: return TURBOWASM_VALUE_I64;
        case TURBOWASM_VAL_F32: return TURBOWASM_VALUE_F32;
        case TURBOWASM_VAL_F64: return TURBOWASM_VALUE_F64;
        case TURBOWASM_VAL_V128: return TURBOWASM_VALUE_V128;
        default: return (turbowasm_value_kind)0;
    }
}

static bool turbowasm_runtime_value_matches(
    const turbowasm_value *value,
    uint8_t type) {
    turbowasm_value_kind kind = turbowasm_runtime_kind(type);
    return value != NULL && kind != 0 && value->kind == kind;
}

static bool turbowasm_runtime_zero_value(
    uint8_t type,
    turbowasm_value *out) {
    turbowasm_value_kind kind = turbowasm_runtime_kind(type);

    if (out == NULL || kind == 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_VALUE_V128)
        out->as.v128.shape = TURBOWASM_V128_RAW;
    return true;
}

static turbowasm_status turbowasm_eval_const_expr(
    const turbowasm_module_impl *module,
    const turbowasm_validation_expr_span *span,
    turbowasm_runtime_const *out) {
    turbowasm_reader reader;
    uint8_t opcode;
    uint8_t end;

    if (module == NULL || span == NULL || out == NULL ||
        span->bytes == NULL || span->size == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    turbowasm_reader_init(&reader, span->bytes, span->size);

    if (!turbowasm_reader_u8(&reader, &opcode))
        return TURBOWASM_MALFORMED_MODULE;

    switch (opcode) {
        case 0x41u: {
            int32_t value;
            if (!turbowasm_reader_sleb32(&reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            out->kind = TURBOWASM_RUNTIME_CONST_VALUE;
            out->value_type = TURBOWASM_VAL_I32;
            out->value.kind = TURBOWASM_VALUE_I32;
            out->value.as.i32 = value;
            break;
        }
        case 0x42u: {
            int64_t value;
            if (!turbowasm_reader_sleb64(&reader, &value))
                return TURBOWASM_MALFORMED_MODULE;
            out->kind = TURBOWASM_RUNTIME_CONST_VALUE;
            out->value_type = TURBOWASM_VAL_I64;
            out->value.kind = TURBOWASM_VALUE_I64;
            out->value.as.i64 = value;
            break;
        }
        case 0x43u: {
            uint32_t bits;
            if (!turbowasm_reader_u32le(&reader, &bits))
                return TURBOWASM_MALFORMED_MODULE;
            out->kind = TURBOWASM_RUNTIME_CONST_VALUE;
            out->value_type = TURBOWASM_VAL_F32;
            out->value.kind = TURBOWASM_VALUE_F32;
            memcpy(&out->value.as.f32, &bits, sizeof(bits));
            break;
        }
        case 0x44u: {
            turbowasm_reader bytes;
            uint64_t bits = 0u;
            uint32_t index;
            if (!turbowasm_reader_slice(&reader, 8u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            for (index = 0u; index < 8u; ++index)
                bits |= (uint64_t)bytes.cursor[index] << (index * 8u);
            out->kind = TURBOWASM_RUNTIME_CONST_VALUE;
            out->value_type = TURBOWASM_VAL_F64;
            out->value.kind = TURBOWASM_VALUE_F64;
            memcpy(&out->value.as.f64, &bits, sizeof(bits));
            break;
        }
        case 0xfdu: {
            uint32_t subopcode;
            turbowasm_reader bytes;
            if (!turbowasm_reader_uleb32(&reader, &subopcode) ||
                subopcode != 0x0cu ||
                !turbowasm_reader_slice(&reader, 16u, &bytes))
                return TURBOWASM_MALFORMED_MODULE;
            out->kind = TURBOWASM_RUNTIME_CONST_VALUE;
            out->value_type = TURBOWASM_VAL_V128;
            out->value.kind = TURBOWASM_VALUE_V128;
            out->value.as.v128.shape = TURBOWASM_V128_RAW;
            memcpy(out->value.as.v128.bits.bytes,
                   bytes.cursor, 16u);
            break;
        }
        case 0xd0u: {
            uint8_t reference_type;
            if (!turbowasm_reader_u8(&reader, &reference_type))
                return TURBOWASM_MALFORMED_MODULE;
            if (reference_type != TURBOWASM_VAL_FUNCREF &&
                reference_type != TURBOWASM_VAL_EXTERNREF)
                return TURBOWASM_UNSUPPORTED;
            out->kind = TURBOWASM_RUNTIME_CONST_REF_NULL;
            out->value_type = reference_type;
            break;
        }
        case 0xd2u: {
            uint32_t function_index;
            if (!turbowasm_reader_uleb32(&reader, &function_index) ||
                function_index >= module->validation.function_count)
                return TURBOWASM_MALFORMED_MODULE;
            out->kind = TURBOWASM_RUNTIME_CONST_REF_FUNC;
            out->value_type = TURBOWASM_VAL_FUNCREF;
            out->function_index = function_index;
            break;
        }
        case 0x23u:
            /* Baseline instance creation has no host-global import binding.
             * Validation permits only imported immutable global.get here. */
            return TURBOWASM_UNSUPPORTED;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (!turbowasm_reader_u8(&reader, &end) ||
        end != 0x0bu ||
        turbowasm_reader_remaining(&reader) != 0u)
        return TURBOWASM_MALFORMED_MODULE;

    return out->value_type == span->result_type
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status turbowasm_eval_i32_offset(
    const turbowasm_module_impl *module,
    const turbowasm_validation_expr_span *span,
    uint32_t *out) {
    turbowasm_runtime_const value;
    turbowasm_status status;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_eval_const_expr(module, span, &value);
    if (status != TURBOWASM_OK)
        return status;
    if (value.kind != TURBOWASM_RUNTIME_CONST_VALUE ||
        value.value.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_MALFORMED_MODULE;

    *out = (uint32_t)value.value.as.i32;
    return TURBOWASM_OK;
}

static bool turbowasm_memory_byte_size(
    uint32_t pages,
    size_t *out) {
    uint64_t bytes = (uint64_t)pages * TURBOWASM_PAGE_SIZE;

    if (out == NULL || bytes > (uint64_t)SIZE_MAX)
        return false;
    *out = (size_t)bytes;
    return true;
}

static turbowasm_status turbowasm_allocate_memory(
    turbowasm_runtime_memory *memory,
    const turbowasm_validation_memory *descriptor) {
    size_t bytes;

    if (memory == NULL || descriptor == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (descriptor->imported)
        return TURBOWASM_UNSUPPORTED;
    if (!turbowasm_memory_byte_size(
            descriptor->limits.minimum, &bytes))
        return TURBOWASM_OUT_OF_MEMORY;

    memory->limits = descriptor->limits;
    memory->pages = descriptor->limits.minimum;
    if (bytes != 0u) {
        memory->bytes = (uint8_t *)calloc(bytes, 1u);
        if (memory->bytes == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_allocate_table(
    turbowasm_runtime_table *table,
    const turbowasm_validation_table *descriptor) {
    size_t bytes;
    uint32_t index;

    if (table == NULL || descriptor == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (descriptor->imported)
        return TURBOWASM_UNSUPPORTED;

    if ((uint64_t)descriptor->limits.minimum *
            sizeof(*table->entries) >
        (uint64_t)SIZE_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    table->reference_type = descriptor->reference_type;
    table->limits = descriptor->limits;
    table->size = descriptor->limits.minimum;

    bytes = (size_t)table->size * sizeof(*table->entries);
    if (bytes != 0u) {
        table->entries = (uint32_t *)malloc(bytes);
        if (table->entries == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        for (index = 0u; index < table->size; ++index)
            table->entries[index] = TURBOWASM_NULL_FUNCTION;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_initialize_globals(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    uint32_t index;

    if (module->summary.imported_global_count != 0u)
        return TURBOWASM_UNSUPPORTED;

    instance->global_count = module->validation.global_count;
    if (instance->global_count != 0u) {
        instance->globals = (turbowasm_value *)calloc(
            (size_t)instance->global_count,
            sizeof(*instance->globals));
        if (instance->globals == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < instance->global_count; ++index) {
        const turbowasm_validation_global *descriptor =
            &module->validation.globals[index];
        turbowasm_runtime_const value;
        turbowasm_status status;

        if (descriptor->imported)
            return TURBOWASM_UNSUPPORTED;
        if (descriptor->value_type == TURBOWASM_VAL_FUNCREF ||
            descriptor->value_type == TURBOWASM_VAL_EXTERNREF)
            return TURBOWASM_UNSUPPORTED;

        if (descriptor->initializer == NULL ||
            descriptor->initializer_size == 0u) {
            if (!turbowasm_runtime_zero_value(
                    descriptor->value_type,
                    &instance->globals[index]))
                return TURBOWASM_UNSUPPORTED;
            continue;
        }

        {
            turbowasm_validation_expr_span span = {
                descriptor->initializer,
                descriptor->initializer_size,
                descriptor->value_type
            };
            status = turbowasm_eval_const_expr(
                module, &span, &value);
        }
        if (status != TURBOWASM_OK)
            return status;
        if (value.kind != TURBOWASM_RUNTIME_CONST_VALUE ||
            !turbowasm_runtime_value_matches(
                &value.value, descriptor->value_type))
            return TURBOWASM_MALFORMED_MODULE;

        instance->globals[index] = value.value;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_initialize_memories(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    uint32_t index;

    if (module->summary.imported_memory_count != 0u)
        return TURBOWASM_UNSUPPORTED;

    instance->memory_count = module->validation.memory_count;
    if (instance->memory_count != 0u) {
        instance->memories = (turbowasm_runtime_memory *)calloc(
            (size_t)instance->memory_count,
            sizeof(*instance->memories));
        if (instance->memories == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < instance->memory_count; ++index) {
        turbowasm_status status = turbowasm_allocate_memory(
            &instance->memories[index],
            &module->validation.memories[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_initialize_tables(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    uint32_t index;

    if (module->summary.imported_table_count != 0u)
        return TURBOWASM_UNSUPPORTED;

    instance->table_count = module->validation.table_count;
    if (instance->table_count != 0u) {
        instance->tables = (turbowasm_runtime_table *)calloc(
            (size_t)instance->table_count,
            sizeof(*instance->tables));
        if (instance->tables == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < instance->table_count; ++index) {
        turbowasm_status status = turbowasm_allocate_table(
            &instance->tables[index],
            &module->validation.tables[index]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_apply_data_segments(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    uint32_t index;

    instance->data_segment_count =
        module->validation.data_segment_count;
    if (instance->data_segment_count != 0u) {
        instance->data_dropped = (uint8_t *)calloc(
            (size_t)instance->data_segment_count, 1u);
        if (instance->data_dropped == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < instance->data_segment_count; ++index) {
        const turbowasm_validation_data_segment *segment =
            &module->validation.data_segments[index];

        if (segment->mode == TURBOWASM_VALIDATION_SEGMENT_ACTIVE) {
            uint32_t offset;
            uint64_t end;
            turbowasm_runtime_memory *memory;
            turbowasm_status status;

            if (segment->memory_index >= instance->memory_count)
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_eval_i32_offset(
                module, &segment->offset, &offset);
            if (status != TURBOWASM_OK)
                return status;

            memory = &instance->memories[segment->memory_index];
            end = (uint64_t)offset + segment->data_size;
            if (end > (uint64_t)memory->pages * TURBOWASM_PAGE_SIZE)
                return TURBOWASM_TRAPPED;

            if (segment->data_size != 0u)
                memcpy(memory->bytes + offset,
                       segment->data,
                       segment->data_size);

            instance->data_dropped[index] = 1u;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_apply_element_segments(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module) {
    uint32_t index;

    instance->element_segment_count =
        module->validation.element_segment_count;
    if (instance->element_segment_count != 0u) {
        instance->element_dropped = (uint8_t *)calloc(
            (size_t)instance->element_segment_count, 1u);
        if (instance->element_dropped == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (index = 0u; index < instance->element_segment_count; ++index) {
        const turbowasm_validation_element_segment *segment =
            &module->validation.element_segments[index];

        if (segment->mode == TURBOWASM_VALIDATION_SEGMENT_ACTIVE) {
            uint32_t offset;
            uint64_t end;
            turbowasm_runtime_table *table;
            uint32_t item_index;
            turbowasm_status status;

            if (segment->table_index >= instance->table_count)
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_eval_i32_offset(
                module, &segment->offset, &offset);
            if (status != TURBOWASM_OK)
                return status;

            table = &instance->tables[segment->table_index];
            end = (uint64_t)offset + segment->item_count;
            if (end > table->size)
                return TURBOWASM_TRAPPED;

            for (item_index = 0u;
                 item_index < segment->item_count;
                 ++item_index) {
                const turbowasm_validation_element_item *item =
                    &segment->items[item_index];
                uint32_t entry = TURBOWASM_NULL_FUNCTION;

                if (item->kind ==
                    TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX) {
                    entry = item->function_index;
                } else {
                    turbowasm_runtime_const value;
                    status = turbowasm_eval_const_expr(
                        module, &item->expression, &value);
                    if (status != TURBOWASM_OK)
                        return status;
                    if (value.kind == TURBOWASM_RUNTIME_CONST_REF_FUNC) {
                        entry = value.function_index;
                    } else if (value.kind !=
                               TURBOWASM_RUNTIME_CONST_REF_NULL) {
                        return TURBOWASM_MALFORMED_MODULE;
                    }
                }

                if (table->reference_type == TURBOWASM_VAL_EXTERNREF &&
                    entry != TURBOWASM_NULL_FUNCTION)
                    return TURBOWASM_MALFORMED_MODULE;

                table->entries[offset + item_index] = entry;
            }

            instance->element_dropped[index] = 1u;
        } else if (segment->mode ==
                   TURBOWASM_VALIDATION_SEGMENT_DECLARATIVE) {
            instance->element_dropped[index] = 1u;
        }
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_state_init(
    turbowasm_instance_impl *instance,
    const turbowasm_module *module) {
    const turbowasm_module_impl *module_impl;
    turbowasm_status status;

    if (instance == NULL || module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module_impl = turbowasm_module_impl_get(module);
    if (module_impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(instance, 0, sizeof(*instance));
    instance->module = module;

    status = turbowasm_initialize_globals(instance, module_impl);
    if (status != TURBOWASM_OK)
        goto fail;
    status = turbowasm_initialize_memories(instance, module_impl);
    if (status != TURBOWASM_OK)
        goto fail;
    status = turbowasm_initialize_tables(instance, module_impl);
    if (status != TURBOWASM_OK)
        goto fail;
    status = turbowasm_apply_data_segments(instance, module_impl);
    if (status != TURBOWASM_OK)
        goto fail;
    status = turbowasm_apply_element_segments(instance, module_impl);
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

    for (index = 0u; index < instance->memory_count; ++index)
        free(instance->memories[index].bytes);
    for (index = 0u; index < instance->table_count; ++index)
        free(instance->tables[index].entries);

    free(instance->globals);
    free(instance->memories);
    free(instance->tables);
    free(instance->data_dropped);
    free(instance->element_dropped);
    memset(instance, 0, sizeof(*instance));
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
    const turbowasm_value *value) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_global *descriptor;

    if (instance == NULL || value == NULL ||
        index >= instance->global_count)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL || index >= module->validation.global_count)
        return TURBOWASM_INVALID_ARGUMENT;

    descriptor = &module->validation.globals[index];
    if (!descriptor->mutable_value)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_value_matches(
            value, descriptor->value_type))
        return TURBOWASM_TYPE_MISMATCH;

    instance->globals[index] = *value;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_memory_range(
    const turbowasm_runtime_memory *memory,
    uint32_t address,
    uint32_t offset,
    size_t size,
    size_t *start,
    turbowasm_trap *trap) {
    uint64_t effective;
    uint64_t end;
    uint64_t memory_size;

    if (memory == NULL || start == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    effective = (uint64_t)address + offset;
    end = effective + size;
    memory_size = (uint64_t)memory->pages * TURBOWASM_PAGE_SIZE;

    if (end < effective || end > memory_size) {
        *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }

    *start = (size_t)effective;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_read(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    void *destination,
    size_t size,
    turbowasm_trap *trap) {
    size_t start;
    turbowasm_status status;

    if (instance == NULL || destination == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_memory_range(
        &instance->memories[memory_index],
        address, offset, size, &start, trap);
    if (status != TURBOWASM_OK)
        return status;

    if (size != 0u)
        memcpy(destination,
               instance->memories[memory_index].bytes + start,
               size);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_write(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    const void *source,
    size_t size,
    turbowasm_trap *trap) {
    size_t start;
    turbowasm_status status;

    if (instance == NULL || source == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_memory_range(
        &instance->memories[memory_index],
        address, offset, size, &start, trap);
    if (status != TURBOWASM_OK)
        return status;

    if (size != 0u)
        memcpy(instance->memories[memory_index].bytes + start,
               source,
               size);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_size(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t *pages) {
    if (instance == NULL || pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;
    *pages = instance->memories[memory_index].pages;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *previous_pages) {
    turbowasm_runtime_memory *memory;
    uint64_t requested;
    size_t old_bytes;
    size_t new_bytes;
    uint8_t *grown;

    if (instance == NULL || previous_pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = &instance->memories[memory_index];
    *previous_pages = memory->pages;

    requested = (uint64_t)memory->pages + delta_pages;
    if (requested > UINT32_MAX ||
        (memory->limits.has_maximum &&
         requested > memory->limits.maximum) ||
        !turbowasm_memory_byte_size(
            (uint32_t)requested, &new_bytes) ||
        !turbowasm_memory_byte_size(
            memory->pages, &old_bytes)) {
        *previous_pages = UINT32_MAX;
        return TURBOWASM_OK;
    }

    if (delta_pages == 0u)
        return TURBOWASM_OK;

    grown = (uint8_t *)realloc(memory->bytes, new_bytes);
    if (grown == NULL) {
        *previous_pages = UINT32_MAX;
        return TURBOWASM_OK;
    }

    memset(grown + old_bytes, 0, new_bytes - old_bytes);
    memory->bytes = grown;
    memory->pages = (uint32_t)requested;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_function(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    uint32_t *function_index,
    turbowasm_trap *trap) {
    const turbowasm_runtime_table *table;
    uint32_t entry;

    if (instance == NULL || function_index == NULL || trap == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = &instance->tables[table_index];
    if (table->reference_type != TURBOWASM_VAL_FUNCREF)
        return TURBOWASM_TYPE_MISMATCH;

    if (element_index >= table->size) {
        *trap = TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS;
        return TURBOWASM_TRAPPED;
    }

    entry = table->entries[element_index];
    if (entry == TURBOWASM_NULL_FUNCTION) {
        *trap = TURBOWASM_TRAP_INDIRECT_CALL_NULL;
        return TURBOWASM_TRAPPED;
    }

    *function_index = entry;
    return TURBOWASM_OK;
}
