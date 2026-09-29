#include "component_resource_binding.h"

#include <string.h>

static turbowasm_status resource_type_info(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id resource_type,
    uint64_t *out_identity,
    turbowasm_value_kind *out_rep_kind) {
    const turbowasm_component_type *type;

    if (graph == NULL || out_identity == NULL || out_rep_kind == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_component_type_graph_get(graph, resource_type);
    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        type->as.resource.identity == 0u)
        return TURBOWASM_TYPE_MISMATCH;

    if (type->as.resource.rep_type == 0x7fu) {
        *out_rep_kind = TURBOWASM_VALUE_I32;
    } else if (type->as.resource.rep_type == 0x7eu) {
        *out_rep_kind = TURBOWASM_VALUE_I64;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    *out_identity = type->as.resource.identity;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_resource_binding_init(
    turbowasm_component_resource_binding *binding,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id resource_type,
    turbowasm_component_resource_table *table,
    turbowasm_component_resource_destructor_fn destructor,
    void *destructor_context) {
    turbowasm_status status;

    if (binding == NULL || graph == NULL || table == NULL ||
        binding->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_type_graph_validate(graph) ||
        table->max_entries == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(binding, 0, sizeof(*binding));
    status = resource_type_info(
        graph, resource_type,
        &binding->resource_identity,
        &binding->rep_kind);
    if (status != TURBOWASM_OK)
        return status;

    binding->graph = graph;
    binding->resource_type = resource_type;
    binding->table = table;
    binding->destructor = destructor;
    binding->destructor_context = destructor_context;
    binding->initialized = true;
    return TURBOWASM_OK;
}

void turbowasm_component_resource_binding_destroy(
    turbowasm_component_resource_binding *binding) {
    if (binding == NULL)
        return;
    memset(binding, 0, sizeof(*binding));
}

turbowasm_status turbowasm_component_resource_binding_new(
    const turbowasm_component_resource_binding *binding,
    turbowasm_value rep,
    turbowasm_component_resource_handle *out_handle) {
    if (binding == NULL || !binding->initialized ||
        out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (rep.kind != binding->rep_kind)
        return TURBOWASM_TYPE_MISMATCH;

    return turbowasm_component_resource_new_owned(
        binding->table,
        binding->resource_identity,
        rep,
        out_handle);
}

turbowasm_status turbowasm_component_resource_binding_rep(
    const turbowasm_component_resource_binding *binding,
    turbowasm_component_resource_handle handle,
    turbowasm_value *out_rep) {
    turbowasm_status status;

    if (binding == NULL || !binding->initialized ||
        out_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        binding->table, handle,
        binding->resource_identity, out_rep);
    if (status != TURBOWASM_OK)
        return status;
    return out_rep->kind == binding->rep_kind
        ? TURBOWASM_OK
        : TURBOWASM_TRAPPED;
}

turbowasm_status turbowasm_component_resource_binding_drop(
    const turbowasm_component_resource_binding *binding,
    turbowasm_component_resource_handle handle) {
    if (binding == NULL || !binding->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_component_resource_drop(
        binding->table,
        handle,
        binding->resource_identity,
        binding->destructor,
        binding->destructor_context);
}
