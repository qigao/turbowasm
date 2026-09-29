#ifndef TURBOWASM_COMPONENT_EXEC_H
#define TURBOWASM_COMPONENT_EXEC_H

#include "component_binary.h"
#include "component_core_call.h"
#include "component_resource_binding.h"

#include <turbowasm/instance.h>
#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_component_exec_core_function_kind {
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID = 0,
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE,
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN
} turbowasm_component_exec_core_function_kind;

typedef struct turbowasm_component_exec_core_function {
    turbowasm_component_exec_core_function_kind kind;
    uint32_t instance_index;
    uint32_t function_index;
    uint32_t resource_builtin_index;
} turbowasm_component_exec_core_function;

typedef struct turbowasm_component_exec_core_memory {
    uint32_t instance_index;
    uint32_t memory_index;
} turbowasm_component_exec_core_memory;

typedef struct turbowasm_component_exec_realloc_context {
    turbowasm_instance *instance;
    uint32_t function_index;
    turbowasm_component_pointer_type pointer_type;
} turbowasm_component_exec_realloc_context;

typedef struct turbowasm_component_exec_resource_context {
    struct turbowasm_component_exec *exec;
    uint32_t resource_type;
} turbowasm_component_exec_resource_context;

typedef struct turbowasm_component_exec_resource_builtin_context {
    turbowasm_component_resource_binding *binding;
    turbowasm_component_resource_builtin_kind kind;
} turbowasm_component_exec_resource_builtin_context;

typedef struct turbowasm_component_exec {
    const turbowasm_component_binary *binary;

    turbowasm_module *core_modules;
    uint32_t core_module_count;

    turbowasm_instance *core_instances;
    uint32_t core_instance_count;

    turbowasm_component_exec_core_function *core_functions;
    uint32_t core_function_count;

    turbowasm_component_exec_core_memory *core_memories;
    uint32_t core_memory_count;

    turbowasm_component_exec_realloc_context *realloc_contexts;

    turbowasm_component_resource_table resource_table;
    turbowasm_component_resource_binding *resource_bindings;
    turbowasm_component_exec_resource_context *resource_contexts;
    uint32_t resource_binding_count;

    turbowasm_component_exec_resource_builtin_context
        *resource_builtin_contexts;

    turbowasm_component_core_call_adapter *functions;
    uint32_t function_count;

    bool initialized;
} turbowasm_component_exec;

/*
 * Instantiate the C5c1 executable subset:
 * - no Component imports;
 * - Core instance definitions are zero-argument module instantiations;
 * - Core function definitions come from core-export aliases;
 * - Component functions come from synchronous canon lift with no options.
 *
 * The executable borrows the decoded Component binary and its source bytes.
 */
turbowasm_status turbowasm_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary);

void turbowasm_component_exec_destroy(
    turbowasm_component_exec *exec);

turbowasm_status turbowasm_component_exec_invoke_export(
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

#endif /* TURBOWASM_COMPONENT_EXEC_H */
