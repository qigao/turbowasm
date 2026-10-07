#ifndef TURBOWASM_COMPONENT_ENDPOINT_BUILTIN_H
#define TURBOWASM_COMPONENT_ENDPOINT_BUILTIN_H
#include "component_task_builtin.h"

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
