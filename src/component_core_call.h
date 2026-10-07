#ifndef TURBOWASM_COMPONENT_CORE_CALL_H
#define TURBOWASM_COMPONENT_CORE_CALL_H

#include "component_canonical.h"
#include "component_resource.h"

#include <turbowasm/execution.h>
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
/* A result hook takes responsibility for releasing a lifted own on both success
 * and failure. The admission hook runs only after all lowering has succeeded. */
typedef turbowasm_status (*turbowasm_component_result_owner_fn)(
    void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type, turbowasm_component_value *value);
typedef void (*turbowasm_component_admission_commit_fn)(void *context);

typedef struct turbowasm_component_core_call_adapter {
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id function_type;
    turbowasm_instance *instance;
    uint32_t function_index;

    turbowasm_instance *post_return_instance;
    uint32_t post_return_function_index;
    /* Shared with the owning exec's canon-lower boundary. */
    bool *may_leave;

    turbowasm_component_canonical_memory memory;
    turbowasm_component_flat_signature flat_signature;
    turbowasm_component_resource_table *resources;

    turbowasm_component_result_owner_fn result_owner;
    void *result_owner_context;
    turbowasm_component_admission_commit_fn admission_commit;
    void *admission_context;

    bool uses_memory;
    bool uses_resources;
    /* The exec owning this graph implements its non-alias resource definitions.
     * Standalone adapters treat supplied resources as foreign to their callee. */
    bool defines_local_resources;
    bool initialized;
} turbowasm_component_core_call_adapter;

turbowasm_status turbowasm_component_core_call_adapter_init(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_component_canonical_memory *memory);

turbowasm_status turbowasm_component_core_call_adapter_init_with_resources(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_component_resource_table *resources);

void turbowasm_component_core_call_adapter_destroy(
    turbowasm_component_core_call_adapter *adapter);

turbowasm_status turbowasm_component_core_call_set_post_return(
    turbowasm_component_core_call_adapter *adapter,
    turbowasm_instance *instance, uint32_t function_index);

turbowasm_status turbowasm_component_core_call_invoke(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

typedef struct turbowasm_component_core_execution {
    void *impl;
} turbowasm_component_core_execution;

turbowasm_status turbowasm_component_core_execution_create(
    turbowasm_component_core_execution *execution,
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count);

void turbowasm_component_core_execution_destroy(
    turbowasm_component_core_execution *execution);

turbowasm_status turbowasm_component_core_execution_resume(
    turbowasm_component_core_execution *execution,
    const turbowasm_execution_options *options);

turbowasm_execution_state turbowasm_component_core_execution_state_get(
    const turbowasm_component_core_execution *execution);

turbowasm_yield_reason turbowasm_component_core_execution_yield_reason_get(
    const turbowasm_component_core_execution *execution);

bool turbowasm_component_core_execution_pending_host_wait(
    const turbowasm_component_core_execution *execution,
    turbowasm_host_wait *out_wait);

turbowasm_status turbowasm_component_core_execution_complete_host_wait(
    turbowasm_component_core_execution *execution,
    turbowasm_host_wait wait,
    int status);

turbowasm_status turbowasm_component_core_execution_terminal_status(
    const turbowasm_component_core_execution *execution);

turbowasm_trap turbowasm_component_core_execution_trap(
    const turbowasm_component_core_execution *execution);

size_t turbowasm_component_core_execution_result_count(
    const turbowasm_component_core_execution *execution);

turbowasm_status turbowasm_component_core_execution_take_result(
    turbowasm_component_core_execution *execution,
    turbowasm_component_value *out_result);

#endif /* TURBOWASM_COMPONENT_CORE_CALL_H */
