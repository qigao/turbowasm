#ifndef TURBOWASM_COMPONENT_RESOURCE_BINDING_H
#define TURBOWASM_COMPONENT_RESOURCE_BINDING_H

#include "component_resource.h"
#include "component_type_graph.h"

typedef struct turbowasm_component_resource_binding {
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id resource_type;
    turbowasm_component_resource_table *table;
    turbowasm_component_resource_destructor_fn destructor;
    void *destructor_context;

    uint64_t resource_identity;
    turbowasm_value_kind rep_kind;
    bool initialized;
} turbowasm_component_resource_binding;

turbowasm_status turbowasm_component_resource_binding_init(
    turbowasm_component_resource_binding *binding,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id resource_type,
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_destructor_fn destructor,
    void *destructor_context);

void turbowasm_component_resource_binding_destroy(
    turbowasm_component_resource_binding *binding);

turbowasm_status turbowasm_component_resource_binding_new(
    const turbowasm_component_resource_binding *binding,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle);

turbowasm_status turbowasm_component_resource_binding_rep(
    const turbowasm_component_resource_binding *binding,
    turbowasm_component_resource_handle handle,
    turbowasm_value *out_rep);

turbowasm_status turbowasm_component_resource_binding_drop(
    const turbowasm_component_resource_binding *binding,
    turbowasm_component_resource_handle handle);

#endif /* TURBOWASM_COMPONENT_RESOURCE_BINDING_H */
