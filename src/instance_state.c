#include "instance_internal.h"

#include "reader.h"
#include "validate_type.h"

#include <salts/clock.h>

#include <errno.h>
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
        case 0x6fu: return TURBOWASM_VALUE_EXTERNREF;
        default: return (turbowasm_value_kind)0;
    }
}

static salts_once_t turbowasm_sc_once = SALTS_ONCE_INIT;
static salts_mutex_t turbowasm_sc_mutex = NULL;

static void turbowasm_sc_mutex_init_once(void) {
    salts_mutex_init(&turbowasm_sc_mutex);
}

static bool turbowasm_sc_lock(void) {
    salts_once(&turbowasm_sc_once, turbowasm_sc_mutex_init_once);
    if (turbowasm_sc_mutex == NULL)
        return false;
    salts_mutex_lock(&turbowasm_sc_mutex);
    return true;
}

static void turbowasm_sc_unlock(void) {
    if (turbowasm_sc_mutex != NULL)
        salts_mutex_unlock(&turbowasm_sc_mutex);
}

turbowasm_status turbowasm_threads_sc_fence(void) {
    if (!turbowasm_sc_lock())
        return TURBOWASM_OUT_OF_MEMORY;
    turbowasm_sc_unlock();
    return TURBOWASM_OK;
}

typedef struct turbowasm_const_value_stack {
    turbowasm_value *values;
    uint32_t size;
    uint32_t capacity;
} turbowasm_const_value_stack;

static bool turbowasm_const_value_stack_reserve(
    turbowasm_const_value_stack *stack,
    uint32_t required) {
    uint32_t next;
    turbowasm_value *grown;

    if (stack == NULL)
        return false;
    if (required <= stack->capacity)
        return true;

    next = stack->capacity == 0u ? 4u : stack->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * sizeof(*grown) > (uint64_t)SIZE_MAX)
        return false;
    grown = (turbowasm_value *)realloc(
        stack->values, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    stack->values = grown;
    stack->capacity = next;
    return true;
}

static turbowasm_status turbowasm_const_value_stack_push(
    turbowasm_const_value_stack *stack,
    turbowasm_value value) {
    if (stack == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_const_value_stack_reserve(
            stack, stack->size + 1u))
        return TURBOWASM_OUT_OF_MEMORY;

    stack->values[stack->size++] = value;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_const_value_stack_pop(
    turbowasm_const_value_stack *stack,
    turbowasm_value_kind kind,
    turbowasm_value *out) {
    if (stack == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (stack->size == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    --stack->size;
    *out = stack->values[stack->size];
    return out->kind == kind
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status turbowasm_eval_value_expr(
    const turbowasm_instance_impl *instance,
    const turbowasm_validation_expr_span *expression,
    turbowasm_value *out) {
    turbowasm_reader reader;
    turbowasm_const_value_stack stack = {0};
    turbowasm_status status = TURBOWASM_OK;

    if (expression == NULL || out == NULL ||
        expression->bytes == NULL || expression->size == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_reader_init(
        &reader, expression->bytes, expression->size);

    for (;;) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&reader, &opcode)) {
            status = TURBOWASM_MALFORMED_MODULE;
            break;
        }
        if (opcode == 0x0bu)
            break;

        switch (opcode) {
            case 0x41u: {
                turbowasm_value value = {0};
                value.kind = TURBOWASM_VALUE_I32;
                if (!turbowasm_reader_sleb32(
                        &reader, &value.as.i32)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x42u: {
                turbowasm_value value = {0};
                value.kind = TURBOWASM_VALUE_I64;
                if (!turbowasm_reader_sleb64(
                        &reader, &value.as.i64)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x43u: {
                turbowasm_value value = {0};
                uint32_t bits;

                value.kind = TURBOWASM_VALUE_F32;
                if (!turbowasm_reader_u32le(
                        &reader, &bits)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                memcpy(&value.as.f32, &bits, sizeof(bits));
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x44u: {
                turbowasm_value value = {0};
                turbowasm_reader bytes;
                uint64_t bits = 0u;
                uint32_t index;

                value.kind = TURBOWASM_VALUE_F64;
                if (!turbowasm_reader_slice(
                        &reader, 8u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                for (index = 0u; index < 8u; ++index)
                    bits |= (uint64_t)bytes.cursor[index] <<
                            (8u * index);
                memcpy(&value.as.f64, &bits, sizeof(bits));
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x6au: /* i32.add */
            case 0x6bu: /* i32.sub */
            case 0x6cu: { /* i32.mul */
                turbowasm_value right;
                turbowasm_value left;
                turbowasm_value value = {0};
                uint32_t a;
                uint32_t b;
                uint32_t bits;

                status = turbowasm_const_value_stack_pop(
                    &stack, TURBOWASM_VALUE_I32, &right);
                if (status != TURBOWASM_OK)
                    break;
                status = turbowasm_const_value_stack_pop(
                    &stack, TURBOWASM_VALUE_I32, &left);
                if (status != TURBOWASM_OK)
                    break;

                a = (uint32_t)left.as.i32;
                b = (uint32_t)right.as.i32;
                bits = opcode == 0x6au ? a + b :
                       opcode == 0x6bu ? a - b :
                                         a * b;
                value.kind = TURBOWASM_VALUE_I32;
                memcpy(&value.as.i32, &bits, sizeof(bits));
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x7cu: /* i64.add */
            case 0x7du: /* i64.sub */
            case 0x7eu: { /* i64.mul */
                turbowasm_value right;
                turbowasm_value left;
                turbowasm_value value = {0};
                uint64_t a;
                uint64_t b;
                uint64_t bits;

                status = turbowasm_const_value_stack_pop(
                    &stack, TURBOWASM_VALUE_I64, &right);
                if (status != TURBOWASM_OK)
                    break;
                status = turbowasm_const_value_stack_pop(
                    &stack, TURBOWASM_VALUE_I64, &left);
                if (status != TURBOWASM_OK)
                    break;

                a = (uint64_t)left.as.i64;
                b = (uint64_t)right.as.i64;
                bits = opcode == 0x7cu ? a + b :
                       opcode == 0x7du ? a - b :
                                         a * b;
                value.kind = TURBOWASM_VALUE_I64;
                memcpy(&value.as.i64, &bits, sizeof(bits));
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0xfdu: {
                uint32_t subopcode;
                turbowasm_reader bytes;
                turbowasm_value value = {0};

                value.kind = TURBOWASM_VALUE_V128;
                value.as.v128.shape = TURBOWASM_V128_RAW;
                if (!turbowasm_reader_uleb32(
                        &reader, &subopcode) ||
                    subopcode != 0x0cu ||
                    !turbowasm_reader_slice(
                        &reader, 16u, &bytes)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                salts_simd_v128_load(
                    &value.as.v128.bits, bytes.cursor);
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0x23u: {
                uint32_t global_index;
                turbowasm_value value;

                if (instance == NULL ||
                    !turbowasm_reader_uleb32(
                        &reader, &global_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }

                status = turbowasm_instance_global_get(
                    instance, global_index, &value);
                if (status == TURBOWASM_OK)
                    status = turbowasm_const_value_stack_push(
                        &stack, value);
                break;
            }
            case 0xd0u: {
                turbowasm_validation_value_type reference_type;
                turbowasm_value value = {0};

                status = turbowasm_validation_read_heaptype(
                    &reader, &reference_type);
                if (status != TURBOWASM_OK)
                    break;
                if (reference_type.heap_kind ==
                        TURBOWASM_VALIDATION_HEAP_TYPE_INDEX &&
                    instance != NULL) {
                    const turbowasm_module_impl *module =
                        turbowasm_module_impl_get(instance->module);
                    if (module == NULL ||
                        reference_type.type_index >=
                            module->validation.type_count) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        break;
                    }
                }

                if (reference_type.carrier == 0x70u) {
                    value.kind = TURBOWASM_VALUE_FUNCREF;
                    value.as.funcref.is_null = true;
                    value.as.funcref.function_index = UINT32_MAX;
                    value.as.funcref.owner = NULL;
                } else if (reference_type.carrier == 0x6fu) {
                    value.kind = TURBOWASM_VALUE_EXTERNREF;
                    value.as.externref.is_null = true;
                    value.as.externref.token = 0u;
                } else {
                    status = TURBOWASM_UNSUPPORTED;
                    break;
                }

                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            case 0xd2u: {
                turbowasm_value value = {0};

                value.kind = TURBOWASM_VALUE_FUNCREF;
                value.as.funcref.is_null = false;
                if (!turbowasm_reader_uleb32(
                        &reader,
                        &value.as.funcref.function_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    break;
                }
                value.as.funcref.owner = instance;
                status = turbowasm_const_value_stack_push(
                    &stack, value);
                break;
            }
            default:
                status = TURBOWASM_UNSUPPORTED;
                break;
        }

        if (status != TURBOWASM_OK)
            break;
    }

    if (status == TURBOWASM_OK) {
        if (stack.size != 1u) {
            status = TURBOWASM_MALFORMED_MODULE;
        } else {
            *out = stack.values[0];
        }
    }

    free(stack.values);
    return status;
}

static turbowasm_status turbowasm_eval_i32_expr(
    const turbowasm_instance_impl *instance,
    const turbowasm_validation_expr_span *expression,
    uint32_t *out) {
    turbowasm_value value;
    turbowasm_status status = turbowasm_eval_value_expr(
        instance, expression, &value);

    if (status != TURBOWASM_OK)
        return status;
    if (value.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_MALFORMED_MODULE;
    *out = (uint32_t)value.as.i32;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_eval_table_expr(
    const turbowasm_instance_impl *instance,
    const turbowasm_validation_expr_span *expression,
    uint8_t reference_type,
    turbowasm_instance_table_entry *out) {
    turbowasm_value value;
    turbowasm_status status;

    if (expression == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_eval_value_expr(
        instance, expression, &value);
    if (status != TURBOWASM_OK)
        return status;

    if ((reference_type == 0x70u &&
         value.kind != TURBOWASM_VALUE_FUNCREF) ||
        (reference_type == 0x6fu &&
         value.kind != TURBOWASM_VALUE_EXTERNREF))
        return TURBOWASM_TYPE_MISMATCH;

    memset(out, 0, sizeof(*out));
    out->value = value;
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

        if (global->imported) {
            if (index >= instance->linked_global_count ||
                instance->linked_globals[index].provider == NULL)
                return TURBOWASM_UNSUPPORTED;
            continue;
        }

        expression.bytes = global->initializer;
        expression.size = global->initializer_size;
        expression.result_type = global->value_type;
        status = turbowasm_eval_value_expr(
            instance, &expression,
            &instance->globals[index]);
        if (status != TURBOWASM_OK)
            return status;
        if (instance->globals[index].kind !=
            turbowasm_state_kind_from_valtype(
                global->value_type))
            return TURBOWASM_MALFORMED_MODULE;
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_storage_init(
    turbowasm_instance_memory *memory,
    bool shared,
    size_t bytes) {
    turbowasm_memory_waiter *waiters = NULL;
    uint8_t *data = NULL;
    uint32_t initialized_waiters = 0u;
    bool lock_initialized = false;
    bool waiter_mutex_initialized = false;

    if (memory == NULL || memory->storage_initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    if (bytes != 0u) {
        data = (uint8_t *)calloc(bytes, 1u);
        if (data == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (shared) {
        if (salts_rwlock_init(&memory->access_lock) != 0)
            goto out_of_memory;
        lock_initialized = true;

        salts_mutex_init(&memory->waiter_mutex);
        if (memory->waiter_mutex == NULL)
            goto out_of_memory;
        waiter_mutex_initialized = true;

        waiters = (turbowasm_memory_waiter *)calloc(
            TURBOWASM_MEMORY_WAITER_CAPACITY,
            sizeof(*waiters));
        if (waiters == NULL)
            goto out_of_memory;

        while (initialized_waiters <
               TURBOWASM_MEMORY_WAITER_CAPACITY) {
            salts_cond_init(
                &waiters[initialized_waiters].condition);
            if (waiters[initialized_waiters].condition == NULL)
                goto out_of_memory;
            ++initialized_waiters;
        }
    }

    memory->data = data;
    memory->waiters = waiters;
    memory->waiter_count = 0u;
    memory->waiter_capacity = shared
        ? TURBOWASM_MEMORY_WAITER_CAPACITY
        : 0u;
    memory->shared = shared;
    memory->access_lock_initialized = lock_initialized;
    memory->waiter_mutex_initialized =
        waiter_mutex_initialized;
    memory->storage_initialized = true;
    return TURBOWASM_OK;

out_of_memory:
    while (initialized_waiters != 0u) {
        --initialized_waiters;
        salts_cond_destroy(
            &waiters[initialized_waiters].condition);
    }
    free(waiters);
    if (waiter_mutex_initialized)
        salts_mutex_destroy(&memory->waiter_mutex);
    if (lock_initialized)
        salts_rwlock_destroy(&memory->access_lock);
    free(data);
    memory->access_lock = NULL;
    memory->waiter_mutex = NULL;
    return TURBOWASM_OUT_OF_MEMORY;
}

void turbowasm_instance_memory_storage_destroy(
    turbowasm_instance_memory *memory) {
    uint32_t index;

    if (memory == NULL || !memory->storage_initialized)
        return;

    /*
     * Instance lifetime is externally synchronized. Active waiters therefore
     * must have completed before the backing is destroyed.
     */

    if (memory->waiters != NULL) {
        for (index = 0u;
             index < memory->waiter_capacity;
             ++index) {
            salts_cond_destroy(
                &memory->waiters[index].condition);
        }
    }
    free(memory->waiters);
    memory->waiters = NULL;
    memory->waiter_capacity = 0u;

    if (memory->waiter_mutex_initialized)
        salts_mutex_destroy(&memory->waiter_mutex);
    memory->waiter_mutex = NULL;
    memory->waiter_mutex_initialized = false;

    free(memory->data);
    memory->data = NULL;

    if (memory->access_lock_initialized)
        salts_rwlock_destroy(&memory->access_lock);

    memory->access_lock = NULL;
    memory->access_lock_initialized = false;
    memory->shared = false;
    memory->storage_initialized = false;
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
            source->page_size;

        if (source->imported) {
            if (index >= instance->linked_memory_count ||
                instance->linked_memories[index].provider == NULL)
                return TURBOWASM_UNSUPPORTED;
            continue;
        }
        if (bytes > (uint64_t)SIZE_MAX)
            return TURBOWASM_OUT_OF_MEMORY;

        memory->pages = source->limits.minimum;
        memory->maximum_pages = source->limits.maximum;
        memory->page_size = source->page_size;
        memory->has_maximum = source->limits.has_maximum;

        {
            turbowasm_status status =
                turbowasm_instance_memory_storage_init(
                    memory, source->shared, (size_t)bytes);
            if (status != TURBOWASM_OK)
                return status;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_table_initial_value(
    turbowasm_instance_impl *instance,
    const turbowasm_validation_table *source,
    turbowasm_instance_table_entry *out) {
    turbowasm_value value = {0};
    turbowasm_status status;

    if (instance == NULL || source == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (source->initializer != NULL) {
        turbowasm_validation_expr_span expression;

        if (source->initializer_size == 0u)
            return TURBOWASM_MALFORMED_MODULE;

        expression.bytes = source->initializer;
        expression.size = source->initializer_size;
        expression.result_type = source->reference_type;
        status = turbowasm_eval_value_expr(
            instance, &expression, &value);
        if (status != TURBOWASM_OK)
            return status;
    } else if (source->reference_type == 0x70u) {
        value.kind = TURBOWASM_VALUE_FUNCREF;
        value.as.funcref.is_null = true;
        value.as.funcref.function_index = UINT32_MAX;
        value.as.funcref.owner = NULL;
    } else if (source->reference_type == 0x6fu) {
        value.kind = TURBOWASM_VALUE_EXTERNREF;
        value.as.externref.is_null = true;
        value.as.externref.token = 0u;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    if ((source->reference_type == 0x70u &&
         value.kind != TURBOWASM_VALUE_FUNCREF) ||
        (source->reference_type == 0x6fu &&
         value.kind != TURBOWASM_VALUE_EXTERNREF))
        return TURBOWASM_MALFORMED_MODULE;

    memset(out, 0, sizeof(*out));
    out->value = value;
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
        turbowasm_instance_table_entry initial;
        uint32_t item;
        turbowasm_status status;

        if (source->imported) {
            if (index >= instance->linked_table_count ||
                instance->linked_tables[index].provider == NULL)
                return TURBOWASM_UNSUPPORTED;
            continue;
        }
        if (source->reference_type != 0x70u &&
            source->reference_type != 0x6fu)
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

        status = turbowasm_table_initial_value(
            instance, source, &initial);
        if (status != TURBOWASM_OK)
            return status;

        for (item = 0u; item < table->size; ++item)
            table->entries[item] = initial;
    }

    return TURBOWASM_OK;
}

static const turbowasm_instance_table *
turbowasm_instance_table_resolve_const(
    const turbowasm_instance_impl *instance,
    uint32_t table_index) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_table *table;
    const turbowasm_linked_table *binding;

    if (instance == NULL || table_index >= instance->table_count)
        return NULL;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        table_index >= module->validation.table_count)
        return NULL;

    table = &module->validation.tables[table_index];
    if (!table->imported)
        return &instance->tables[table_index];

    if (table_index >= instance->linked_table_count)
        return NULL;
    binding = &instance->linked_tables[table_index];
    if (binding->provider == NULL)
        return NULL;

    return turbowasm_instance_table_resolve_const(
        binding->provider,
        binding->table_index);
}

static turbowasm_instance_table *
turbowasm_instance_table_resolve(
    turbowasm_instance_impl *instance,
    uint32_t table_index) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_table *table;
    const turbowasm_linked_table *binding;

    if (instance == NULL || table_index >= instance->table_count)
        return NULL;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        table_index >= module->validation.table_count)
        return NULL;

    table = &module->validation.tables[table_index];
    if (!table->imported)
        return &instance->tables[table_index];

    if (table_index >= instance->linked_table_count)
        return NULL;
    binding = &instance->linked_tables[table_index];
    if (binding->provider == NULL)
        return NULL;

    return turbowasm_instance_table_resolve(
        binding->provider,
        binding->table_index);
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
        turbowasm_status status;

        if (segment->mode !=
            TURBOWASM_VALIDATION_SEGMENT_ACTIVE)
            continue;

        status = turbowasm_eval_i32_expr(
            instance, &segment->offset, &offset);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_instance_memory_write_bytes(
            instance,
            segment->memory_index,
            offset,
            0u,
            segment->data,
            segment->data_size);
        if (status != TURBOWASM_OK)
            return status == TURBOWASM_TRAPPED
                ? TURBOWASM_TRAPPED
                : status;
    }

    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_element_item_value(
    const turbowasm_instance_impl *instance,
    const turbowasm_validation_element_item *item,
    uint8_t reference_type,
    turbowasm_instance_table_entry *out) {
    if (item == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (item->kind ==
        TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX) {
        if (reference_type != 0x70u)
            return TURBOWASM_TYPE_MISMATCH;
        out->value.kind = TURBOWASM_VALUE_FUNCREF;
        out->value.as.funcref.is_null = false;
        out->value.as.funcref.function_index =
            item->function_index;
        out->value.as.funcref.owner =
            (turbowasm_instance_impl *)instance;
        return TURBOWASM_OK;
    }

    return turbowasm_eval_table_expr(
        instance, &item->expression,
        reference_type, out);
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

        table = turbowasm_instance_table_resolve(
            instance, segment->table_index);
        if (table == NULL)
            return TURBOWASM_UNSUPPORTED;
        if (table->reference_type != segment->reference_type)
            return TURBOWASM_MALFORMED_MODULE;

        status = turbowasm_eval_i32_expr(
            instance, &segment->offset, &offset);
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
                instance,
                &segment->items[item_index],
                table->reference_type,
                &value);
            if (status != TURBOWASM_OK)
                return status;
            table->entries[offset + item_index] = value;
        }
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_tag_identity(
    const turbowasm_instance_impl *instance,
    uint32_t tag_index,
    turbowasm_tag_identity *out) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_tag *tag;

    if (instance == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    tag = turbowasm_validation_context_tag(
        &module->validation, tag_index);
    if (tag == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    if (!tag->imported) {
        out->owner = (turbowasm_instance_impl *)instance;
        out->tag_index = tag_index;
        return TURBOWASM_OK;
    }

    if (tag_index >= instance->linked_tag_count ||
        instance->linked_tags == NULL ||
        instance->linked_tags[tag_index].provider == NULL)
        return TURBOWASM_LINK_ERROR;

    {
        turbowasm_instance_impl *owner =
            instance->linked_tags[tag_index].provider;
        uint32_t owner_index =
            instance->linked_tags[tag_index].tag_index;
        const turbowasm_module_impl *owner_module =
            turbowasm_module_impl_get(owner->module);
        const turbowasm_validation_tag *owner_tag;

        if (owner_module == NULL)
            return TURBOWASM_LINK_ERROR;
        owner_tag = turbowasm_validation_context_tag(
            &owner_module->validation, owner_index);
        if (owner_tag == NULL || owner_tag->imported)
            return TURBOWASM_LINK_ERROR;

        out->owner = owner;
        out->tag_index = owner_index;
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

    /*
     * Instantiation initializes active element segments before active data
     * segments. Side effects to imported store objects are observable even if
     * a later segment traps, so this order is semantically significant.
     */
    status = turbowasm_apply_element_segments(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_apply_data_segments(
        instance, &module->validation);
    if (status != TURBOWASM_OK)
        return status;

    return TURBOWASM_OK;

fail:
    /*
     * The creator owns cleanup policy. Public creation destroys this partial
     * state; the private spec-store path may retain it after an instantiation
     * trap so escaped references remain alive.
     */
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

    {
        const turbowasm_module_impl *module =
            turbowasm_module_impl_get(instance->module);

        for (index = 0u; index < instance->memory_count; ++index) {
            if (module != NULL &&
                index < module->validation.memory_count &&
                module->validation.memories[index].imported)
                continue;
            turbowasm_instance_memory_storage_destroy(
                &instance->memories[index]);
        }
    }
    free(instance->memories);
    instance->memories = NULL;
    instance->memory_count = 0u;

    {
        const turbowasm_module_impl *module =
            turbowasm_module_impl_get(instance->module);

        for (index = 0u; index < instance->table_count; ++index) {
            if (module != NULL &&
                index < module->validation.table_count &&
                module->validation.tables[index].imported)
                continue;
            free(instance->tables[index].entries);
        }
    }
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
    const turbowasm_module_impl *module;
    const turbowasm_validation_global *global;

    if (instance == NULL || out == NULL ||
        index >= instance->global_count)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        index >= module->validation.global_count)
        return TURBOWASM_INVALID_ARGUMENT;
    global = &module->validation.globals[index];

    if (global->imported) {
        const turbowasm_linked_global *binding;

        if (index >= instance->linked_global_count)
            return TURBOWASM_UNSUPPORTED;
        binding = &instance->linked_globals[index];
        if (binding->provider == NULL)
            return TURBOWASM_UNSUPPORTED;

        return turbowasm_instance_global_get(
            binding->provider,
            binding->global_index,
            out);
    }

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

    if (global->imported) {
        const turbowasm_linked_global *binding;

        if (index >= instance->linked_global_count)
            return TURBOWASM_UNSUPPORTED;
        binding = &instance->linked_globals[index];
        if (binding->provider == NULL)
            return TURBOWASM_UNSUPPORTED;

        return turbowasm_instance_global_set(
            binding->provider,
            binding->global_index,
            value);
    }

    if (!global->mutable_value)
        return TURBOWASM_TYPE_MISMATCH;
    if (value.kind !=
        turbowasm_state_kind_from_valtype(global->value_type))
        return TURBOWASM_TYPE_MISMATCH;

    instance->globals[index] = value;
    return TURBOWASM_OK;
}

static const turbowasm_instance_memory *
turbowasm_instance_memory_resolve_const(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_memory *memory;
    const turbowasm_linked_memory *binding;

    if (instance == NULL || memory_index >= instance->memory_count)
        return NULL;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        memory_index >= module->validation.memory_count)
        return NULL;

    memory = &module->validation.memories[memory_index];
    if (!memory->imported)
        return &instance->memories[memory_index];

    if (memory_index >= instance->linked_memory_count)
        return NULL;
    binding = &instance->linked_memories[memory_index];
    if (binding->provider == NULL)
        return NULL;

    return turbowasm_instance_memory_resolve_const(
        binding->provider,
        binding->memory_index);
}

static turbowasm_instance_memory *
turbowasm_instance_memory_resolve(
    turbowasm_instance_impl *instance,
    uint32_t memory_index) {
    const turbowasm_module_impl *module;
    const turbowasm_validation_memory *memory;
    const turbowasm_linked_memory *binding;

    if (instance == NULL || memory_index >= instance->memory_count)
        return NULL;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL ||
        memory_index >= module->validation.memory_count)
        return NULL;

    memory = &module->validation.memories[memory_index];
    if (!memory->imported)
        return &instance->memories[memory_index];

    if (memory_index >= instance->linked_memory_count)
        return NULL;
    binding = &instance->linked_memories[memory_index];
    if (binding->provider == NULL)
        return NULL;

    return turbowasm_instance_memory_resolve(
        binding->provider,
        binding->memory_index);
}

turbowasm_status turbowasm_instance_memory_shared(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    bool *out_shared) {
    const turbowasm_instance_memory *memory;

    if (instance == NULL || out_shared == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve_const(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    *out_shared = memory->shared;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_instance_memory_storage_range(
    const turbowasm_instance_memory *memory,
    uint32_t address,
    uint32_t offset,
    size_t width,
    size_t *out_effective) {
    uint64_t effective;
    uint64_t size;

    if (memory == NULL || out_effective == NULL ||
        !memory->storage_initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    effective = (uint64_t)address + (uint64_t)offset;
    size = (uint64_t)memory->pages *
           (uint64_t)memory->page_size;

    if (effective > size ||
        (uint64_t)width > size - effective)
        return TURBOWASM_TRAPPED;
    if (effective > (uint64_t)SIZE_MAX)
        return TURBOWASM_TRAPPED;

    *out_effective = (size_t)effective;
    return TURBOWASM_OK;
}

static void turbowasm_instance_memory_rdlock(
    turbowasm_instance_memory *memory) {
    if (memory != NULL && memory->shared &&
        memory->access_lock_initialized)
        salts_rwlock_rdlock(&memory->access_lock);
}

static void turbowasm_instance_memory_rdunlock(
    turbowasm_instance_memory *memory) {
    if (memory != NULL && memory->shared &&
        memory->access_lock_initialized)
        salts_rwlock_rdunlock(&memory->access_lock);
}

static void turbowasm_instance_memory_wrlock(
    turbowasm_instance_memory *memory) {
    if (memory != NULL && memory->shared &&
        memory->access_lock_initialized)
        salts_rwlock_wrlock(&memory->access_lock);
}

static void turbowasm_instance_memory_wrunlock(
    turbowasm_instance_memory *memory) {
    if (memory != NULL && memory->shared &&
        memory->access_lock_initialized)
        salts_rwlock_wrunlock(&memory->access_lock);
}

turbowasm_status turbowasm_instance_memory_read_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    void *out,
    size_t width) {
    turbowasm_instance_memory *memory;
    size_t effective = 0u;
    turbowasm_status status;

    if (instance == NULL || (width != 0u && out == NULL) ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_rdlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, address, offset, width, &effective);
    if (status == TURBOWASM_OK && width != 0u)
        memmove(out, memory->data + effective, width);
    turbowasm_instance_memory_rdunlock(memory);
    return status;
}

turbowasm_status turbowasm_instance_memory_write_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    const void *source,
    size_t width) {
    turbowasm_instance_memory *memory;
    size_t effective = 0u;
    turbowasm_status status;

    if (instance == NULL || (width != 0u && source == NULL) ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_wrlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, address, offset, width, &effective);
    if (status == TURBOWASM_OK && width != 0u)
        memmove(memory->data + effective, source, width);
    turbowasm_instance_memory_wrunlock(memory);
    return status;
}

turbowasm_status turbowasm_instance_memory_fill_bytes(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t destination,
    uint8_t value,
    size_t length) {
    turbowasm_instance_memory *memory;
    size_t effective = 0u;
    turbowasm_status status;

    if (instance == NULL || memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_wrlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, destination, 0u, length, &effective);
    if (status == TURBOWASM_OK && length != 0u)
        memset(memory->data + effective, value, length);
    turbowasm_instance_memory_wrunlock(memory);
    return status;
}

turbowasm_status turbowasm_instance_memory_copy_bytes(
    turbowasm_instance_impl *instance,
    uint32_t destination_memory,
    uint32_t source_memory,
    uint32_t destination,
    uint32_t source,
    size_t length) {
    turbowasm_instance_memory *dst;
    turbowasm_instance_memory *src;
    turbowasm_instance_memory *first;
    turbowasm_instance_memory *second;
    bool first_write;
    bool second_write;
    size_t destination_effective = 0u;
    size_t source_effective = 0u;
    turbowasm_status status;

    if (instance == NULL ||
        destination_memory >= instance->memory_count ||
        source_memory >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    dst = turbowasm_instance_memory_resolve(
        instance, destination_memory);
    src = turbowasm_instance_memory_resolve(
        instance, source_memory);
    if (dst == NULL || src == NULL)
        return TURBOWASM_UNSUPPORTED;

    if (dst == src) {
        turbowasm_instance_memory_wrlock(dst);
        status = turbowasm_instance_memory_storage_range(
            dst, destination, 0u, length,
            &destination_effective);
        if (status == TURBOWASM_OK) {
            status = turbowasm_instance_memory_storage_range(
                src, source, 0u, length,
                &source_effective);
        }
        if (status == TURBOWASM_OK && length != 0u) {
            memmove(
                dst->data + destination_effective,
                src->data + source_effective,
                length);
        }
        turbowasm_instance_memory_wrunlock(dst);
        return status;
    }

    if ((uintptr_t)dst < (uintptr_t)src) {
        first = dst;
        second = src;
        first_write = true;
        second_write = false;
    } else {
        first = src;
        second = dst;
        first_write = false;
        second_write = true;
    }

    if (first_write)
        turbowasm_instance_memory_wrlock(first);
    else
        turbowasm_instance_memory_rdlock(first);
    if (second_write)
        turbowasm_instance_memory_wrlock(second);
    else
        turbowasm_instance_memory_rdlock(second);

    status = turbowasm_instance_memory_storage_range(
        dst, destination, 0u, length,
        &destination_effective);
    if (status == TURBOWASM_OK) {
        status = turbowasm_instance_memory_storage_range(
            src, source, 0u, length,
            &source_effective);
    }
    if (status == TURBOWASM_OK && length != 0u) {
        memmove(
            dst->data + destination_effective,
            src->data + source_effective,
            length);
    }

    if (second_write)
        turbowasm_instance_memory_wrunlock(second);
    else
        turbowasm_instance_memory_rdunlock(second);
    if (first_write)
        turbowasm_instance_memory_wrunlock(first);
    else
        turbowasm_instance_memory_rdunlock(first);

    return status;
}

static uint64_t turbowasm_atomic_load_le(
    const uint8_t *data,
    uint8_t width) {
    uint64_t value = 0u;
    uint8_t index;

    for (index = 0u; index < width; ++index)
        value |= (uint64_t)data[index] << (8u * index);
    return value;
}

static void turbowasm_atomic_store_le(
    uint8_t *data,
    uint8_t width,
    uint64_t value) {
    uint8_t index;

    for (index = 0u; index < width; ++index)
        data[index] = (uint8_t)(value >> (8u * index));
}

static uint64_t turbowasm_atomic_width_mask(
    uint8_t width) {
    if (width >= 8u)
        return UINT64_MAX;
    return (UINT64_C(1) << (width * 8u)) - UINT64_C(1);
}

turbowasm_status turbowasm_instance_memory_atomic(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    const turbowasm_atomic_descriptor *descriptor,
    uint64_t value,
    uint64_t expected,
    uint64_t replacement,
    uint64_t *out_old,
    turbowasm_trap *trap) {
    turbowasm_instance_memory *memory;
    uint64_t effective64;
    uint64_t mask;
    uint64_t old_value;
    uint64_t new_value = 0u;
    size_t effective = 0u;
    turbowasm_status status;

    if (instance == NULL || descriptor == NULL ||
        out_old == NULL || trap == NULL ||
        memory_index >= instance->memory_count ||
        (descriptor->width != 1u &&
         descriptor->width != 2u &&
         descriptor->width != 4u &&
         descriptor->width != 8u))
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    *out_old = 0u;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    effective64 = (uint64_t)address + (uint64_t)offset;
    if ((effective64 % descriptor->width) != 0u) {
        *trap = TURBOWASM_TRAP_UNALIGNED_ATOMIC;
        return TURBOWASM_TRAPPED;
    }

    /*
     * The Runtime-wide SC lock gives one total order across different linear
     * memories. The backing write lock then makes this individual memory
     * transaction indivisible.
     */
    if (!turbowasm_sc_lock())
        return TURBOWASM_OUT_OF_MEMORY;
    turbowasm_instance_memory_wrlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, address, offset,
        descriptor->width, &effective);
    if (status != TURBOWASM_OK) {
        turbowasm_instance_memory_wrunlock(memory);
        turbowasm_sc_unlock();
        if (status == TURBOWASM_TRAPPED)
            *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return status;
    }

    old_value = turbowasm_atomic_load_le(
        memory->data + effective,
        descriptor->width);
    mask = turbowasm_atomic_width_mask(
        descriptor->width);

    switch (descriptor->kind) {
        case TURBOWASM_ATOMIC_LOAD:
            break;

        case TURBOWASM_ATOMIC_STORE:
            turbowasm_atomic_store_le(
                memory->data + effective,
                descriptor->width,
                value & mask);
            break;

        case TURBOWASM_ATOMIC_RMW:
            value &= mask;
            switch (descriptor->op) {
                case TURBOWASM_ATOMIC_OP_ADD:
                    new_value = (old_value + value) & mask;
                    break;
                case TURBOWASM_ATOMIC_OP_SUB:
                    new_value = (old_value - value) & mask;
                    break;
                case TURBOWASM_ATOMIC_OP_AND:
                    new_value = old_value & value;
                    break;
                case TURBOWASM_ATOMIC_OP_OR:
                    new_value = old_value | value;
                    break;
                case TURBOWASM_ATOMIC_OP_XOR:
                    new_value = old_value ^ value;
                    break;
                case TURBOWASM_ATOMIC_OP_XCHG:
                    new_value = value;
                    break;
                default:
                    turbowasm_instance_memory_wrunlock(memory);
                    turbowasm_sc_unlock();
                    return TURBOWASM_INVALID_ARGUMENT;
            }
            turbowasm_atomic_store_le(
                memory->data + effective,
                descriptor->width,
                new_value & mask);
            break;

        case TURBOWASM_ATOMIC_CMPXCHG:
            expected &= mask;
            replacement &= mask;
            if (old_value == expected) {
                turbowasm_atomic_store_le(
                    memory->data + effective,
                    descriptor->width,
                    replacement);
            }
            break;

        default:
            turbowasm_instance_memory_wrunlock(memory);
            turbowasm_sc_unlock();
            return TURBOWASM_INVALID_ARGUMENT;
    }

    *out_old = old_value & mask;
    turbowasm_instance_memory_wrunlock(memory);
    turbowasm_sc_unlock();
    return TURBOWASM_OK;
}

static turbowasm_memory_waiter *
turbowasm_memory_waiter_acquire_slot(
    turbowasm_instance_memory *memory) {
    uint32_t index;

    if (memory == NULL || memory->waiters == NULL)
        return NULL;

    for (index = 0u; index < memory->waiter_capacity; ++index) {
        if (!memory->waiters[index].active)
            return &memory->waiters[index];
    }
    return NULL;
}

static turbowasm_status turbowasm_instance_memory_wait_internal(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    uint8_t width,
    uint64_t expected,
    int64_t timeout_ns,
    turbowasm_interrupt_check_fn should_interrupt,
    void *interrupt_context,
    uint32_t *out_result,
    turbowasm_trap *trap) {
    turbowasm_instance_memory *memory;
    turbowasm_memory_waiter *waiter = NULL;
    uint64_t effective64;
    uint64_t observed;
    uint64_t deadline = 0u;
    size_t effective = 0u;
    turbowasm_status status;
    int wait_status = 0;
    bool interrupted = false;

    if (instance == NULL || out_result == NULL || trap == NULL ||
        memory_index >= instance->memory_count ||
        (width != 4u && width != 8u))
        return TURBOWASM_INVALID_ARGUMENT;

    *out_result = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    effective64 = (uint64_t)address + (uint64_t)offset;
    if ((effective64 % width) != 0u) {
        *trap = TURBOWASM_TRAP_UNALIGNED_ATOMIC;
        return TURBOWASM_TRAPPED;
    }

    /*
     * Per the core execution order, wait on an unshared memory traps before
     * performing the shared-memory bounds/value action.
     */
    if (!memory->shared) {
        *trap = TURBOWASM_TRAP_EXPECTED_SHARED_MEMORY;
        return TURBOWASM_TRAPPED;
    }

    if (!memory->waiter_mutex_initialized ||
        memory->waiter_mutex == NULL)
        return TURBOWASM_UNSUPPORTED;

    if (!turbowasm_sc_lock())
        return TURBOWASM_OUT_OF_MEMORY;

    /*
     * Registry mutex closes the notify race between the expected-value load
     * and publishing this waiter. Data lock is never held across sleeping.
     */
    salts_mutex_lock(&memory->waiter_mutex);
    turbowasm_instance_memory_wrlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, address, offset, width, &effective);
    if (status == TURBOWASM_OK) {
        observed = turbowasm_atomic_load_le(
            memory->data + effective, width);
    } else {
        observed = 0u;
    }
    turbowasm_instance_memory_wrunlock(memory);

    if (status != TURBOWASM_OK) {
        salts_mutex_unlock(&memory->waiter_mutex);
        turbowasm_sc_unlock();
        if (status == TURBOWASM_TRAPPED)
            *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return status;
    }

    if (observed !=
        (expected & turbowasm_atomic_width_mask(width))) {
        salts_mutex_unlock(&memory->waiter_mutex);
        turbowasm_sc_unlock();
        *out_result = 1u;
        return TURBOWASM_OK;
    }

    if (timeout_ns == 0) {
        salts_mutex_unlock(&memory->waiter_mutex);
        turbowasm_sc_unlock();
        *out_result = 2u;
        return TURBOWASM_OK;
    }

    waiter = turbowasm_memory_waiter_acquire_slot(memory);
    if (waiter == NULL) {
        salts_mutex_unlock(&memory->waiter_mutex);
        turbowasm_sc_unlock();
        *trap = TURBOWASM_TRAP_TOO_MANY_WAITERS;
        return TURBOWASM_TRAPPED;
    }

    waiter->address = effective64;
    waiter->notified = false;
    waiter->active = true;
    ++memory->waiter_count;

    if (timeout_ns > 0) {
        uint64_t now = salts_hrtime();
        uint64_t timeout = (uint64_t)timeout_ns;
        deadline = UINT64_MAX - now < timeout
            ? UINT64_MAX
            : now + timeout;
    }

    /*
     * Registration is now visible to notify under waiter_mutex. Release the
     * global SC point before blocking, while retaining waiter_mutex until the
     * cond wait atomically releases it.
     */
    turbowasm_sc_unlock();

    while (!waiter->notified) {
        /*
         * Check while holding waiter_mutex. If the interrupt becomes true just
         * after this check, the interrupter blocks on the same mutex until the
         * condition wait atomically releases it, then signals this waiter.
         */
        if (should_interrupt != NULL &&
            should_interrupt(interrupt_context)) {
            interrupted = true;
            break;
        }

        if (timeout_ns < 0) {
            salts_cond_wait(
                &waiter->condition,
                &memory->waiter_mutex);
            continue;
        }

        {
            uint64_t now = salts_hrtime();
            uint64_t remaining;
            if (now >= deadline) {
                wait_status = -ETIMEDOUT;
                break;
            }
            remaining = deadline - now;
            wait_status = salts_cond_timedwait(
                &waiter->condition,
                &memory->waiter_mutex,
                remaining);
        }

        if (waiter->notified)
            break;
        if (wait_status == -ETIMEDOUT)
            break;
        if (wait_status != 0)
            break;
        /* Spurious/interrupt wake: re-check policy and original deadline. */
    }

    if (waiter->notified)
        *out_result = 0u;
    else if (wait_status == -ETIMEDOUT)
        *out_result = 2u;

    waiter->active = false;
    waiter->notified = false;
    waiter->address = 0u;
    if (memory->waiter_count != 0u)
        --memory->waiter_count;
    salts_mutex_unlock(&memory->waiter_mutex);

    if (interrupted)
        return TURBOWASM_INTERRUPTED;

    if (wait_status != 0 &&
        wait_status != -ETIMEDOUT)
        return TURBOWASM_UNSUPPORTED;

    /*
     * Place successful wake completion after the notifying SC action in the
     * global order without inverting SC->waiter_mutex lock ordering.
     */
    if (*out_result == 0u)
        return turbowasm_threads_sc_fence();

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_wait(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    uint8_t width,
    uint64_t expected,
    int64_t timeout_ns,
    uint32_t *out_result,
    turbowasm_trap *trap) {
    return turbowasm_instance_memory_wait_internal(
        instance, memory_index, address, offset, width,
        expected, timeout_ns, NULL, NULL, out_result, trap);
}

turbowasm_status turbowasm_instance_memory_wait_with_interrupt(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    uint8_t width,
    uint64_t expected,
    int64_t timeout_ns,
    turbowasm_interrupt_check_fn should_interrupt,
    void *interrupt_context,
    uint32_t *out_result,
    turbowasm_trap *trap) {
    return turbowasm_instance_memory_wait_internal(
        instance, memory_index, address, offset, width,
        expected, timeout_ns,
        should_interrupt, interrupt_context,
        out_result, trap);
}

void turbowasm_instance_interrupt_waiters(
    turbowasm_instance_impl *instance) {
    uint32_t memory_index;

    if (instance == NULL)
        return;

    for (memory_index = 0u;
         memory_index < instance->memory_count;
         ++memory_index) {
        turbowasm_instance_memory *memory =
            turbowasm_instance_memory_resolve(
                instance, memory_index);
        uint32_t waiter_index;

        if (memory == NULL || !memory->shared ||
            !memory->waiter_mutex_initialized ||
            memory->waiter_mutex == NULL)
            continue;

        salts_mutex_lock(&memory->waiter_mutex);
        for (waiter_index = 0u;
             waiter_index < memory->waiter_capacity;
             ++waiter_index) {
            turbowasm_memory_waiter *waiter =
                &memory->waiters[waiter_index];
            if (waiter->active)
                salts_cond_signal(&waiter->condition);
        }
        salts_mutex_unlock(&memory->waiter_mutex);
    }
}

turbowasm_status turbowasm_instance_memory_notify(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t address,
    uint32_t offset,
    uint32_t count,
    uint32_t *out_woken,
    turbowasm_trap *trap) {
    turbowasm_instance_memory *memory;
    uint64_t effective64;
    size_t effective = 0u;
    uint32_t woken = 0u;
    uint32_t index;
    turbowasm_status status;

    if (instance == NULL || out_woken == NULL || trap == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    *out_woken = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    effective64 = (uint64_t)address + (uint64_t)offset;
    if ((effective64 % 4u) != 0u) {
        *trap = TURBOWASM_TRAP_UNALIGNED_ATOMIC;
        return TURBOWASM_TRAPPED;
    }

    if (!turbowasm_sc_lock())
        return TURBOWASM_OUT_OF_MEMORY;

    turbowasm_instance_memory_rdlock(memory);
    status = turbowasm_instance_memory_storage_range(
        memory, address, offset, 4u, &effective);
    turbowasm_instance_memory_rdunlock(memory);
    (void)effective;

    if (status != TURBOWASM_OK) {
        turbowasm_sc_unlock();
        if (status == TURBOWASM_TRAPPED)
            *trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        return status;
    }

    if (!memory->shared) {
        turbowasm_sc_unlock();
        return TURBOWASM_OK;
    }

    if (!memory->waiter_mutex_initialized ||
        memory->waiter_mutex == NULL) {
        turbowasm_sc_unlock();
        return TURBOWASM_UNSUPPORTED;
    }

    salts_mutex_lock(&memory->waiter_mutex);
    for (index = 0u;
         index < memory->waiter_capacity &&
         woken < count;
         ++index) {
        turbowasm_memory_waiter *waiter =
            &memory->waiters[index];

        if (!waiter->active ||
            waiter->notified ||
            waiter->address != effective64)
            continue;

        waiter->notified = true;
        ++woken;
        salts_cond_signal(&waiter->condition);
    }
    salts_mutex_unlock(&memory->waiter_mutex);
    turbowasm_sc_unlock();

    *out_woken = woken;
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

    memory = turbowasm_instance_memory_resolve_const(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    /*
     * Raw borrowed pointers are unshared-only. Shared callers must use the
     * guarded read/write/fill/copy substrate so the lock lifetime covers the
     * whole logical memory operation.
     */
    if (memory->shared)
        return TURBOWASM_UNSUPPORTED;

    effective = (uint64_t)address + offset;
    size = (uint64_t)memory->pages *
           memory->page_size;

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
    const turbowasm_instance_memory *memory;

    if (instance == NULL || out_pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve_const(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_rdlock(
        (turbowasm_instance_memory *)memory);
    *out_pages = memory->pages;
    turbowasm_instance_memory_rdunlock(
        (turbowasm_instance_memory *)memory);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_limits(
    const turbowasm_instance_impl *instance,
    uint32_t memory_index,
    turbowasm_instance_limits *out_limits) {
    const turbowasm_instance_memory *memory;

    if (instance == NULL || out_limits == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve_const(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_rdlock(
        (turbowasm_instance_memory *)memory);
    out_limits->minimum = memory->pages;
    out_limits->maximum = memory->maximum_pages;
    out_limits->has_maximum = memory->has_maximum;
    turbowasm_instance_memory_rdunlock(
        (turbowasm_instance_memory *)memory);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_memory_grow(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t delta_pages,
    uint32_t *out_previous_pages) {
    turbowasm_instance_memory *memory;
    uint64_t next_pages;
    uint64_t maximum_pages;
    uint64_t next_bytes;
    uint64_t previous_bytes;
    uint8_t *grown;

    if (instance == NULL || out_previous_pages == NULL ||
        memory_index >= instance->memory_count)
        return TURBOWASM_INVALID_ARGUMENT;

    memory = turbowasm_instance_memory_resolve(
        instance, memory_index);
    if (memory == NULL)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_instance_memory_wrlock(memory);

    *out_previous_pages = memory->pages;

    next_pages = (uint64_t)memory->pages + delta_pages;
    maximum_pages = memory->page_size == UINT32_C(1)
        ? UINT32_MAX
        : (UINT64_C(1) << 32) / memory->page_size;
    if (memory->page_size == 0u ||
        next_pages > maximum_pages ||
        (memory->has_maximum &&
         next_pages > memory->maximum_pages)) {
        *out_previous_pages = UINT32_MAX;
        turbowasm_instance_memory_wrunlock(memory);
        return TURBOWASM_OK;
    }

    next_bytes = next_pages * memory->page_size;
    previous_bytes =
        (uint64_t)memory->pages * memory->page_size;
    if (next_bytes > (uint64_t)SIZE_MAX) {
        *out_previous_pages = UINT32_MAX;
        turbowasm_instance_memory_wrunlock(memory);
        return TURBOWASM_OK;
    }

    if (next_bytes == 0u) {
        memory->pages = (uint32_t)next_pages;
        turbowasm_instance_memory_wrunlock(memory);
        return TURBOWASM_OK;
    }

    grown = (uint8_t *)realloc(
        memory->data, (size_t)next_bytes);
    if (grown == NULL) {
        *out_previous_pages = UINT32_MAX;
        turbowasm_instance_memory_wrunlock(memory);
        return TURBOWASM_OK;
    }

    if (next_bytes > previous_bytes)
        memset(grown + (size_t)previous_bytes,
               0,
               (size_t)(next_bytes - previous_bytes));

    memory->data = grown;
    memory->pages = (uint32_t)next_pages;
    turbowasm_instance_memory_wrunlock(memory);
    return TURBOWASM_OK;
}

static bool turbowasm_instance_funcref_owner_visible(
    const turbowasm_instance_impl *instance,
    const turbowasm_instance_impl *owner) {
    uint32_t index;

    if (instance == NULL || owner == NULL)
        return false;
    if (owner == instance)
        return true;

    for (index = 0u; index < instance->linked_function_count; ++index) {
        if (instance->linked_functions[index].provider == owner)
            return true;
    }
    for (index = 0u; index < instance->linked_global_count; ++index) {
        if (instance->linked_globals[index].provider == owner)
            return true;
    }
    for (index = 0u; index < instance->linked_memory_count; ++index) {
        if (instance->linked_memories[index].provider == owner)
            return true;
    }
    for (index = 0u; index < instance->linked_table_count; ++index) {
        if (instance->linked_tables[index].provider == owner)
            return true;
    }

    /*
     * A runtime-produced foreign ref may already be observable through a
     * shared/local table even when its owner is not a direct linker provider
     * of this instance. Compare opaque tokens stored in trusted table entries
     * before ever dereferencing the candidate owner.
     */
    for (index = 0u; index < instance->table_count; ++index) {
        const turbowasm_instance_table *table =
            turbowasm_instance_table_resolve_const(instance, index);
        uint32_t element;

        if (table == NULL)
            continue;
        for (element = 0u; element < table->size; ++element) {
            if (table->entries[element].value.kind ==
                    TURBOWASM_VALUE_FUNCREF &&
                !table->entries[element].value.as.funcref.is_null &&
                table->entries[element].value.as.funcref.owner == owner)
                return true;
        }
    }

    return false;
}

static turbowasm_status turbowasm_value_to_table_entry(
    turbowasm_instance_impl *instance,
    uint8_t reference_type,
    turbowasm_value value,
    turbowasm_instance_table_entry *out) {
    turbowasm_instance_impl *owner;
    const turbowasm_module_impl *module;

    if (instance == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (reference_type == 0x6fu) {
        if (value.kind != TURBOWASM_VALUE_EXTERNREF)
            return TURBOWASM_TYPE_MISMATCH;
        out->value = value;
        return TURBOWASM_OK;
    }

    if (reference_type != 0x70u)
        return TURBOWASM_UNSUPPORTED;
    if (value.kind != TURBOWASM_VALUE_FUNCREF)
        return TURBOWASM_TYPE_MISMATCH;

    if (value.as.funcref.is_null) {
        value.as.funcref.function_index = UINT32_MAX;
        value.as.funcref.owner = NULL;
        out->value = value;
        return TURBOWASM_OK;
    }

    owner = value.as.funcref.owner != NULL
        ? (turbowasm_instance_impl *)
            value.as.funcref.owner
        : instance;

    if (!turbowasm_instance_funcref_owner_visible(
            instance, owner))
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_module_impl_get(owner->module);
    if (module == NULL ||
        value.as.funcref.function_index >=
            module->validation.function_count)
        return TURBOWASM_INVALID_ARGUMENT;

    value.as.funcref.owner = owner;
    out->value = value;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_lookup(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_instance_table_entry *out) {
    const turbowasm_instance_table *table;

    if (instance == NULL || out == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve_const(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (element_index >= table->size)
        return TURBOWASM_TRAPPED;

    *out = table->entries[element_index];
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_get_value(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value *out) {
    turbowasm_instance_table_entry entry;
    turbowasm_status status;

    const turbowasm_instance_table *table;

    if (instance == NULL || out == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve_const(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = turbowasm_instance_table_lookup(
        instance, table_index, element_index, &entry);
    if (status != TURBOWASM_OK)
        return status;

    *out = entry.value;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_set_value(
    turbowasm_instance_impl *instance,
    uint32_t table_index,
    uint32_t element_index,
    turbowasm_value value) {
    turbowasm_instance_table *table;

    if (instance == NULL || table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (element_index >= table->size)
        return TURBOWASM_TRAPPED;

    {
        turbowasm_instance_table_entry entry;
        turbowasm_status status =
            turbowasm_value_to_table_entry(
                instance, table->reference_type,
                value, &entry);
        if (status != TURBOWASM_OK)
            return status;
        table->entries[element_index] = entry;
    }
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

    status = turbowasm_instance_memory_write_bytes(
        instance, memory_index,
        destination, 0u,
        segment->data + source,
        length);
    return status;
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
    return turbowasm_instance_memory_copy_bytes(
        instance,
        destination_memory,
        source_memory,
        destination,
        source,
        length);
}

turbowasm_status turbowasm_instance_memory_fill(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint32_t destination,
    uint8_t value,
    uint32_t length) {
    return turbowasm_instance_memory_fill_bytes(
        instance,
        memory_index,
        destination,
        value,
        length);
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
    table = turbowasm_instance_table_resolve(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (segment->reference_type != table->reference_type)
        return TURBOWASM_TYPE_MISMATCH;

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
                instance,
                &segment->items[source + index],
                table->reference_type,
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

    destination_object = turbowasm_instance_table_resolve(
        instance, destination_table);
    source_object = turbowasm_instance_table_resolve(
        instance, source_table);
    if (destination_object == NULL || source_object == NULL)
        return TURBOWASM_UNSUPPORTED;
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
    uint64_t next_size;
    turbowasm_instance_table_entry *grown;
    uint32_t index;

    if (instance == NULL || out_previous_size == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    *out_previous_size = table->size;

    {
        turbowasm_status status =
            turbowasm_value_to_table_entry(
                instance, table->reference_type,
                initial, &entry);
        if (status != TURBOWASM_OK)
            return status;
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
    const turbowasm_instance_table *table;

    if (instance == NULL || out_size == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve_const(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;

    *out_size = table->size;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_instance_table_limits(
    const turbowasm_instance_impl *instance,
    uint32_t table_index,
    turbowasm_instance_limits *out_limits) {
    const turbowasm_instance_table *table;

    if (instance == NULL || out_limits == NULL ||
        table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve_const(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;

    out_limits->minimum = table->size;
    out_limits->maximum = table->maximum;
    out_limits->has_maximum = table->has_maximum;
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
    uint32_t index;

    if (instance == NULL || table_index >= instance->table_count)
        return TURBOWASM_INVALID_ARGUMENT;

    table = turbowasm_instance_table_resolve(
        instance, table_index);
    if (table == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (!turbowasm_range_fits(destination, length, table->size))
        return TURBOWASM_TRAPPED;

    {
        turbowasm_status status =
            turbowasm_value_to_table_entry(
                instance, table->reference_type,
                value, &entry);
        if (status != TURBOWASM_OK)
            return status;
    }

    for (index = 0u; index < length; ++index)
        table->entries[destination + index] = entry;
    return TURBOWASM_OK;
}
