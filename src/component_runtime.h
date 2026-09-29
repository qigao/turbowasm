#ifndef TURBOWASM_COMPONENT_RUNTIME_H
#define TURBOWASM_COMPONENT_RUNTIME_H

#include "component_binary.h"
#include "component_core_call.h"

#include <turbowasm/module.h>

typedef struct turbowasm_component_core_function_ref {
    turbowasm_instance *instance;
    uint32_t function_index;
} turbowasm_component_core_function_ref;

typedef struct turbowasm_component_runtime {
    const turbowasm_component_binary *binary;

    turbowasm_module *core_modules;
    uint32_t core_module_count;

    turbowasm_instance *core_instances;
    uint32_t core_instance_count;

    turbowasm_component_core_function_ref *core_functions;
    uint32_t core_function_count;

    turbowasm_component_core_call_adapter *functions;
    uint32_t function_count;

    bool initialized;
} turbowasm_component_runtime;

turbowasm_status turbowasm_component_runtime_init(
    turbowasm_component_runtime *runtime,
    const turbowasm_component_binary *binary);

void turbowasm_component_runtime_destroy(
    turbowasm_component_runtime *runtime);

turbowasm_status turbowasm_component_runtime_invoke_export(
    turbowasm_component_runtime *runtime,
    turbowasm_component_name export_name,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

#endif /* TURBOWASM_COMPONENT_RUNTIME_H */
