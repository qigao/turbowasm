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
typedef struct turbowasm_component_core_call_adapter {
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id function_type;
    turbowasm_instance *instance;
    uint32_t function_index;

    turbowasm_component_canonical_memory memory;
    turbowasm_component_flat_signature flat_signature;
    turbowasm_component_resource_table *resources;

    turbowasm_component_resource_lower_fn external_resource_lower;
    turbowasm_component_resource_lift_fn external_resource_lift;
    void *external_resource_context;

    bool uses_memory;
    bool uses_resources;
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

void turbowasm_component_core_call_adapter_set_external_resources(
    turbowasm_component_core_call_adapter *adapter,
    turbowasm_component_resource_lower_fn lower,
    turbowasm_component_resource_lift_fn lift,
    void *context);

void turbowasm_component_core_call_adapter_destroy(
    turbowasm_component_core_call_adapter *adapter);

turbowasm_status turbowasm_component_core_call_invoke(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

/*
 * Private restartable form of one lifted Component export call.
 *
 * Component arguments are canonically lowered exactly once at create time.
 * The retained call-local resource/borrow scope remains live across Runtime
 * yields until the execution terminates or is destroyed.
 */
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

turbowasm_execution_state
turbowasm_component_core_execution_state_get(
    const turbowasm_component_core_execution *execution);

turbowasm_yield_reason
turbowasm_component_core_execution_yield_reason_get(
    const turbowasm_component_core_execution *execution);

bool turbowasm_component_core_execution_pending_host_wait(
    const turbowasm_component_core_execution *execution,
    turbowasm_host_wait *out_wait);

turbowasm_status
turbowasm_component_core_execution_complete_host_wait(
    turbowasm_component_core_execution *execution,
    turbowasm_host_wait wait,
    int status);

turbowasm_status turbowasm_component_core_execution_terminal_status(
    const turbowasm_component_core_execution *execution);

turbowasm_trap turbowasm_component_core_execution_trap(
    const turbowasm_component_core_execution *execution);

size_t turbowasm_component_core_execution_result_count(
    const turbowasm_component_core_execution *execution);

/*
 * Transfer the retained lifted result to the caller exactly once.
 * Returns INVALID_ARGUMENT when the call has no result or the result has
 * already been taken.
 */
turbowasm_status turbowasm_component_core_execution_take_result(
    turbowasm_component_core_execution *execution,
    turbowasm_component_value *out_result);

#endif /* TURBOWASM_COMPONENT_CORE_CALL_H */
