#include "component_type_graph.h"

#include "runtime_alloc.h"

#include <stddef.h>
#include <string.h>

static bool scalar_kind(turbowasm_component_type_kind kind) {
    return kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
           kind <= TURBOWASM_COMPONENT_TYPE_CHAR;
}

static bool inline_kind(turbowasm_component_type_kind kind) {
    return scalar_kind(kind) ||
           kind == TURBOWASM_COMPONENT_TYPE_STRING;
}

turbowasm_component_type_ref turbowasm_component_type_ref_indexed(
    turbowasm_component_type_id id) {
    turbowasm_component_type_ref ref;
    memset(&ref, 0, sizeof(ref));
    ref.kind = TURBOWASM_COMPONENT_TYPE_REF_INDEXED;
    ref.as.indexed = id;
    return ref;
}

turbowasm_component_type_ref turbowasm_component_type_ref_inline(
    turbowasm_component_type_kind kind) {
    turbowasm_component_type_ref ref;
    memset(&ref, 0, sizeof(ref));
    ref.kind = TURBOWASM_COMPONENT_TYPE_REF_INLINE;
    ref.as.inline_type = kind;
    return ref;
}

bool turbowasm_component_type_ref_validate(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    if (graph == NULL)
        return false;
    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        return inline_kind(ref.as.inline_type);
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED ||
        ref.as.indexed >= graph->count ||
        graph->types == NULL)
        return false;
    return graph->types[ref.as.indexed].kind !=
           TURBOWASM_COMPONENT_TYPE_UNDEFINED;
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
    uint32_t index;

    if (graph == NULL)
        return;

    if (graph->types != NULL) {
        for (index = 0u; index < graph->count; ++index) {
            if (graph->types[index].kind ==
                    TURBOWASM_COMPONENT_TYPE_FUNCTION) {
                turbowasm_rt_free(
                    graph->types[index].as.function.params);
                graph->types[index].as.function.params = NULL;
            } else if (graph->types[index].kind ==
                           TURBOWASM_COMPONENT_TYPE_RECORD) {
                turbowasm_rt_free(
                    graph->types[index].as.record.fields);
                graph->types[index].as.record.fields = NULL;
            } else if (graph->types[index].kind ==
                           TURBOWASM_COMPONENT_TYPE_TUPLE) {
                turbowasm_rt_free(
                    graph->types[index].as.tuple.elements);
                graph->types[index].as.tuple.elements = NULL;
            } else if (graph->types[index].kind ==
                           TURBOWASM_COMPONENT_TYPE_INSTANCE &&
                       graph->types[index].as.instance != NULL) {
                turbowasm_component_instance_type *instance_type =
                    graph->types[index].as.instance;
                turbowasm_component_type_graph_destroy(
                    &instance_type->type_graph);
                turbowasm_rt_free(instance_type->exports);
                turbowasm_rt_free(instance_type);
                graph->types[index].as.instance = NULL;
            }
        }
    }

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

bool turbowasm_component_type_graph_define_list_ref(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_ref element_type) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        return false;

    if (element_type.kind ==
            TURBOWASM_COMPONENT_TYPE_REF_INDEXED) {
        if (element_type.as.indexed >= graph->count)
            return false;
    } else if (element_type.kind ==
                   TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        if (!inline_kind(element_type.as.inline_type))
            return false;
    } else {
        return false;
    }

    type->kind = TURBOWASM_COMPONENT_TYPE_LIST;
    type->as.list.element_type = element_type;
    return true;
}

bool turbowasm_component_type_graph_define_list(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_id element_type) {
    return turbowasm_component_type_graph_define_list_ref(
        graph, id,
        turbowasm_component_type_ref_indexed(element_type));
}

static bool retained_ref_in_range(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    if (graph == NULL)
        return false;
    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        return inline_kind(ref.as.inline_type);
    return ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED &&
           ref.as.indexed < graph->count;
}

bool turbowasm_component_type_graph_define_record(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_record_field *fields,
    uint32_t field_count) {
    turbowasm_component_type *type = slot(graph, id);
    turbowasm_component_record_field *copy = NULL;
    uint32_t i;

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        field_count == 0u || fields == NULL)
        return false;

    for (i = 0u; i < field_count; ++i) {
        if ((fields[i].name_size != 0u && fields[i].name == NULL) ||
            !retained_ref_in_range(graph, fields[i].type))
            return false;
    }
    if ((size_t)field_count > SIZE_MAX / sizeof(*copy))
        return false;
    copy = (turbowasm_component_record_field *)turbowasm_rt_malloc(
        (size_t)field_count * sizeof(*copy));
    if (copy == NULL)
        return false;
    memcpy(copy, fields, (size_t)field_count * sizeof(*copy));

    type->kind = TURBOWASM_COMPONENT_TYPE_RECORD;
    type->as.record.fields = copy;
    type->as.record.count = field_count;
    return true;
}

bool turbowasm_component_type_graph_define_tuple(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_type_ref *elements,
    uint32_t element_count) {
    turbowasm_component_type *type = slot(graph, id);
    turbowasm_component_type_ref *copy = NULL;
    uint32_t i;

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        element_count == 0u || elements == NULL)
        return false;

    for (i = 0u; i < element_count; ++i) {
        if (!retained_ref_in_range(graph, elements[i]))
            return false;
    }
    if ((size_t)element_count > SIZE_MAX / sizeof(*copy))
        return false;
    copy = (turbowasm_component_type_ref *)turbowasm_rt_malloc(
        (size_t)element_count * sizeof(*copy));
    if (copy == NULL)
        return false;
    memcpy(copy, elements, (size_t)element_count * sizeof(*copy));

    type->kind = TURBOWASM_COMPONENT_TYPE_TUPLE;
    type->as.tuple.elements = copy;
    type->as.tuple.count = element_count;
    return true;
}

bool turbowasm_component_type_graph_define_option(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_ref payload) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        !retained_ref_in_range(graph, payload))
        return false;
    type->kind = TURBOWASM_COMPONENT_TYPE_OPTION;
    type->as.option.payload = payload;
    return true;
}

bool turbowasm_component_type_graph_define_result(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    bool has_ok,
    turbowasm_component_type_ref ok,
    bool has_error,
    turbowasm_component_type_ref error) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        (has_ok && !retained_ref_in_range(graph, ok)) ||
        (has_error && !retained_ref_in_range(graph, error)))
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_RESULT;
    type->as.result.has_ok = has_ok;
    type->as.result.ok = ok;
    type->as.result.has_error = has_error;
    type->as.result.error = error;
    return true;
}

bool turbowasm_component_type_graph_define_function(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_type_ref *params,
    uint32_t param_count,
    bool has_result,
    turbowasm_component_type_ref result) {
    turbowasm_component_type *type = slot(graph, id);
    turbowasm_component_type_ref *copy = NULL;

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        (param_count != 0u && params == NULL))
        return false;

    if (param_count != 0u) {
        if ((size_t)param_count >
            SIZE_MAX / sizeof(*copy))
            return false;
        copy = (turbowasm_component_type_ref *)turbowasm_rt_malloc(
            (size_t)param_count * sizeof(*copy));
        if (copy == NULL)
            return false;
        memcpy(copy, params,
               (size_t)param_count * sizeof(*copy));
    }

    type->kind = TURBOWASM_COMPONENT_TYPE_FUNCTION;
    type->as.function.params = copy;
    type->as.function.param_count = param_count;
    type->as.function.has_result = has_result;
    type->as.function.result = result;
    return true;
}

bool turbowasm_component_type_graph_define_resource_full(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity,
    uint8_t rep_type,
    bool has_destructor,
    uint32_t destructor_index) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        nominal_identity == 0u)
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_RESOURCE;
    type->as.resource.identity = nominal_identity;
    type->as.resource.rep_type = rep_type;
    type->as.resource.has_destructor = has_destructor;
    type->as.resource.identity_alias = false;
    type->as.resource.destructor_index = destructor_index;
    return true;
}

bool turbowasm_component_type_graph_define_resource_alias(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity,
    uint8_t rep_type,
    bool has_destructor,
    uint32_t destructor_index) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        nominal_identity == 0u)
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_RESOURCE;
    type->as.resource.identity = nominal_identity;
    type->as.resource.rep_type = rep_type;
    type->as.resource.has_destructor = has_destructor;
    type->as.resource.identity_alias = true;
    type->as.resource.destructor_index = destructor_index;
    return true;
}

bool turbowasm_component_type_graph_define_resource(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity) {
    return turbowasm_component_type_graph_define_resource_full(
        graph, id, nominal_identity, 0u, false, UINT32_MAX);
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

bool turbowasm_component_type_graph_define_instance(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_instance_type *instance_type) {
    turbowasm_component_type *type = slot(graph, id);

    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED ||
        instance_type == NULL ||
        !turbowasm_component_type_graph_validate(
            &instance_type->type_graph))
        return false;

    type->kind = TURBOWASM_COMPONENT_TYPE_INSTANCE;
    type->as.instance = instance_type;
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

static bool type_ref_contains_borrow(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t depth) {
    const turbowasm_component_type *type;

    if (graph == NULL || depth > graph->count)
        return true;
    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        return false;
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED ||
        ref.as.indexed >= graph->count)
        return true;

    type = &graph->types[ref.as.indexed];
    if (type->kind == TURBOWASM_COMPONENT_TYPE_BORROW)
        return true;
    if (type->kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return type_ref_contains_borrow(
            graph, type->as.list.element_type, depth + 1u);
    if (type->kind == TURBOWASM_COMPONENT_TYPE_RECORD) {
        uint32_t i;
        for (i = 0u; i < type->as.record.count; ++i) {
            if (type_ref_contains_borrow(
                    graph,
                    type->as.record.fields[i].type,
                    depth + 1u))
                return true;
        }
        return false;
    }
    if (type->kind == TURBOWASM_COMPONENT_TYPE_TUPLE) {
        uint32_t i;
        for (i = 0u; i < type->as.tuple.count; ++i) {
            if (type_ref_contains_borrow(
                    graph,
                    type->as.tuple.elements[i],
                    depth + 1u))
                return true;
        }
        return false;
    }
    if (type->kind == TURBOWASM_COMPONENT_TYPE_OPTION)
        return type_ref_contains_borrow(
            graph, type->as.option.payload, depth + 1u);
    if (type->kind == TURBOWASM_COMPONENT_TYPE_RESULT)
        return (type->as.result.has_ok &&
                type_ref_contains_borrow(
                    graph, type->as.result.ok, depth + 1u)) ||
               (type->as.result.has_error &&
                type_ref_contains_borrow(
                    graph, type->as.result.error, depth + 1u));
    return false;
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
                if (!turbowasm_component_type_ref_validate(
                        graph, type->as.list.element_type))
                    return false;
                break;

            case TURBOWASM_COMPONENT_TYPE_RECORD: {
                uint32_t field_index;
                if (type->as.record.count == 0u ||
                    type->as.record.fields == NULL)
                    return false;
                for (field_index = 0u;
                     field_index < type->as.record.count;
                     ++field_index) {
                    uint32_t previous;
                    const turbowasm_component_record_field *field =
                        &type->as.record.fields[field_index];
                    if ((field->name_size != 0u && field->name == NULL) ||
                        !turbowasm_component_type_ref_validate(
                            graph, field->type))
                        return false;
                    for (previous = 0u;
                         previous < field_index;
                         ++previous) {
                        const turbowasm_component_record_field *other =
                            &type->as.record.fields[previous];
                        if (other->name_size == field->name_size &&
                            (field->name_size == 0u ||
                             memcmp(
                                 other->name,
                                 field->name,
                                 field->name_size) == 0))
                            return false;
                    }
                }
                break;
            }

            case TURBOWASM_COMPONENT_TYPE_TUPLE: {
                uint32_t element_index;
                if (type->as.tuple.count == 0u ||
                    type->as.tuple.elements == NULL)
                    return false;
                for (element_index = 0u;
                     element_index < type->as.tuple.count;
                     ++element_index) {
                    if (!turbowasm_component_type_ref_validate(
                            graph,
                            type->as.tuple.elements[element_index]))
                        return false;
                }
                break;
            }

            case TURBOWASM_COMPONENT_TYPE_OPTION:
                if (!turbowasm_component_type_ref_validate(
                        graph, type->as.option.payload))
                    return false;
                break;

            case TURBOWASM_COMPONENT_TYPE_RESULT:
                if ((type->as.result.has_ok &&
                     !turbowasm_component_type_ref_validate(
                         graph, type->as.result.ok)) ||
                    (type->as.result.has_error &&
                     !turbowasm_component_type_ref_validate(
                         graph, type->as.result.error)))
                    return false;
                break;

            case TURBOWASM_COMPONENT_TYPE_FUNCTION: {
                uint32_t param_index;
                for (param_index = 0u;
                     param_index < type->as.function.param_count;
                     ++param_index) {
                    if (!turbowasm_component_type_ref_validate(
                            graph,
                            type->as.function.params[param_index]))
                        return false;
                }
                if (type->as.function.has_result) {
                    if (!turbowasm_component_type_ref_validate(
                            graph, type->as.function.result) ||
                        type_ref_contains_borrow(
                            graph, type->as.function.result, 0u))
                        return false;
                }
                break;
            }

            case TURBOWASM_COMPONENT_TYPE_RESOURCE:
                if (type->as.resource.identity == 0u)
                    return false;
                if (!type->as.resource.identity_alias) {
                    for (j = 0u; j < i; ++j) {
                        if (graph->types[j].kind ==
                                TURBOWASM_COMPONENT_TYPE_RESOURCE &&
                            !graph->types[j].as.resource.identity_alias &&
                            graph->types[j].as.resource.identity ==
                                type->as.resource.identity)
                            return false;
                    }
                }
                break;

            case TURBOWASM_COMPONENT_TYPE_OWN:
            case TURBOWASM_COMPONENT_TYPE_BORROW:
                if (type->as.handle.resource_type >= graph->count ||
                    graph->types[type->as.handle.resource_type].kind !=
                        TURBOWASM_COMPONENT_TYPE_RESOURCE)
                    return false;
                break;

            case TURBOWASM_COMPONENT_TYPE_INSTANCE: {
                const turbowasm_component_instance_type *instance_type =
                    type->as.instance;
                uint32_t export_index;

                if (instance_type == NULL ||
                    !turbowasm_component_type_graph_validate(
                        &instance_type->type_graph))
                    return false;
                for (export_index = 0u;
                     export_index < instance_type->export_count;
                     ++export_index) {
                    const turbowasm_component_instance_type_export *export_desc =
                        &instance_type->exports[export_index];
                    const turbowasm_component_type *export_type;

                    if (export_desc->type_index >=
                            instance_type->type_graph.count ||
                        (export_desc->name_size != 0u &&
                         export_desc->name == NULL))
                        return false;
                    export_type = turbowasm_component_type_graph_get(
                        &instance_type->type_graph,
                        export_desc->type_index);
                    if (export_type == NULL)
                        return false;
                    if (export_desc->kind ==
                            TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION) {
                        if (export_type->kind !=
                            TURBOWASM_COMPONENT_TYPE_FUNCTION)
                            return false;
                    } else if (export_desc->kind ==
                                   TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE) {
                        if (export_type->kind ==
                            TURBOWASM_COMPONENT_TYPE_UNDEFINED)
                            return false;
                    } else {
                        return false;
                    }
                }
                break;
            }

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
