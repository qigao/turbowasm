#ifndef TURBOWASM_COMPONENT_TASK_BUILTIN_H
#define TURBOWASM_COMPONENT_TASK_BUILTIN_H

#include "component_binary.h"
#include "component_task.h"

/* Private Core host binding for task/subtask/context/backpressure/yield/waitable-set
 * builtins. Bind rejects other async families until their ownership adapters
 * are connected. Stable binding, domain, graph and resolved memory/codec context
 * outlive every linked instance and suspended callback. No guest code runs here. */
typedef struct turbowasm_component_task_builtin {
    turbowasm_component_task_domain *domain;
    const turbowasm_component_type_graph *graph;
    turbowasm_component_async_builtin definition;
    turbowasm_component_canonical_memory memory;
    turbowasm_host_function_type type;
    turbowasm_value_kind params[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS];
    turbowasm_value_kind results[TURBOWASM_COMPONENT_MAX_FLAT_RESULTS];
} turbowasm_component_task_builtin;

turbowasm_status turbowasm_component_task_builtin_bind(turbowasm_component_task_builtin *binding,
    turbowasm_component_task_domain *domain, const turbowasm_component_type_graph *graph,
    const turbowasm_component_async_builtin *definition,
    const turbowasm_component_canonical_memory *resolved_memory);

/* Ordinary Runtime host ABI; result outputs are published only on success. WAIT and
 * YIELD suspend through Core host-wait. Task resume supplies readiness, while
 * destruction unwinds the same frame and releases its set/subtask pin once. */
turbowasm_status turbowasm_component_task_builtin_invoke(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t result_capacity, size_t *result_count, turbowasm_trap *trap);

#endif
