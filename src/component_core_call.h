#ifndef TURBOWASM_COMPONENT_CORE_CALL_H
#define TURBOWASM_COMPONENT_CORE_CALL_H

#include "component_canonical.h"

#include <turbowasm/instance.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Private synchronous canon-lift adapter.
 *
 * The adapter borrows the Component type graph, Core instance and canonical
 * memory options. It never owns or bypasses the Core Runtime instance.
 */
typedef struct turbowasm_component_core_call_adapter {
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id function_type;
    turbowasm_instance *instance;
    uint32_t function_index;

    turbowasm_component_canonical_memory memory;
    turbowasm_component_flat_signature flat_signature;

    bool uses_memory;
    bool initialized;
} turbowasm_component_core_call_adapter;

turbowasm_status turbowasm_component_core_call_adapter_init(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_component_canonical_memory *memory);

void turbowasm_component_core_call_adapter_destroy(
    turbowasm_component_core_call_adapter *adapter);

turbowasm_status turbowasm_component_core_call_invoke(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

#endif /* TURBOWASM_COMPONENT_CORE_CALL_H */
