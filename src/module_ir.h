#ifndef TURBOWASM_MODULE_IR_H
#define TURBOWASM_MODULE_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_ir_limits {
    uint32_t minimum;
    uint32_t maximum;
    bool has_maximum;
} turbowasm_ir_limits;

typedef struct turbowasm_ir_func_type {
    uint8_t *params;
    uint32_t param_count;
    uint8_t *results;
    uint32_t result_count;
} turbowasm_ir_func_type;

typedef struct turbowasm_ir_function {
    uint32_t type_index;
    bool imported;
} turbowasm_ir_function;

typedef struct turbowasm_ir_table {
    uint8_t reference_type;
    turbowasm_ir_limits limits;
    bool imported;
} turbowasm_ir_table;

typedef struct turbowasm_ir_memory {
    turbowasm_ir_limits limits;
    bool imported;
} turbowasm_ir_memory;

typedef struct turbowasm_ir_global {
    uint8_t value_type;
    bool mutable_value;
    bool imported;
} turbowasm_ir_global;

typedef struct turbowasm_module_ir {
    turbowasm_ir_func_type *types;
    uint32_t type_count;

    turbowasm_ir_function *functions;
    uint32_t function_count;
    uint32_t function_capacity;

    turbowasm_ir_table *tables;
    uint32_t table_count;
    uint32_t table_capacity;

    turbowasm_ir_memory *memories;
    uint32_t memory_count;
    uint32_t memory_capacity;

    turbowasm_ir_global *globals;
    uint32_t global_count;
    uint32_t global_capacity;
} turbowasm_module_ir;

void turbowasm_module_ir_destroy(turbowasm_module_ir *ir);

bool turbowasm_module_ir_allocate_types(turbowasm_module_ir *ir,
                                        uint32_t count);

bool turbowasm_module_ir_append_function(turbowasm_module_ir *ir,
                                         turbowasm_ir_function value);
bool turbowasm_module_ir_append_table(turbowasm_module_ir *ir,
                                      turbowasm_ir_table value);
bool turbowasm_module_ir_append_memory(turbowasm_module_ir *ir,
                                       turbowasm_ir_memory value);
bool turbowasm_module_ir_append_global(turbowasm_module_ir *ir,
                                       turbowasm_ir_global value);

#endif /* TURBOWASM_MODULE_IR_H */
