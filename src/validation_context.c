#include "validation_context.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool turbowasm_validation_reserve(
    void **storage,
    uint32_t *capacity,
    uint32_t required,
    size_t element_size) {
    uint32_t next;
    void *grown;

    if (storage == NULL || capacity == NULL || element_size == 0u)
        return false;
    if (required <= *capacity)
        return true;

    next = *capacity == 0u ? 4u : *capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * (uint64_t)element_size > (uint64_t)SIZE_MAX)
        return false;

    grown = realloc(*storage, (size_t)next * element_size);
    if (grown == NULL)
        return false;

    if (next > *capacity) {
        memset((unsigned char *)grown +
                   (size_t)(*capacity) * element_size,
               0,
               (size_t)(next - *capacity) * element_size);
    }

    *storage = grown;
    *capacity = next;
    return true;
}

void turbowasm_validation_context_destroy(
    turbowasm_validation_context *context) {
    uint32_t index;

    if (context == NULL)
        return;

    for (index = 0u; index < context->type_count; ++index) {
        free(context->types[index].params);
        free(context->types[index].results);
    }

    free(context->types);
    free(context->functions);
    free(context->globals);
    free(context->tables);
    free(context->memories);
    memset(context, 0, sizeof(*context));
}

bool turbowasm_validation_context_allocate_types(
    turbowasm_validation_context *context,
    uint32_t count) {
    if (context == NULL ||
        context->types != NULL ||
        context->type_count != 0u)
        return false;

    if (count == 0u)
        return true;

    if ((uint64_t)count * sizeof(*context->types) >
        (uint64_t)SIZE_MAX)
        return false;

    context->types = (turbowasm_validation_func_type *)calloc(
        (size_t)count, sizeof(*context->types));
    if (context->types == NULL)
        return false;

    context->type_count = count;
    return true;
}

bool turbowasm_validation_context_define_type(
    turbowasm_validation_context *context,
    uint32_t index,
    uint32_t param_count,
    uint32_t result_count) {
    turbowasm_validation_func_type *type;

    if (context == NULL || index >= context->type_count)
        return false;

    type = &context->types[index];
    if (type->defined)
        return false;

    if (param_count != 0u) {
        type->params = (uint8_t *)calloc((size_t)param_count, 1u);
        if (type->params == NULL)
            return false;
    }

    if (result_count != 0u) {
        type->results = (uint8_t *)calloc((size_t)result_count, 1u);
        if (type->results == NULL) {
            free(type->params);
            type->params = NULL;
            return false;
        }
    }

    type->param_count = param_count;
    type->result_count = result_count;
    type->defined = true;
    return true;
}

turbowasm_validation_func_type *
turbowasm_validation_context_type_mut(
    turbowasm_validation_context *context,
    uint32_t index) {
    if (context == NULL || index >= context->type_count)
        return NULL;
    return &context->types[index];
}

const turbowasm_validation_func_type *
turbowasm_validation_context_type(
    const turbowasm_validation_context *context,
    uint32_t index) {
    if (context == NULL || index >= context->type_count)
        return NULL;
    return &context->types[index];
}

bool turbowasm_validation_context_append_function(
    turbowasm_validation_context *context,
    uint32_t type_index,
    bool imported) {
    uint32_t required;

    if (context == NULL ||
        type_index >= context->type_count ||
        context->function_count == UINT32_MAX)
        return false;

    required = context->function_count + 1u;
    if (!turbowasm_validation_reserve(
            (void **)&context->functions,
            &context->function_capacity,
            required,
            sizeof(*context->functions)))
        return false;

    context->functions[context->function_count].type_index = type_index;
    context->functions[context->function_count].imported = imported;
    ++context->function_count;
    return true;
}

bool turbowasm_validation_context_append_global(
    turbowasm_validation_context *context,
    uint8_t value_type,
    bool mutable_value,
    bool imported) {
    uint32_t required;

    if (context == NULL || context->global_count == UINT32_MAX)
        return false;

    required = context->global_count + 1u;
    if (!turbowasm_validation_reserve(
            (void **)&context->globals,
            &context->global_capacity,
            required,
            sizeof(*context->globals)))
        return false;

    context->globals[context->global_count].value_type = value_type;
    context->globals[context->global_count].mutable_value = mutable_value;
    context->globals[context->global_count].imported = imported;
    ++context->global_count;
    return true;
}

bool turbowasm_validation_context_append_table(
    turbowasm_validation_context *context,
    uint8_t reference_type,
    bool imported) {
    uint32_t required;

    if (context == NULL || context->table_count == UINT32_MAX)
        return false;

    required = context->table_count + 1u;
    if (!turbowasm_validation_reserve(
            (void **)&context->tables,
            &context->table_capacity,
            required,
            sizeof(*context->tables)))
        return false;

    context->tables[context->table_count].reference_type = reference_type;
    context->tables[context->table_count].imported = imported;
    ++context->table_count;
    return true;
}

bool turbowasm_validation_context_append_memory(
    turbowasm_validation_context *context,
    bool imported) {
    uint32_t required;

    if (context == NULL || context->memory_count == UINT32_MAX)
        return false;

    required = context->memory_count + 1u;
    if (!turbowasm_validation_reserve(
            (void **)&context->memories,
            &context->memory_capacity,
            required,
            sizeof(*context->memories)))
        return false;

    context->memories[context->memory_count].imported = imported;
    ++context->memory_count;
    return true;
}

const turbowasm_validation_func_type *
turbowasm_validation_context_function_type(
    const turbowasm_validation_context *context,
    uint32_t function_index) {
    uint32_t type_index;

    if (context == NULL || function_index >= context->function_count)
        return NULL;

    type_index = context->functions[function_index].type_index;
    return turbowasm_validation_context_type(context, type_index);
}

const turbowasm_validation_global *
turbowasm_validation_context_global(
    const turbowasm_validation_context *context,
    uint32_t global_index) {
    if (context == NULL || global_index >= context->global_count)
        return NULL;
    return &context->globals[global_index];
}
