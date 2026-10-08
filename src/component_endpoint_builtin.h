#ifndef TURBOWASM_COMPONENT_ENDPOINT_BUILTIN_H
#define TURBOWASM_COMPONENT_ENDPOINT_BUILTIN_H
#include "component_task_builtin.h"

struct turbowasm_component_endpoint;
/* Domain-owned stable pair storage, shared by guest new and host admission.
 * Empty, distinct output cells are written only on success. Guest mode registers
 * both ends in the domain table; host mode registers neither. The immutable
 * graph and domain outlive both ends, including moves to other tables. Closed
 * pairs may be collected on valid admission; live pairs share table max_entries
 * as their storage quota even when they occupy no handles. Owner-thread only. */
turbowasm_status turbowasm_component_endpoint_domain_pair_open(
    turbowasm_component_task_domain *domain, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type, bool guest_handles,
    struct turbowasm_component_endpoint **reader, struct turbowasm_component_endpoint **writer);
/* True only for a still-retained end in this module's stable pair storage. */
bool turbowasm_component_endpoint_domain_pair_retained(const struct turbowasm_component_endpoint *endpoint);

bool turbowasm_component_endpoint_builtin_kind(turbowasm_component_async_builtin_kind kind, bool *future);
turbowasm_status turbowasm_component_endpoint_builtin_invoke(turbowasm_component_task_builtin *binding,
    turbowasm_host_call *call, const turbowasm_value *arguments, turbowasm_value *result);
/* Reclaim only fully closed pairs. Live/transferred endpoints retain their owner. */
void turbowasm_component_endpoint_domain_collect(turbowasm_component_task_domain *domain);
/* Readiness includes conversion failures. A conversion in progress is not ready
 * and prevents task destruction from unwinding an endpoint wait underneath it. */
turbowasm_status turbowasm_component_endpoint_builtin_ready(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle handle, bool *ready, bool *can_unwind);
#endif
