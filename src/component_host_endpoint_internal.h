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
    /* Result staging borrows the canonical reader until the entire tree commits.
     * The enclosing result precharges this body and retains its source tree. */
    struct component_host_endpoint_impl *stage_next;
    turbowasm_component_endpoint_value_owner *stage_record;
} component_host_endpoint_impl;

/* Transfer ownership changes the cancellation root without duplicating it. */
void turbowasm_component_host_endpoint_register(component_host_endpoint_impl *impl);
bool turbowasm_component_host_endpoint_destroy_ready(const turbowasm_component_host_endpoint *owner);
turbowasm_status turbowasm_component_host_endpoint_destroy_locked(turbowasm_component_host_endpoint *owner);
bool turbowasm_component_host_endpoint_move_ready(const turbowasm_component_host_endpoint *owner,
    const turbowasm_component_value *value, const bool *admitted);
/* Whole-tree preflight has authenticated the prepared owner; admission flag is
 * now true. This exclusive transition has no remaining fallible operation. */
void turbowasm_component_host_endpoint_move_publish(turbowasm_component_host_endpoint *owner);

#endif
