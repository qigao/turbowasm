#include "component_type_graph.h"

#include "runtime_alloc.h"

#include <stddef.h>
#include <string.h>

static bool scalar_kind(turbowasm_component_type_kind kind) {
    return kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
           kind <= TURBOWASM_COMPONENT_TYPE_CHAR;
}

static turbowasm_component_type *slot(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id) {
    if (graph == NULL || graph->types == NULL || id >= graph->count)
        return NULL;
    return &graph->types[id];
}

bool turbowasm_component_type_graph_allocate(
    turbowasm_component_type_graph *graph,
    uint32_t count) {
    if (graph == NULL || graph->types != NULL || graph->count != 0u)
        return false;

    if (count == 0u)
        return true;
    if ((size_t)count > SIZE_MAX / sizeof(*graph->types))
        return false;

    graph->types = (turbowasm_component_type *)turbowasm_rt_calloc(
        (size_t)count, sizeof(*graph->types));
    if (graph->types == NULL)
        return false;

    graph->count = count;
    return true;
}

void turbowasm_component_type_graph_destroy(
    turbowasm_component_type_graph *graph) {
    if (graph == NULL)
        return;
    turbowasm_rt_free(graph->types);
    memset(graph, 0, sizeof(*graph));
}

bool turbowasm_component_type_graph_define_scalar(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_kind kind) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        !scalar_kind(kind))
        return false;

    type->kind = kind;
    return true;
}

bool turbowasm_component_type_graph_define_string(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        return false;
    type->kind = TURBOWASM_COMPONENT_TYPE_STRING;
    return true;
}

bool turbowasm_component_type_graph_define_list(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_id element_type) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        element_type >= graph->count)
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_LIST;
    type->as.list.element_type = element_type;
    return true;
}

bool turbowasm_component_type_graph_define_resource(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        nominal_identity == 0u)
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_RESOURCE;
    type->as.resource.identity = nominal_identity;
    return true;
}

bool turbowasm_component_type_graph_define_handle(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_kind kind,
    turbowasm_component_type_id resource_type) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        (kind != TURBOWASM_COMPONENT_TYPE_OWN &&
         kind != TURBOWASM_COMPONENT_TYPE_BORROW) ||
        resource_type >= graph->count)
        return false;

    type->kind = kind;
    type->as.handle.resource_type = resource_type;
    return true;
}

const turbowasm_component_type *
turbowasm_component_type_graph_get(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id) {
    if (graph == NULL || graph->types == NULL || id >= graph->count)
        return NULL;
    return &graph->types[id];
}

bool turbowasm_component_type_graph_validate(
    const turbowasm_component_type_graph *graph) {
    uint32_t i;
    uint32_t j;

    if (graph == NULL || (graph->count != 0u && graph->types == NULL))
        return false;

    for (i = 0u; i < graph->count; ++i) {
        const turbowasm_component_type *type = &graph->types[i];

        if (type->kind == TURBOWASM_COMPONENT_TYPE_UNDEFINED)
            return false;

        switch (type->kind) {
            case TURBOWASM_COMPONENT_TYPE_LIST:
                if (type->as.list.element_type >= graph->count)
                    return false;
                break;

            case TURBOWASM_COMPONENT_TYPE_RESOURCE:
                if (type->as.resource.identity == 0u)
                    return false;
                for (j = 0u; j < i; ++j) {
                    if (graph->types[j].kind ==
                            TURBOWASM_COMPONENT_TYPE_RESOURCE &&
                        graph->types[j].as.resource.identity ==
                            type->as.resource.identity)
                        return false;
                }
                break;

            case TURBOWASM_COMPONENT_TYPE_OWN:
            case TURBOWASM_COMPONENT_TYPE_BORROW:
                if (type->as.handle.resource_type >= graph->count ||
                    graph->types[type->as.handle.resource_type].kind !=
                        TURBOWASM_COMPONENT_TYPE_RESOURCE)
                    return false;
                break;

            default:
                if (!scalar_kind(type->kind) &&
                    type->kind != TURBOWASM_COMPONENT_TYPE_STRING)
                    return false;
                break;
        }
    }

    return true;
}

const cmeta_type_desc *turbowasm_component_scalar_cmeta_type(
    turbowasm_component_type_kind kind) {
    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL: return &cmeta_type_bool;
        case TURBOWASM_COMPONENT_TYPE_S8: return &cmeta_type_int8;
        case TURBOWASM_COMPONENT_TYPE_U8: return &cmeta_type_uint8;
        case TURBOWASM_COMPONENT_TYPE_S16: return &cmeta_type_int16;
        case TURBOWASM_COMPONENT_TYPE_U16: return &cmeta_type_uint16;
        case TURBOWASM_COMPONENT_TYPE_S32: return &cmeta_type_int32;
        case TURBOWASM_COMPONENT_TYPE_U32: return &cmeta_type_uint32;
        case TURBOWASM_COMPONENT_TYPE_S64: return &cmeta_type_int64;
        case TURBOWASM_COMPONENT_TYPE_U64: return &cmeta_type_uint64;
        case TURBOWASM_COMPONENT_TYPE_F32: return &cmeta_type_float;
        case TURBOWASM_COMPONENT_TYPE_F64: return &cmeta_type_double;
        case TURBOWASM_COMPONENT_TYPE_CHAR: return &cmeta_type_uint32;
        default: return NULL;
    }
}
