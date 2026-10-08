#include "component_type_view.h"
#include "runtime_alloc.h"
#include <string.h>

static turbowasm_status count_resources(const turbowasm_component_type_graph *graph,
    uint32_t depth, uint32_t *count) {
    uint32_t i;
    if (depth >= TURBOWASM_COMPONENT_VALUE_MAX_DEPTH) return TURBOWASM_UNSUPPORTED;
    for (i = 0u; i < graph->count; ++i) {
        const turbowasm_component_type *type = &graph->types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE) {
            if (*count == UINT32_MAX) return TURBOWASM_OUT_OF_MEMORY;
            ++*count;
        } else if (type->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE && type->as.instance != NULL) {
            turbowasm_status status = count_resources(&type->as.instance->type_graph, depth + 1u, count);
            if (status != TURBOWASM_OK) return status;
        }
    }
    return TURBOWASM_OK;
}

static void destroy_graph(turbowasm_component_type_graph *graph) {
    uint32_t i;
    for (i = 0u; i < graph->count; ++i) {
        turbowasm_component_type *type = &graph->types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE && type->as.instance != NULL) {
            destroy_graph(&type->as.instance->type_graph);
            turbowasm_rt_free(type->as.instance);
        }
    }
    turbowasm_rt_free(graph->types);
    memset(graph, 0, sizeof(*graph));
}

static const void *instance_identity(turbowasm_component_type_view *view, uint64_t declaration) {
    uint32_t i;
    for (i = 0u; i < view->identity_count; ++i)
        if (view->identities[i].declaration == declaration) return view->identities[i].runtime;
    /* The exact-size token array never grows. Pointer identity represents the
     * runtime resource type, matching its generative instance lifetime. */
    view->identities[i].declaration = declaration;
    view->identities[i].runtime = &view->identities[i];
    ++view->identity_count;
    return view->identities[i].runtime;
}

static turbowasm_status copy_graph(turbowasm_component_type_view *view,
    const turbowasm_component_type_graph *source, turbowasm_component_type_graph *destination) {
    uint32_t i;
    if (source->count == 0u) return TURBOWASM_OK;
    if ((size_t)source->count > SIZE_MAX / sizeof(*destination->types)) return TURBOWASM_OUT_OF_MEMORY;
    destination->types = turbowasm_rt_calloc(source->count, sizeof(*destination->types));
    if (destination->types == NULL) return TURBOWASM_OUT_OF_MEMORY;
    destination->count = source->count;
    for (i = 0u; i < source->count; ++i) {
        const turbowasm_component_type *type = &source->types[i];
        turbowasm_component_type *copy = &destination->types[i];
        *copy = *type;
        if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE)
            copy->as.resource.instance_key = instance_identity(view, type->as.resource.identity);
        else if (type->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE && type->as.instance != NULL) {
            turbowasm_status status;
            copy->as.instance = turbowasm_rt_calloc(1u, sizeof(*copy->as.instance));
            if (copy->as.instance == NULL) return TURBOWASM_OUT_OF_MEMORY;
            copy->as.instance->exports = type->as.instance->exports;
            copy->as.instance->export_count = type->as.instance->export_count;
            status = copy_graph(view, &type->as.instance->type_graph, &copy->as.instance->type_graph);
            if (status != TURBOWASM_OK) return status;
        }
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_type_view_create(
    const turbowasm_component_binary *source, turbowasm_component_type_view **out) {
    turbowasm_component_type_view *view;
    turbowasm_status status;
    uint32_t resources = 0u;
    if (source == NULL || out == NULL || *out != NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = count_resources(&source->type_graph, 0u, &resources);
    if (status != TURBOWASM_OK || resources == 0u) return status;
    if ((size_t)resources > SIZE_MAX / sizeof(*view->identities)) return TURBOWASM_OUT_OF_MEMORY;
    view = turbowasm_rt_calloc(1u, sizeof(*view));
    if (view == NULL) return TURBOWASM_OUT_OF_MEMORY;
    view->binary = *source;
    memset(&view->binary.type_graph, 0, sizeof(view->binary.type_graph));
    view->identities = turbowasm_rt_calloc(resources, sizeof(*view->identities));
    if (view->identities == NULL) status = TURBOWASM_OUT_OF_MEMORY;
    else status = copy_graph(view, &source->type_graph, &view->binary.type_graph);
    if (status != TURBOWASM_OK) { turbowasm_component_type_view_destroy(view); return status; }
    *out = view;
    return TURBOWASM_OK;
}

void turbowasm_component_type_view_destroy(turbowasm_component_type_view *view) {
    if (view == NULL) return;
    destroy_graph(&view->binary.type_graph);
    turbowasm_rt_free(view->identities);
    turbowasm_rt_free(view);
}
