#ifndef TURBOWASM_COMPONENT_HOST_ENDPOINT_INTERNAL_H
#define TURBOWASM_COMPONENT_HOST_ENDPOINT_INTERNAL_H

#include "component_api_internal.h"

/* Shared only by the host endpoint/transfer owners in the Component module.
 * A driving body cannot be moved, inspected or destroyed through its carrier. */
typedef struct component_host_endpoint_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_budget *budget;
    turbowasm_component_endpoint *endpoint;
    bool driving;
} component_host_endpoint_impl;

#endif
