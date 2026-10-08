#ifndef TURBOWASM_COMPONENT_HOST_ENDPOINT_INTERNAL_H
#define TURBOWASM_COMPONENT_HOST_ENDPOINT_INTERNAL_H

#include "component_api_internal.h"

/* Shared only by the host endpoint/transfer owners in the Component module.
 * A driving body cannot be moved, inspected or destroyed through its carrier. */
typedef struct component_host_endpoint_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_budget *budget;
    turbowasm_component_endpoint *endpoint;
    turbowasm_component_host_registration registration;
    bool driving;
    bool deferred_move, move_cleanup_activity;
} component_host_endpoint_impl;

/* Transfer ownership changes the cancellation root without duplicating it. */
void turbowasm_component_host_endpoint_register(component_host_endpoint_impl *impl);

#endif
