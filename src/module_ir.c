#include "module_ir.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool turbowasm_ir_reserve(void **storage,
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

    if ((size_t)next > SIZE_MAX / element_size)
        return false;

    grown = realloc(*storage, (size_t)next * element_size);
    if (grown == NULL)
        return false;

    if (next > *capacity) {
        memset((unsigned char *)grown + (size_t)(*capacity) * element_size,
               0,
               (size_t)(next - *capacity) * element_size);
    }

    *storage = grown;
    *capacity = next;
    return true;
}

void turbowasm_module_ir_destroy(turbowasm_module_ir *ir) {
    uint32_t index;

    if (ir == NULL) return;

    for (index = 0u; index < ir->type_count; ++index) {
        free(ir->types[index].params);
        free(ir->types[index].results);
    }

    free(ir->types);
    free(ir->functions);
    free(ir->tables);
    free(ir->memories);
    free(ir->globals);
    memset(ir, 0, sizeof(*ir));
}

bool turbowasm_module_ir_allocate_types(turbowasm_module_ir *ir,
                                        uint32_t count) {
    if (ir == NULL || ir->types != NULL || ir->type_count != 0u)
        return false;
    if (count == 0u)
        return true;
    if ((size_t)count > SIZE_MAX / sizeof(*ir->types))
        return false;

    ir->types = (turbowasm_ir_func_type *)calloc(
        (size_t)count, sizeof(*ir->types));
    if (ir->types == NULL)
        return false;
    ir->type_count = count;
    return true;
}

bool turbowasm_module_ir_append_function(turbowasm_module_ir *ir,
                                         turbowasm_ir_function value) {
    uint32_t required;
    if (ir == NULL || ir->function_count == UINT32_MAX)
        return false;
    required = ir->function_count + 1u;
    if (!turbowasm_ir_reserve((void **)&ir->functions,
                              &ir->function_capacity,
                              required,
                              sizeof(*ir->functions)))
        return false;
    ir->functions[ir->function_count++] = value;
    return true;
}

bool turbowasm_module_ir_append_table(turbowasm_module_ir *ir,
                                      turbowasm_ir_table value) {
    uint32_t required;
    if (ir == NULL || ir->table_count == UINT32_MAX)
        return false;
    required = ir->table_count + 1u;
    if (!turbowasm_ir_reserve((void **)&ir->tables,
                              &ir->table_capacity,
                              required,
                              sizeof(*ir->tables)))
        return false;
    ir->tables[ir->table_count++] = value;
    return true;
}

bool turbowasm_module_ir_append_memory(turbowasm_module_ir *ir,
                                       turbowasm_ir_memory value) {
    uint32_t required;
    if (ir == NULL || ir->memory_count == UINT32_MAX)
        return false;
    required = ir->memory_count + 1u;
    if (!turbowasm_ir_reserve((void **)&ir->memories,
                              &ir->memory_capacity,
                              required,
                              sizeof(*ir->memories)))
        return false;
    ir->memories[ir->memory_count++] = value;
    return true;
}

bool turbowasm_module_ir_append_global(turbowasm_module_ir *ir,
                                       turbowasm_ir_global value) {
    uint32_t required;
    if (ir == NULL || ir->global_count == UINT32_MAX)
        return false;
    required = ir->global_count + 1u;
    if (!turbowasm_ir_reserve((void **)&ir->globals,
                              &ir->global_capacity,
                              required,
                              sizeof(*ir->globals)))
        return false;
    ir->globals[ir->global_count++] = value;
    return true;
}
