#include "component_canonical.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool pointer_type_valid(
    turbowasm_component_pointer_type pointer_type) {
    return pointer_type == TURBOWASM_COMPONENT_POINTER_I32 ||
           pointer_type == TURBOWASM_COMPONENT_POINTER_I64;
}

static uint64_t pointer_size(
    turbowasm_component_pointer_type pointer_type) {
    return pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? UINT64_C(8)
        : UINT64_C(4);
}

static turbowasm_component_flat_type pointer_flat_type(
    turbowasm_component_pointer_type pointer_type) {
    return pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? TURBOWASM_COMPONENT_FLAT_I64
        : TURBOWASM_COMPONENT_FLAT_I32;
}

static turbowasm_status resolved_kind(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind *out_kind,
    const turbowasm_component_type **out_type) {
    const turbowasm_component_type *type = NULL;

    if (graph == NULL || out_kind == NULL || out_type == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        switch (ref.as.inline_type) {
            case TURBOWASM_COMPONENT_TYPE_BOOL:
            case TURBOWASM_COMPONENT_TYPE_S8:
            case TURBOWASM_COMPONENT_TYPE_U8:
            case TURBOWASM_COMPONENT_TYPE_S16:
            case TURBOWASM_COMPONENT_TYPE_U16:
            case TURBOWASM_COMPONENT_TYPE_S32:
            case TURBOWASM_COMPONENT_TYPE_U32:
            case TURBOWASM_COMPONENT_TYPE_S64:
            case TURBOWASM_COMPONENT_TYPE_U64:
            case TURBOWASM_COMPONENT_TYPE_F32:
            case TURBOWASM_COMPONENT_TYPE_F64:
            case TURBOWASM_COMPONENT_TYPE_CHAR:
            case TURBOWASM_COMPONENT_TYPE_STRING:
                *out_kind = ref.as.inline_type;
                *out_type = NULL;
                return TURBOWASM_OK;
            default:
                return TURBOWASM_UNSUPPORTED;
        }
    }

    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (type == NULL ||
        type->kind == TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        return TURBOWASM_MALFORMED_MODULE;

    *out_kind = type->kind;
    *out_type = type;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_canonical_layout(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_layout *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    uint64_t ptr;
    turbowasm_status status;

    if (out == NULL || !pointer_type_valid(pointer_type))
        return TURBOWASM_INVALID_ARGUMENT;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    ptr = pointer_size(pointer_type);
    memset(out, 0, sizeof(*out));

    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
        case TURBOWASM_COMPONENT_TYPE_S8:
        case TURBOWASM_COMPONENT_TYPE_U8:
            out->alignment = 1u;
            out->size = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_S16:
        case TURBOWASM_COMPONENT_TYPE_U16:
            out->alignment = 2u;
            out->size = 2u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_S32:
        case TURBOWASM_COMPONENT_TYPE_U32:
        case TURBOWASM_COMPONENT_TYPE_F32:
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            out->alignment = 4u;
            out->size = 4u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_S64:
        case TURBOWASM_COMPONENT_TYPE_U64:
        case TURBOWASM_COMPONENT_TYPE_F64:
            out->alignment = 8u;
            out->size = 8u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_STRING:
        case TURBOWASM_COMPONENT_TYPE_LIST:
            out->alignment = ptr;
            out->size = 2u * ptr;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_OWN:
        case TURBOWASM_COMPONENT_TYPE_BORROW:
            out->alignment = 4u;
            out->size = 4u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_RESOURCE:
        case TURBOWASM_COMPONENT_TYPE_FUNCTION:
            (void)type;
            return TURBOWASM_UNSUPPORTED;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_component_canonical_flatten_type(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_flat_type_list *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    turbowasm_status status;

    if (out == NULL || !pointer_type_valid(pointer_type))
        return TURBOWASM_INVALID_ARGUMENT;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    memset(out, 0, sizeof(*out));

    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
        case TURBOWASM_COMPONENT_TYPE_S8:
        case TURBOWASM_COMPONENT_TYPE_U8:
        case TURBOWASM_COMPONENT_TYPE_S16:
        case TURBOWASM_COMPONENT_TYPE_U16:
        case TURBOWASM_COMPONENT_TYPE_S32:
        case TURBOWASM_COMPONENT_TYPE_U32:
        case TURBOWASM_COMPONENT_TYPE_CHAR:
        case TURBOWASM_COMPONENT_TYPE_OWN:
        case TURBOWASM_COMPONENT_TYPE_BORROW:
            out->types[0] = TURBOWASM_COMPONENT_FLAT_I32;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_S64:
        case TURBOWASM_COMPONENT_TYPE_U64:
            out->types[0] = TURBOWASM_COMPONENT_FLAT_I64;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_F32:
            out->types[0] = TURBOWASM_COMPONENT_FLAT_F32;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_F64:
            out->types[0] = TURBOWASM_COMPONENT_FLAT_F64;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_STRING:
        case TURBOWASM_COMPONENT_TYPE_LIST:
            out->types[0] = pointer_flat_type(pointer_type);
            out->types[1] = pointer_flat_type(pointer_type);
            out->count = 2u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_RESOURCE:
        case TURBOWASM_COMPONENT_TYPE_FUNCTION:
            (void)type;
            return TURBOWASM_UNSUPPORTED;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status append_flat_type(
    turbowasm_component_flat_type *out,
    uint32_t capacity,
    uint32_t *count,
    const turbowasm_component_flat_type_list *flat) {
    uint32_t i;

    if (out == NULL || count == NULL || flat == NULL ||
        *count > capacity)
        return TURBOWASM_INVALID_ARGUMENT;
    if (flat->count > capacity - *count)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < flat->count; ++i)
        out[(*count)++] = flat->types[i];
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_canonical_flatten_function(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_canonical_context context,
    turbowasm_component_flat_signature *out) {
    const turbowasm_component_type *function;
    uint64_t raw_param_count = 0u;
    uint32_t i;
    turbowasm_component_flat_type_list flat;
    turbowasm_status status;

    if (graph == NULL || out == NULL ||
        !pointer_type_valid(pointer_type) ||
        (context != TURBOWASM_COMPONENT_CANONICAL_LIFT &&
         context != TURBOWASM_COMPONENT_CANONICAL_LOWER))
        return TURBOWASM_INVALID_ARGUMENT;

    function = turbowasm_component_type_graph_get(
        graph, function_type);
    if (function == NULL ||
        function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    for (i = 0u; i < function->as.function.param_count; ++i) {
        status = turbowasm_component_canonical_flatten_type(
            graph,
            function->as.function.params[i],
            pointer_type,
            &flat);
        if (status != TURBOWASM_OK)
            return status;
        raw_param_count += flat.count;
        if (raw_param_count > UINT32_MAX)
            return TURBOWASM_UNSUPPORTED;
    }

    if (raw_param_count >
        TURBOWASM_COMPONENT_MAX_FLAT_PARAMS) {
        out->params[0] = pointer_flat_type(pointer_type);
        out->param_count = 1u;
        out->params_indirect = true;
    } else {
        for (i = 0u;
             i < function->as.function.param_count;
             ++i) {
            status = turbowasm_component_canonical_flatten_type(
                graph,
                function->as.function.params[i],
                pointer_type,
                &flat);
            if (status != TURBOWASM_OK)
                return status;
            status = append_flat_type(
                out->params,
                TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS,
                &out->param_count,
                &flat);
            if (status != TURBOWASM_OK)
                return status;
        }
    }

    if (!function->as.function.has_result)
        return TURBOWASM_OK;

    status = turbowasm_component_canonical_flatten_type(
        graph,
        function->as.function.result,
        pointer_type,
        &flat);
    if (status != TURBOWASM_OK)
        return status;

    if (flat.count <= TURBOWASM_COMPONENT_MAX_FLAT_RESULTS) {
        out->results[0] = flat.types[0];
        out->result_count = flat.count;
        return TURBOWASM_OK;
    }

    out->results_indirect = true;
    if (context == TURBOWASM_COMPONENT_CANONICAL_LIFT) {
        out->results[0] = pointer_flat_type(pointer_type);
        out->result_count = 1u;
    } else {
        if (out->param_count >=
            TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS)
            return TURBOWASM_UNSUPPORTED;
        out->params[out->param_count++] =
            pointer_flat_type(pointer_type);
        out->result_count = 0u;
    }

    return TURBOWASM_OK;
}
