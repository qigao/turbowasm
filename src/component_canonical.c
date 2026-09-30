#include "component_canonical.h"

#include "instance_internal.h"
#include "module_internal.h"
#include "runtime_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
    TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH = 64u
};

#define TURBOWASM_COMPONENT_MAX_STRING_BYTE_LENGTH \
    (UINT64_C(1) << 28u) - UINT64_C(1)
#define TURBOWASM_COMPONENT_MAX_LIST_BYTE_LENGTH \
    (UINT64_C(1) << 28u) - UINT64_C(1)

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

static bool align_up_u64(
    uint64_t value,
    uint64_t alignment,
    uint64_t *out) {
    uint64_t remainder;
    uint64_t delta;

    if (out == NULL || alignment == 0u)
        return false;
    remainder = value % alignment;
    if (remainder == 0u) {
        *out = value;
        return true;
    }
    delta = alignment - remainder;
    if (value > UINT64_MAX - delta)
        return false;
    *out = value + delta;
    return true;
}

static size_t enum_storage_width(uint32_t count) {
    if (count == 0u)
        return 0u;
    if (count <= 256u)
        return 1u;
    if (count <= 65536u)
        return 2u;
    return 4u;
}

static size_t flags_storage_width(uint32_t count) {
    if (count == 0u || count > 32u)
        return 0u;
    if (count <= 8u)
        return 1u;
    if (count <= 16u)
        return 2u;
    return 4u;
}

static uint32_t flags_valid_mask(uint32_t count) {
    if (count >= 32u)
        return UINT32_MAX;
    return count == 0u
        ? 0u
        : ((UINT32_C(1) << count) - UINT32_C(1));
}


static turbowasm_status canonical_layout_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_pointer_type pointer_type,
    uint32_t depth,
    turbowasm_component_layout *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    uint64_t ptr;
    turbowasm_status status;

    if (out == NULL || !pointer_type_valid(pointer_type))
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_UNSUPPORTED;

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
            out->alignment = ptr;
            out->size = 2u * ptr;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_LIST:
            if (type == NULL ||
                !turbowasm_component_type_ref_validate(
                    graph, type->as.list.element_type))
                return TURBOWASM_MALFORMED_MODULE;
            out->alignment = ptr;
            out->size = 2u * ptr;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_RECORD:
        case TURBOWASM_COMPONENT_TYPE_TUPLE: {
            uint32_t count;
            uint32_t i;
            uint64_t alignment = 1u;
            uint64_t offset = 0u;

            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            count = kind == TURBOWASM_COMPONENT_TYPE_RECORD
                ? type->as.record.count
                : type->as.tuple.count;
            if (count == 0u)
                return TURBOWASM_MALFORMED_MODULE;

            for (i = 0u; i < count; ++i) {
                turbowasm_component_type_ref child =
                    kind == TURBOWASM_COMPONENT_TYPE_RECORD
                        ? type->as.record.fields[i].type
                        : type->as.tuple.elements[i];
                turbowasm_component_layout child_layout;
                uint64_t aligned;

                status = canonical_layout_inner(
                    graph, child, pointer_type,
                    depth + 1u, &child_layout);
                if (status != TURBOWASM_OK)
                    return status;
                if (child_layout.alignment > alignment)
                    alignment = child_layout.alignment;
                if (!align_up_u64(
                        offset, child_layout.alignment, &aligned) ||
                    aligned > UINT64_MAX - child_layout.size)
                    return TURBOWASM_UNSUPPORTED;
                offset = aligned + child_layout.size;
            }

            if (!align_up_u64(offset, alignment, &out->size))
                return TURBOWASM_UNSUPPORTED;
            out->alignment = alignment;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_OPTION:
        case TURBOWASM_COMPONENT_TYPE_RESULT: {
            turbowasm_component_type_ref cases[2];
            bool present[2] = {false, false};
            uint64_t payload_alignment = 1u;
            uint64_t payload_size = 0u;
            uint64_t payload_offset;
            uint32_t i;

            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            if (kind == TURBOWASM_COMPONENT_TYPE_OPTION) {
                present[1] = true;
                cases[1] = type->as.option.payload;
            } else {
                present[0] = type->as.result.has_ok;
                cases[0] = type->as.result.ok;
                present[1] = type->as.result.has_error;
                cases[1] = type->as.result.error;
            }

            for (i = 0u; i < 2u; ++i) {
                turbowasm_component_layout case_layout;
                if (!present[i])
                    continue;
                status = canonical_layout_inner(
                    graph, cases[i], pointer_type,
                    depth + 1u, &case_layout);
                if (status != TURBOWASM_OK)
                    return status;
                if (case_layout.alignment > payload_alignment)
                    payload_alignment = case_layout.alignment;
                if (case_layout.size > payload_size)
                    payload_size = case_layout.size;
            }

            out->alignment = payload_alignment;
            if (!align_up_u64(
                    UINT64_C(1), payload_alignment,
                    &payload_offset) ||
                payload_offset > UINT64_MAX - payload_size ||
                !align_up_u64(
                    payload_offset + payload_size,
                    out->alignment,
                    &out->size))
                return TURBOWASM_UNSUPPORTED;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_ENUM: {
            size_t width;
            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            width = enum_storage_width(type->as.enumeration.count);
            if (width == 0u)
                return TURBOWASM_MALFORMED_MODULE;
            out->alignment = (uint64_t)width;
            out->size = (uint64_t)width;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_FLAGS: {
            size_t width;
            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            width = flags_storage_width(type->as.flags.count);
            if (width == 0u)
                return TURBOWASM_MALFORMED_MODULE;
            out->alignment = (uint64_t)width;
            out->size = (uint64_t)width;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_OWN:
        case TURBOWASM_COMPONENT_TYPE_BORROW: {
            const turbowasm_component_type *resource;
            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            resource = turbowasm_component_type_graph_get(
                graph, type->as.handle.resource_type);
            if (resource == NULL ||
                resource->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
                return TURBOWASM_MALFORMED_MODULE;
            out->alignment = 4u;
            out->size = 4u;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_RESOURCE:
        case TURBOWASM_COMPONENT_TYPE_FUNCTION:
        case TURBOWASM_COMPONENT_TYPE_INSTANCE:
            (void)type;
            return TURBOWASM_UNSUPPORTED;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_component_canonical_layout(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_layout *out) {
    return canonical_layout_inner(
        graph, ref, pointer_type, 0u, out);
}

static bool append_flat_capped(
    turbowasm_component_flat_type_list *out,
    const turbowasm_component_flat_type_list *part) {
    uint32_t capacity = TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS;
    uint32_t i;

    if (out == NULL || part == NULL ||
        out->count > capacity || part->count > capacity)
        return false;

    for (i = 0u; i < part->count && out->count < capacity; ++i)
        out->types[out->count++] = part->types[i];
    return true;
}

static turbowasm_component_flat_type join_flat_type(
    turbowasm_component_flat_type left,
    turbowasm_component_flat_type right) {
    if (left == right)
        return left;
    if ((left == TURBOWASM_COMPONENT_FLAT_I32 &&
         right == TURBOWASM_COMPONENT_FLAT_F32) ||
        (left == TURBOWASM_COMPONENT_FLAT_F32 &&
         right == TURBOWASM_COMPONENT_FLAT_I32))
        return TURBOWASM_COMPONENT_FLAT_I32;
    return TURBOWASM_COMPONENT_FLAT_I64;
}

static turbowasm_status canonical_flatten_type_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_pointer_type pointer_type,
    uint32_t depth,
    turbowasm_component_flat_type_list *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    turbowasm_status status;

    if (out == NULL || !pointer_type_valid(pointer_type))
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_UNSUPPORTED;

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
            out->types[0] = TURBOWASM_COMPONENT_FLAT_I32;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_OWN:
        case TURBOWASM_COMPONENT_TYPE_BORROW: {
            const turbowasm_component_type *resource;
            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            resource = turbowasm_component_type_graph_get(
                graph, type->as.handle.resource_type);
            if (resource == NULL ||
                resource->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
                return TURBOWASM_MALFORMED_MODULE;
            out->types[0] = TURBOWASM_COMPONENT_FLAT_I32;
            out->count = 1u;
            return TURBOWASM_OK;
        }

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
            out->types[0] = pointer_flat_type(pointer_type);
            out->types[1] = pointer_flat_type(pointer_type);
            out->count = 2u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_LIST:
            if (type == NULL ||
                !turbowasm_component_type_ref_validate(
                    graph, type->as.list.element_type))
                return TURBOWASM_MALFORMED_MODULE;
            out->types[0] = pointer_flat_type(pointer_type);
            out->types[1] = pointer_flat_type(pointer_type);
            out->count = 2u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_RECORD:
        case TURBOWASM_COMPONENT_TYPE_TUPLE: {
            uint32_t count;
            uint32_t i;

            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            count = kind == TURBOWASM_COMPONENT_TYPE_RECORD
                ? type->as.record.count
                : type->as.tuple.count;
            if (count == 0u)
                return TURBOWASM_MALFORMED_MODULE;

            for (i = 0u; i < count; ++i) {
                turbowasm_component_type_ref child =
                    kind == TURBOWASM_COMPONENT_TYPE_RECORD
                        ? type->as.record.fields[i].type
                        : type->as.tuple.elements[i];
                turbowasm_component_flat_type_list part;

                status = canonical_flatten_type_inner(
                    graph, child, pointer_type,
                    depth + 1u, &part);
                if (status != TURBOWASM_OK)
                    return status;
                if (!append_flat_capped(out, &part))
                    return TURBOWASM_UNSUPPORTED;
            }
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_OPTION:
        case TURBOWASM_COMPONENT_TYPE_RESULT: {
            turbowasm_component_type_ref cases[2];
            bool present[2] = {false, false};
            turbowasm_component_flat_type_list joined = {{0}, 0u};
            uint32_t i;

            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            if (kind == TURBOWASM_COMPONENT_TYPE_OPTION) {
                present[1] = true;
                cases[1] = type->as.option.payload;
            } else {
                present[0] = type->as.result.has_ok;
                cases[0] = type->as.result.ok;
                present[1] = type->as.result.has_error;
                cases[1] = type->as.result.error;
            }

            for (i = 0u; i < 2u; ++i) {
                turbowasm_component_flat_type_list part;
                uint32_t j;

                if (!present[i])
                    continue;
                status = canonical_flatten_type_inner(
                    graph, cases[i], pointer_type,
                    depth + 1u, &part);
                if (status != TURBOWASM_OK)
                    return status;

                for (j = 0u; j < part.count; ++j) {
                    if (j < joined.count) {
                        joined.types[j] = join_flat_type(
                            joined.types[j], part.types[j]);
                    } else if (joined.count <
                               TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS) {
                        joined.types[joined.count++] = part.types[j];
                    }
                }
            }

            out->types[0] = TURBOWASM_COMPONENT_FLAT_I32;
            out->count = 1u;
            if (!append_flat_capped(out, &joined))
                return TURBOWASM_UNSUPPORTED;
            return TURBOWASM_OK;
        }

        case TURBOWASM_COMPONENT_TYPE_ENUM:
        case TURBOWASM_COMPONENT_TYPE_FLAGS:
            if (type == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            if ((kind == TURBOWASM_COMPONENT_TYPE_ENUM &&
                 type->as.enumeration.count == 0u) ||
                (kind == TURBOWASM_COMPONENT_TYPE_FLAGS &&
                 (type->as.flags.count == 0u ||
                  type->as.flags.count > 32u)))
                return TURBOWASM_MALFORMED_MODULE;
            out->types[0] = TURBOWASM_COMPONENT_FLAT_I32;
            out->count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_TYPE_RESOURCE:
        case TURBOWASM_COMPONENT_TYPE_FUNCTION:
        case TURBOWASM_COMPONENT_TYPE_INSTANCE:
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
    return canonical_flatten_type_inner(
        graph, ref, pointer_type, 0u, out);
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


static bool unicode_scalar_valid(uint32_t value) {
    return value <= UINT32_C(0x10ffff) &&
           !(value >= UINT32_C(0xd800) &&
             value <= UINT32_C(0xdfff));
}

static bool utf8_bytes_valid(const uint8_t *bytes, size_t size) {
    size_t i = 0u;

    if (size != 0u && bytes == NULL)
        return false;

    while (i < size) {
        uint8_t a = bytes[i++];

        if (a < UINT8_C(0x80))
            continue;
        if (a >= UINT8_C(0xc2) && a <= UINT8_C(0xdf)) {
            if (i >= size ||
                (bytes[i] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            ++i;
            continue;
        }
        if (a == UINT8_C(0xe0)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0xa0) ||
                bytes[i] > UINT8_C(0xbf) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 2u;
            continue;
        }
        if ((a >= UINT8_C(0xe1) && a <= UINT8_C(0xec)) ||
            (a >= UINT8_C(0xee) && a <= UINT8_C(0xef))) {
            if (i + 1u >= size ||
                (bytes[i] & UINT8_C(0xc0)) != UINT8_C(0x80) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 2u;
            continue;
        }
        if (a == UINT8_C(0xed)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x9f) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 2u;
            continue;
        }
        if (a == UINT8_C(0xf0)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x90) ||
                bytes[i] > UINT8_C(0xbf) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80) ||
                (bytes[i + 2u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 3u;
            continue;
        }
        if (a >= UINT8_C(0xf1) && a <= UINT8_C(0xf3)) {
            if (i + 2u >= size ||
                (bytes[i] & UINT8_C(0xc0)) != UINT8_C(0x80) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80) ||
                (bytes[i + 2u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 3u;
            continue;
        }
        if (a == UINT8_C(0xf4)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x8f) ||
                (bytes[i + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80) ||
                (bytes[i + 2u] & UINT8_C(0xc0)) != UINT8_C(0x80))
                return false;
            i += 3u;
            continue;
        }
        return false;
    }
    return true;
}

static uint64_t read_le(const uint8_t *bytes, size_t width) {
    uint64_t value = 0u;
    size_t i;
    for (i = 0u; i < width; ++i)
        value |= (uint64_t)bytes[i] << (8u * i);
    return value;
}

static void write_le(uint8_t *bytes, size_t width, uint64_t value) {
    size_t i;
    for (i = 0u; i < width; ++i)
        bytes[i] = (uint8_t)(value >> (8u * i));
}

static turbowasm_status canonical_memory_validate(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl **out_instance) {
    const turbowasm_module *module;
    turbowasm_memory_desc desc;

    if (memory == NULL || memory->instance == NULL ||
        memory->instance->impl == NULL || out_instance == NULL ||
        !pointer_type_valid(memory->pointer_type) ||
        memory->string_encoding != TURBOWASM_COMPONENT_STRING_UTF8)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_instance_module(memory->instance);
    if (module == NULL ||
        !turbowasm_module_memory_at(
            module, memory->memory_index, &desc))
        return TURBOWASM_INVALID_ARGUMENT;

    if ((memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64) !=
        desc.memory64)
        return TURBOWASM_TYPE_MISMATCH;

    *out_instance = (turbowasm_instance_impl *)memory->instance->impl;
    return TURBOWASM_OK;
}

static turbowasm_status read_memory(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    void *out,
    size_t width) {
    turbowasm_status status =
        turbowasm_instance_memory_read_bytes(
            instance, memory_index, address, 0u, out, width);
    return status == TURBOWASM_TRAPPED
        ? TURBOWASM_TRAPPED
        : status;
}

static turbowasm_status write_memory(
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    const void *bytes,
    size_t width) {
    turbowasm_status status =
        turbowasm_instance_memory_write_bytes(
            instance, memory_index, address, 0u, bytes, width);
    return status == TURBOWASM_TRAPPED
        ? TURBOWASM_TRAPPED
        : status;
}

static turbowasm_status read_pointer(
    turbowasm_instance_impl *instance,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    uint64_t *out) {
    uint8_t bytes[8] = {0};
    size_t width = memory->pointer_type ==
            TURBOWASM_COMPONENT_POINTER_I64
        ? 8u : 4u;
    turbowasm_status status;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = read_memory(
        instance, memory->memory_index, address, bytes, width);
    if (status != TURBOWASM_OK)
        return status;
    *out = read_le(bytes, width);
    return TURBOWASM_OK;
}

static turbowasm_status write_pointer(
    turbowasm_instance_impl *instance,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    uint64_t value) {
    uint8_t bytes[8] = {0};
    size_t width = memory->pointer_type ==
            TURBOWASM_COMPONENT_POINTER_I64
        ? 8u : 4u;

    if (width == 4u && value > UINT32_MAX)
        return TURBOWASM_TRAPPED;
    write_le(bytes, width, value);
    return write_memory(
        instance, memory->memory_index, address, bytes, width);
}

static turbowasm_status scalar_width(
    turbowasm_component_type_kind kind,
    size_t *out_width) {
    if (out_width == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
        case TURBOWASM_COMPONENT_TYPE_S8:
        case TURBOWASM_COMPONENT_TYPE_U8:
            *out_width = 1u;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S16:
        case TURBOWASM_COMPONENT_TYPE_U16:
            *out_width = 2u;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S32:
        case TURBOWASM_COMPONENT_TYPE_U32:
        case TURBOWASM_COMPONENT_TYPE_F32:
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            *out_width = 4u;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S64:
        case TURBOWASM_COMPONENT_TYPE_U64:
        case TURBOWASM_COMPONENT_TYPE_F64:
            *out_width = 8u;
            return TURBOWASM_OK;
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status lift_label_value(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    turbowasm_component_value *out) {
    uint8_t bytes[4] = {0};
    size_t width;
    uint32_t bits;
    uint32_t count;
    turbowasm_status status;

    if (type == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        count = type->as.enumeration.count;
        width = enum_storage_width(count);
    } else if (kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        count = type->as.flags.count;
        width = flags_storage_width(count);
    } else {
        return TURBOWASM_INVALID_ARGUMENT;
    }
    if (width == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    status = read_memory(
        instance, memory_index, address, bytes, width);
    if (status != TURBOWASM_OK)
        return status;
    bits = (uint32_t)read_le(bytes, width);

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        if (bits >= count)
            return TURBOWASM_TRAPPED;
        out->as.enum_index = bits;
    } else {
        if ((bits & ~flags_valid_mask(count)) != 0u)
            return TURBOWASM_TRAPPED;
        out->as.flags = bits;
    }
    return TURBOWASM_OK;
}

static turbowasm_status lower_label_value(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    const turbowasm_component_value *value) {
    uint8_t bytes[4] = {0};
    size_t width;
    uint32_t bits;
    uint32_t count;

    if (type == NULL || value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        count = type->as.enumeration.count;
        width = enum_storage_width(count);
        bits = value->as.enum_index;
        if (width == 0u || bits >= count)
            return TURBOWASM_TYPE_MISMATCH;
    } else if (kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        count = type->as.flags.count;
        width = flags_storage_width(count);
        bits = value->as.flags;
        if (width == 0u ||
            (bits & ~flags_valid_mask(count)) != 0u)
            return TURBOWASM_TYPE_MISMATCH;
    } else {
        return TURBOWASM_INVALID_ARGUMENT;
    }

    write_le(bytes, width, bits);
    return write_memory(
        instance, memory_index, address, bytes, width);
}

static turbowasm_status lower_flat_label_value(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_value *value,
    turbowasm_value *out) {
    uint32_t bits;
    uint32_t count;

    if (type == NULL || value == NULL || out == NULL ||
        value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;

    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        count = type->as.enumeration.count;
        bits = value->as.enum_index;
        if (count == 0u || bits >= count)
            return TURBOWASM_TYPE_MISMATCH;
    } else if (kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        count = type->as.flags.count;
        bits = value->as.flags;
        if (count == 0u || count > 32u ||
            (bits & ~flags_valid_mask(count)) != 0u)
            return TURBOWASM_TYPE_MISMATCH;
    } else {
        return TURBOWASM_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_VALUE_I32;
    out->as.i32 = (int32_t)bits;
    return TURBOWASM_OK;
}

static turbowasm_status lift_flat_label_value(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_value *input,
    turbowasm_component_value *out) {
    uint32_t bits;
    uint32_t count;

    if (type == NULL || input == NULL || out == NULL ||
        input->kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;
    bits = (uint32_t)input->as.i32;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        count = type->as.enumeration.count;
        if (count == 0u || bits >= count)
            return TURBOWASM_TRAPPED;
        out->as.enum_index = bits;
    } else if (kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        count = type->as.flags.count;
        if (count == 0u || count > 32u ||
            (bits & ~flags_valid_mask(count)) != 0u)
            return TURBOWASM_TRAPPED;
        out->as.flags = bits;
    } else {
        return TURBOWASM_INVALID_ARGUMENT;
    }
    return TURBOWASM_OK;
}

static turbowasm_status lift_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out);

static turbowasm_status lower_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value);

static void destroy_value_sequence(
    turbowasm_component_value_list *sequence) {
    uint64_t i;

    if (sequence == NULL)
        return;
    for (i = 0u; i < sequence->count; ++i)
        turbowasm_component_value_destroy(&sequence->items[i]);
    turbowasm_rt_free(sequence->items);
    sequence->items = NULL;
    sequence->count = 0u;
}

void turbowasm_component_value_destroy(
    turbowasm_component_value *value) {
    if (value == NULL)
        return;

    switch (value->kind) {
        case TURBOWASM_COMPONENT_TYPE_STRING:
            turbowasm_rt_free(value->as.string.data);
            break;
        case TURBOWASM_COMPONENT_TYPE_LIST:
            destroy_value_sequence(&value->as.list);
            break;
        case TURBOWASM_COMPONENT_TYPE_RECORD:
            destroy_value_sequence(&value->as.record);
            break;
        case TURBOWASM_COMPONENT_TYPE_TUPLE:
            destroy_value_sequence(&value->as.tuple);
            break;
        case TURBOWASM_COMPONENT_TYPE_OPTION:
            if (value->as.option.payload != NULL) {
                turbowasm_component_value_destroy(
                    value->as.option.payload);
                turbowasm_rt_free(value->as.option.payload);
            }
            break;
        case TURBOWASM_COMPONENT_TYPE_RESULT:
            if (value->as.result.payload != NULL) {
                turbowasm_component_value_destroy(
                    value->as.result.payload);
                turbowasm_rt_free(value->as.result.payload);
            }
            break;
        default:
            break;
    }

    memset(value, 0, sizeof(*value));
}

static turbowasm_status lift_scalar(
    turbowasm_component_type_kind kind,
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    turbowasm_component_value *out) {
    uint8_t bytes[8] = {0};
    uint64_t bits;
    size_t width;
    turbowasm_status status;

    status = scalar_width(kind, &width);
    if (status != TURBOWASM_OK)
        return status;
    status = read_memory(
        instance, memory_index, address, bytes, width);
    if (status != TURBOWASM_OK)
        return status;

    bits = read_le(bytes, width);
    memset(out, 0, sizeof(*out));
    out->kind = kind;

    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            if (bits > 1u)
                return TURBOWASM_TRAPPED;
            out->as.boolean = bits != 0u;
            break;
        case TURBOWASM_COMPONENT_TYPE_S8:
            out->as.s8 = (int8_t)(uint8_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_U8:
            out->as.u8 = (uint8_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_S16:
            out->as.s16 = (int16_t)(uint16_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_U16:
            out->as.u16 = (uint16_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_S32:
            out->as.s32 = (int32_t)(uint32_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_U32:
            out->as.u32 = (uint32_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_S64:
            out->as.s64 = (int64_t)bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_U64:
            out->as.u64 = bits;
            break;
        case TURBOWASM_COMPONENT_TYPE_F32: {
            uint32_t word = (uint32_t)bits;
            memcpy(&out->as.f32, &word, sizeof(word));
            break;
        }
        case TURBOWASM_COMPONENT_TYPE_F64:
            memcpy(&out->as.f64, &bits, sizeof(bits));
            break;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            if (!unicode_scalar_valid((uint32_t)bits))
                return TURBOWASM_TRAPPED;
            out->as.character = (uint32_t)bits;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    return TURBOWASM_OK;
}

static turbowasm_status lower_scalar(
    turbowasm_component_type_kind kind,
    turbowasm_instance_impl *instance,
    uint32_t memory_index,
    uint64_t address,
    const turbowasm_component_value *value) {
    uint8_t bytes[8] = {0};
    uint64_t bits = 0u;
    size_t width;
    turbowasm_status status;

    if (value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    status = scalar_width(kind, &width);
    if (status != TURBOWASM_OK)
        return status;

    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            bits = value->as.boolean ? 1u : 0u;
            break;
        case TURBOWASM_COMPONENT_TYPE_S8:
            bits = (uint8_t)value->as.s8;
            break;
        case TURBOWASM_COMPONENT_TYPE_U8:
            bits = value->as.u8;
            break;
        case TURBOWASM_COMPONENT_TYPE_S16:
            bits = (uint16_t)value->as.s16;
            break;
        case TURBOWASM_COMPONENT_TYPE_U16:
            bits = value->as.u16;
            break;
        case TURBOWASM_COMPONENT_TYPE_S32:
            bits = (uint32_t)value->as.s32;
            break;
        case TURBOWASM_COMPONENT_TYPE_U32:
            bits = value->as.u32;
            break;
        case TURBOWASM_COMPONENT_TYPE_S64:
            bits = (uint64_t)value->as.s64;
            break;
        case TURBOWASM_COMPONENT_TYPE_U64:
            bits = value->as.u64;
            break;
        case TURBOWASM_COMPONENT_TYPE_F32: {
            uint32_t word;
            memcpy(&word, &value->as.f32, sizeof(word));
            bits = word;
            break;
        }
        case TURBOWASM_COMPONENT_TYPE_F64:
            memcpy(&bits, &value->as.f64, sizeof(bits));
            break;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            if (!unicode_scalar_valid(value->as.character))
                return TURBOWASM_INVALID_ARGUMENT;
            bits = value->as.character;
            break;
        default:
            return TURBOWASM_UNSUPPORTED;
    }

    write_le(bytes, width, bits);
    return write_memory(
        instance, memory_index, address, bytes, width);
}

static turbowasm_status guest_allocate(
    const turbowasm_component_canonical_memory *memory,
    uint64_t alignment,
    uint64_t size,
    uint64_t *out_pointer) {
    if (memory == NULL || out_pointer == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (size == 0u) {
        *out_pointer = 0u;
        return TURBOWASM_OK;
    }
    if (memory->guest_realloc == NULL)
        return TURBOWASM_UNSUPPORTED;
    return memory->guest_realloc(
        memory->realloc_context,
        0u, 0u, alignment, size, out_pointer);
}

static turbowasm_status lift_string_range(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t pointer,
    uint64_t length,
    turbowasm_component_value *out) {
    turbowasm_status status;
    uint8_t *copy = NULL;

    if (length > TURBOWASM_COMPONENT_MAX_STRING_BYTE_LENGTH ||
        length > (uint64_t)SIZE_MAX)
        return TURBOWASM_TRAPPED;

    {
        uint8_t *range = NULL;
        status = turbowasm_instance_memory_bounds(
            instance, memory->memory_index,
            pointer, 0u, (size_t)length, &range);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (length != 0u) {
        copy = (uint8_t *)turbowasm_rt_malloc((size_t)length);
        if (copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        status = read_memory(
            instance, memory->memory_index,
            pointer, copy, (size_t)length);
        if (status != TURBOWASM_OK) {
            turbowasm_rt_free(copy);
            return status;
        }
        if (!utf8_bytes_valid(copy, (size_t)length)) {
            turbowasm_rt_free(copy);
            return TURBOWASM_TRAPPED;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_COMPONENT_TYPE_STRING;
    out->as.string.data = copy;
    out->as.string.size = (size_t)length;
    return TURBOWASM_OK;
}

static turbowasm_status lift_string(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    turbowasm_component_value *out) {
    uint64_t pointer;
    uint64_t length;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;

    status = read_pointer(instance, memory, address, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    status = read_pointer(
        instance, memory, address + ptr_width, &length);
    if (status != TURBOWASM_OK)
        return status;

    return lift_string_range(
        memory, instance, pointer, length, out);
}

static turbowasm_status lower_string_range(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    const turbowasm_component_value *value,
    uint64_t *out_pointer,
    uint64_t *out_length) {
    uint64_t pointer = 0u;
    turbowasm_status status;

    if (value == NULL || out_pointer == NULL || out_length == NULL ||
        value->kind != TURBOWASM_COMPONENT_TYPE_STRING)
        return TURBOWASM_TYPE_MISMATCH;
    if (value->as.string.size >
            TURBOWASM_COMPONENT_MAX_STRING_BYTE_LENGTH ||
        !utf8_bytes_valid(
            value->as.string.data, value->as.string.size))
        return TURBOWASM_INVALID_ARGUMENT;

    status = guest_allocate(
        memory, 1u, (uint64_t)value->as.string.size, &pointer);
    if (status != TURBOWASM_OK)
        return status;

    if (value->as.string.size != 0u) {
        status = write_memory(
            instance, memory->memory_index, pointer,
            value->as.string.data, value->as.string.size);
        if (status != TURBOWASM_OK)
            return status;
    }

    *out_pointer = pointer;
    *out_length = (uint64_t)value->as.string.size;
    return TURBOWASM_OK;
}

static turbowasm_status lower_string(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    const turbowasm_component_value *value) {
    uint64_t pointer;
    uint64_t length;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;

    status = lower_string_range(
        memory, instance, value, &pointer, &length);
    if (status != TURBOWASM_OK)
        return status;

    status = write_pointer(instance, memory, address, pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    return write_pointer(
        instance, memory, address + ptr_width, length);
}

static turbowasm_status lift_list_range(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t pointer,
    uint64_t count,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_layout layout;
    uint64_t bytes;
    uint64_t i;
    turbowasm_component_value *items = NULL;
    turbowasm_status status;

    if (type == NULL)
        return TURBOWASM_MALFORMED_MODULE;
    status = turbowasm_component_canonical_layout(
        graph, type->as.list.element_type,
        memory->pointer_type, &layout);
    if (status != TURBOWASM_OK)
        return status;
    if (layout.size == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    if (count != 0u && layout.size > UINT64_MAX / count)
        return TURBOWASM_TRAPPED;
    bytes = count * layout.size;
    if (bytes > TURBOWASM_COMPONENT_MAX_LIST_BYTE_LENGTH ||
        count > (uint64_t)SIZE_MAX / sizeof(*items))
        return TURBOWASM_TRAPPED;
    if (layout.alignment != 0u &&
        pointer % layout.alignment != 0u)
        return TURBOWASM_TRAPPED;

    {
        uint8_t *range = NULL;
        if (bytes > (uint64_t)SIZE_MAX)
            return TURBOWASM_TRAPPED;
        status = turbowasm_instance_memory_bounds(
            instance, memory->memory_index,
            pointer, 0u, (size_t)bytes, &range);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (count != 0u) {
        items = (turbowasm_component_value *)turbowasm_rt_calloc(
            (size_t)count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < count; ++i) {
        status = lift_value_inner(
            graph, type->as.list.element_type,
            memory, instance,
            pointer + i * layout.size,
            depth + 1u, &items[i]);
        if (status != TURBOWASM_OK) {
            uint64_t j;
            for (j = 0u; j < i; ++j)
                turbowasm_component_value_destroy(&items[j]);
            turbowasm_rt_free(items);
            return status;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_COMPONENT_TYPE_LIST;
    out->as.list.items = items;
    out->as.list.count = count;
    return TURBOWASM_OK;
}

static turbowasm_status lift_list(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out) {
    uint64_t pointer;
    uint64_t count;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;

    status = read_pointer(instance, memory, address, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    status = read_pointer(
        instance, memory, address + ptr_width, &count);
    if (status != TURBOWASM_OK)
        return status;

    return lift_list_range(
        graph, type, memory, instance,
        pointer, count, depth, out);
}

static turbowasm_status lower_list_range(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint32_t depth,
    const turbowasm_component_value *value,
    uint64_t *out_pointer,
    uint64_t *out_count) {
    turbowasm_component_layout layout;
    uint64_t count;
    uint64_t bytes;
    uint64_t pointer = 0u;
    uint64_t i;
    turbowasm_status status;

    if (type == NULL || value == NULL ||
        out_pointer == NULL || out_count == NULL ||
        value->kind != TURBOWASM_COMPONENT_TYPE_LIST)
        return TURBOWASM_TYPE_MISMATCH;

    status = turbowasm_component_canonical_layout(
        graph, type->as.list.element_type,
        memory->pointer_type, &layout);
    if (status != TURBOWASM_OK)
        return status;

    count = value->as.list.count;
    if (count != 0u && value->as.list.items == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (count != 0u && layout.size > UINT64_MAX / count)
        return TURBOWASM_INVALID_ARGUMENT;
    bytes = count * layout.size;
    if (bytes > TURBOWASM_COMPONENT_MAX_LIST_BYTE_LENGTH)
        return TURBOWASM_INVALID_ARGUMENT;

    status = guest_allocate(
        memory, layout.alignment, bytes, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (layout.alignment != 0u &&
        pointer % layout.alignment != 0u)
        return TURBOWASM_TRAPPED;

    for (i = 0u; i < count; ++i) {
        status = lower_value_inner(
            graph, type->as.list.element_type,
            memory, instance,
            pointer + i * layout.size,
            depth + 1u,
            &value->as.list.items[i]);
        if (status != TURBOWASM_OK)
            return status;
    }

    *out_pointer = pointer;
    *out_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status lower_list(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value) {
    uint64_t pointer;
    uint64_t count;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;

    status = lower_list_range(
        graph, type, memory, instance,
        depth, value, &pointer, &count);
    if (status != TURBOWASM_OK)
        return status;

    status = write_pointer(instance, memory, address, pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    return write_pointer(
        instance, memory, address + ptr_width, count);
}

static turbowasm_status composite_sequence_info(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    uint32_t *out_count) {
    if (type == NULL || out_count == NULL)
        return TURBOWASM_MALFORMED_MODULE;
    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD) {
        *out_count = type->as.record.count;
        return *out_count != 0u
            ? TURBOWASM_OK
            : TURBOWASM_MALFORMED_MODULE;
    }
    if (kind == TURBOWASM_COMPONENT_TYPE_TUPLE) {
        *out_count = type->as.tuple.count;
        return *out_count != 0u
            ? TURBOWASM_OK
            : TURBOWASM_MALFORMED_MODULE;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_component_type_ref composite_sequence_ref(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    uint32_t index) {
    return kind == TURBOWASM_COMPONENT_TYPE_RECORD
        ? type->as.record.fields[index].type
        : type->as.tuple.elements[index];
}

static turbowasm_status lift_sequence_value(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_value *items = NULL;
    uint32_t count = 0u;
    uint32_t i;
    uint64_t cursor = address;
    turbowasm_status status;

    status = composite_sequence_info(type, kind, &count);
    if (status != TURBOWASM_OK)
        return status;
    if ((size_t)count > SIZE_MAX / sizeof(*items))
        return TURBOWASM_OUT_OF_MEMORY;

    items = (turbowasm_component_value *)turbowasm_rt_calloc(
        (size_t)count, sizeof(*items));
    if (items == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_type_ref child =
            composite_sequence_ref(type, kind, i);
        turbowasm_component_layout layout;
        uint64_t aligned;

        status = turbowasm_component_canonical_layout(
            graph, child, memory->pointer_type, &layout);
        if (status != TURBOWASM_OK)
            goto fail;
        if (!align_up_u64(cursor, layout.alignment, &aligned) ||
            aligned > UINT64_MAX - layout.size) {
            status = TURBOWASM_TRAPPED;
            goto fail;
        }

        status = lift_value_inner(
            graph, child, memory, instance,
            aligned, depth + 1u, &items[i]);
        if (status != TURBOWASM_OK)
            goto fail;
        cursor = aligned + layout.size;
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD) {
        out->as.record.items = items;
        out->as.record.count = count;
    } else {
        out->as.tuple.items = items;
        out->as.tuple.count = count;
    }
    return TURBOWASM_OK;

fail:
    while (i != 0u) {
        --i;
        turbowasm_component_value_destroy(&items[i]);
    }
    turbowasm_rt_free(items);
    return status;
}

static turbowasm_status lower_sequence_value(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value) {
    const turbowasm_component_value_list *sequence;
    uint32_t count = 0u;
    uint32_t i;
    uint64_t cursor = address;
    turbowasm_status status;

    if (value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    status = composite_sequence_info(type, kind, &count);
    if (status != TURBOWASM_OK)
        return status;

    sequence = kind == TURBOWASM_COMPONENT_TYPE_RECORD
        ? &value->as.record
        : &value->as.tuple;
    if (sequence->count != count ||
        (count != 0u && sequence->items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_type_ref child =
            composite_sequence_ref(type, kind, i);
        turbowasm_component_layout layout;
        uint64_t aligned;

        status = turbowasm_component_canonical_layout(
            graph, child, memory->pointer_type, &layout);
        if (status != TURBOWASM_OK)
            return status;
        if (!align_up_u64(cursor, layout.alignment, &aligned) ||
            aligned > UINT64_MAX - layout.size)
            return TURBOWASM_TRAPPED;

        status = lower_value_inner(
            graph, child, memory, instance,
            aligned, depth + 1u, &sequence->items[i]);
        if (status != TURBOWASM_OK)
            return status;
        cursor = aligned + layout.size;
    }

    return TURBOWASM_OK;
}

static turbowasm_status variant_case_ref(
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    uint32_t case_index,
    bool *out_has_payload,
    turbowasm_component_type_ref *out_payload) {
    if (type == NULL || out_has_payload == NULL ||
        out_payload == NULL || case_index >= 2u)
        return TURBOWASM_INVALID_ARGUMENT;

    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION) {
        *out_has_payload = case_index == 1u;
        if (*out_has_payload)
            *out_payload = type->as.option.payload;
        return TURBOWASM_OK;
    }
    if (kind == TURBOWASM_COMPONENT_TYPE_RESULT) {
        *out_has_payload = case_index == 0u
            ? type->as.result.has_ok
            : type->as.result.has_error;
        if (*out_has_payload) {
            *out_payload = case_index == 0u
                ? type->as.result.ok
                : type->as.result.error;
        }
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status variant_payload_alignment(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    turbowasm_component_pointer_type pointer_type,
    uint64_t *out_alignment) {
    uint64_t alignment = 1u;
    uint32_t i;

    if (out_alignment == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < 2u; ++i) {
        bool has_payload = false;
        turbowasm_component_type_ref payload;
        turbowasm_component_layout layout;
        turbowasm_status status = variant_case_ref(
            type, kind, i, &has_payload, &payload);

        if (status != TURBOWASM_OK)
            return status;
        if (!has_payload)
            continue;
        status = turbowasm_component_canonical_layout(
            graph, payload, pointer_type, &layout);
        if (status != TURBOWASM_OK)
            return status;
        if (layout.alignment > alignment)
            alignment = layout.alignment;
    }
    *out_alignment = alignment;
    return TURBOWASM_OK;
}

static turbowasm_status lift_variant_value(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out) {
    uint8_t discriminant = 0u;
    uint64_t payload_alignment = 1u;
    uint64_t payload_address;
    bool has_payload = false;
    turbowasm_component_type_ref payload_type;
    turbowasm_component_value *payload = NULL;
    turbowasm_status status;

    status = read_memory(
        instance, memory->memory_index,
        address, &discriminant, 1u);
    if (status != TURBOWASM_OK)
        return status;
    if (discriminant >= 2u)
        return TURBOWASM_TRAPPED;

    status = variant_case_ref(
        type, kind, discriminant,
        &has_payload, &payload_type);
    if (status != TURBOWASM_OK)
        return status;
    status = variant_payload_alignment(
        graph, type, kind, memory->pointer_type,
        &payload_alignment);
    if (status != TURBOWASM_OK)
        return status;

    if (address == UINT64_MAX ||
        !align_up_u64(
            address + UINT64_C(1),
            payload_alignment,
            &payload_address))
        return TURBOWASM_TRAPPED;

    if (has_payload) {
        payload = (turbowasm_component_value *)
            turbowasm_rt_calloc(1u, sizeof(*payload));
        if (payload == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        status = lift_value_inner(
            graph, payload_type, memory, instance,
            payload_address, depth + 1u, payload);
        if (status != TURBOWASM_OK) {
            turbowasm_component_value_destroy(payload);
            turbowasm_rt_free(payload);
            return status;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION) {
        out->as.option.case_index = discriminant;
        out->as.option.payload = payload;
    } else {
        out->as.result.case_index = discriminant;
        out->as.result.payload = payload;
    }
    return TURBOWASM_OK;
}

static turbowasm_status lower_variant_value(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value) {
    const turbowasm_component_value_variant *variant;
    uint8_t discriminant;
    uint64_t payload_alignment = 1u;
    uint64_t payload_address;
    bool has_payload = false;
    turbowasm_component_type_ref payload_type;
    turbowasm_status status;

    if (value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    variant = kind == TURBOWASM_COMPONENT_TYPE_OPTION
        ? &value->as.option
        : &value->as.result;
    if (variant->case_index >= 2u)
        return TURBOWASM_TYPE_MISMATCH;

    status = variant_case_ref(
        type, kind, variant->case_index,
        &has_payload, &payload_type);
    if (status != TURBOWASM_OK)
        return status;
    if (has_payload != (variant->payload != NULL))
        return TURBOWASM_TYPE_MISMATCH;

    discriminant = (uint8_t)variant->case_index;
    status = write_memory(
        instance, memory->memory_index,
        address, &discriminant, 1u);
    if (status != TURBOWASM_OK)
        return status;

    if (!has_payload)
        return TURBOWASM_OK;

    status = variant_payload_alignment(
        graph, type, kind, memory->pointer_type,
        &payload_alignment);
    if (status != TURBOWASM_OK)
        return status;
    if (address == UINT64_MAX ||
        !align_up_u64(
            address + UINT64_C(1),
            payload_alignment,
            &payload_address))
        return TURBOWASM_TRAPPED;

    return lower_value_inner(
        graph, payload_type, memory, instance,
        payload_address, depth + 1u, variant->payload);
}

static turbowasm_status lift_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    size_t width;
    turbowasm_status status;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_TRAPPED;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    if (scalar_width(kind, &width) == TURBOWASM_OK)
        return lift_scalar(
            kind, instance, memory->memory_index, address, out);

    if (kind == TURBOWASM_COMPONENT_TYPE_STRING)
        return lift_string(memory, instance, address, out);
    if (kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return lift_list(
            graph, type, memory, instance,
            address, depth, out);
    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD ||
        kind == TURBOWASM_COMPONENT_TYPE_TUPLE)
        return lift_sequence_value(
            graph, type, kind, memory, instance,
            address, depth, out);
    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION ||
        kind == TURBOWASM_COMPONENT_TYPE_RESULT)
        return lift_variant_value(
            graph, type, kind, memory, instance,
            address, depth, out);
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM ||
        kind == TURBOWASM_COMPONENT_TYPE_FLAGS)
        return lift_label_value(
            type, kind, instance, memory->memory_index,
            address, out);

    if (kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        kind == TURBOWASM_COMPONENT_TYPE_BORROW) {
        uint8_t bytes[4] = {0};
        uint32_t handle;

        if (memory->resource_lift == NULL)
            return TURBOWASM_UNSUPPORTED;
        status = read_memory(
            instance, memory->memory_index,
            address, bytes, sizeof(bytes));
        if (status != TURBOWASM_OK)
            return status;
        handle = (uint32_t)read_le(bytes, sizeof(bytes));
        return memory->resource_lift(
            memory->resource_context,
            graph, ref, handle, out);
    }

    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status lower_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    size_t width;
    turbowasm_status status;

    if (value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_TRAPPED;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    if (scalar_width(kind, &width) == TURBOWASM_OK)
        return lower_scalar(
            kind, instance, memory->memory_index,
            address, value);

    if (kind == TURBOWASM_COMPONENT_TYPE_STRING)
        return lower_string(memory, instance, address, value);
    if (kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return lower_list(
            graph, type, memory, instance,
            address, depth, value);
    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD ||
        kind == TURBOWASM_COMPONENT_TYPE_TUPLE)
        return lower_sequence_value(
            graph, type, kind, memory, instance,
            address, depth, value);
    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION ||
        kind == TURBOWASM_COMPONENT_TYPE_RESULT)
        return lower_variant_value(
            graph, type, kind, memory, instance,
            address, depth, value);
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM ||
        kind == TURBOWASM_COMPONENT_TYPE_FLAGS)
        return lower_label_value(
            type, kind, instance, memory->memory_index,
            address, value);

    if (kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        kind == TURBOWASM_COMPONENT_TYPE_BORROW) {
        uint8_t bytes[4] = {0};
        uint32_t handle;

        if (memory->resource_lower == NULL)
            return TURBOWASM_UNSUPPORTED;
        status = memory->resource_lower(
            memory->resource_context,
            graph, ref, value, &handle);
        if (status != TURBOWASM_OK)
            return status;
        write_le(bytes, sizeof(bytes), handle);
        return write_memory(
            instance, memory->memory_index,
            address, bytes, sizeof(bytes));
    }

    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status flat_pointer_value(
    turbowasm_component_pointer_type pointer_type,
    uint64_t value,
    turbowasm_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value > UINT32_MAX)
            return TURBOWASM_TRAPPED;
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)value;
        return TURBOWASM_OK;
    }
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I64) {
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)value;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status flat_pointer_read(
    turbowasm_component_pointer_type pointer_type,
    const turbowasm_value *value,
    uint64_t *out) {
    if (value == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value->kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint32_t)value->as.i32;
        return TURBOWASM_OK;
    }
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I64) {
        if (value->kind != TURBOWASM_VALUE_I64)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint64_t)value->as.i64;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status lower_flat_scalar(
    turbowasm_component_type_kind kind,
    const turbowasm_component_value *value,
    turbowasm_value *out) {
    if (value == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;

    memset(out, 0, sizeof(*out));
    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = value->as.boolean ? 1 : 0;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S8:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.s8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U8:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.u8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S16:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.s16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U16:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.u16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S32:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = value->as.s32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U32:
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.u32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S64:
            out->kind = TURBOWASM_VALUE_I64;
            out->as.i64 = value->as.s64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U64:
            out->kind = TURBOWASM_VALUE_I64;
            out->as.i64 = (int64_t)value->as.u64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_F32:
            out->kind = TURBOWASM_VALUE_F32;
            out->as.f32 = value->as.f32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_F64:
            out->kind = TURBOWASM_VALUE_F64;
            out->as.f64 = value->as.f64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            if (!unicode_scalar_valid(value->as.character))
                return TURBOWASM_INVALID_ARGUMENT;
            out->kind = TURBOWASM_VALUE_I32;
            out->as.i32 = (int32_t)value->as.character;
            return TURBOWASM_OK;
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status lift_flat_scalar(
    turbowasm_component_type_kind kind,
    const turbowasm_value *value,
    turbowasm_component_value *out) {
    uint32_t bits32;
    uint64_t bits64;

    if (value == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->kind = kind;

    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            bits32 = (uint32_t)value->as.i32;
            if (bits32 > 1u)
                return TURBOWASM_TRAPPED;
            out->as.boolean = bits32 != 0u;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S8:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.s8 = (int8_t)(uint8_t)value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U8:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.u8 = (uint8_t)value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S16:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.s16 = (int16_t)(uint16_t)value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U16:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.u16 = (uint16_t)value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S32:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.s32 = value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U32:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.u32 = (uint32_t)value->as.i32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_S64:
            if (value->kind != TURBOWASM_VALUE_I64)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.s64 = value->as.i64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_U64:
            if (value->kind != TURBOWASM_VALUE_I64)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.u64 = (uint64_t)value->as.i64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_F32:
            if (value->kind != TURBOWASM_VALUE_F32)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.f32 = value->as.f32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_F64:
            if (value->kind != TURBOWASM_VALUE_F64)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.f64 = value->as.f64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            if (value->kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            bits32 = (uint32_t)value->as.i32;
            if (!unicode_scalar_valid(bits32))
                return TURBOWASM_TRAPPED;
            out->as.character = bits32;
            return TURBOWASM_OK;
        default:
            bits64 = 0u;
            (void)bits64;
            return TURBOWASM_UNSUPPORTED;
    }
}

static bool flat_type_value_kind(
    turbowasm_component_flat_type flat,
    turbowasm_value_kind *out) {
    if (out == NULL)
        return false;
    switch (flat) {
        case TURBOWASM_COMPONENT_FLAT_I32:
            *out = TURBOWASM_VALUE_I32;
            return true;
        case TURBOWASM_COMPONENT_FLAT_I64:
            *out = TURBOWASM_VALUE_I64;
            return true;
        case TURBOWASM_COMPONENT_FLAT_F32:
            *out = TURBOWASM_VALUE_F32;
            return true;
        case TURBOWASM_COMPONENT_FLAT_F64:
            *out = TURBOWASM_VALUE_F64;
            return true;
        default:
            return false;
    }
}

static void zero_flat_value(
    turbowasm_component_flat_type type,
    turbowasm_value *out) {
    turbowasm_value_kind kind = TURBOWASM_VALUE_I32;

    memset(out, 0, sizeof(*out));
    if (flat_type_value_kind(type, &kind))
        out->kind = kind;
}

static turbowasm_status coerce_lower_variant_carrier(
    turbowasm_component_flat_type have,
    turbowasm_component_flat_type want,
    const turbowasm_value *input,
    turbowasm_value *out) {
    uint32_t bits32;
    uint64_t bits64;
    turbowasm_value_kind have_kind;

    if (input == NULL || out == NULL ||
        !flat_type_value_kind(have, &have_kind) ||
        input->kind != have_kind)
        return TURBOWASM_TYPE_MISMATCH;

    if (have == want) {
        *out = *input;
        return TURBOWASM_OK;
    }

    memset(out, 0, sizeof(*out));
    if (have == TURBOWASM_COMPONENT_FLAT_F32 &&
        want == TURBOWASM_COMPONENT_FLAT_I32) {
        memcpy(&bits32, &input->as.f32, sizeof(bits32));
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)bits32;
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_I32 &&
        want == TURBOWASM_COMPONENT_FLAT_I64) {
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)(uint64_t)(uint32_t)input->as.i32;
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_F32 &&
        want == TURBOWASM_COMPONENT_FLAT_I64) {
        memcpy(&bits32, &input->as.f32, sizeof(bits32));
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)(uint64_t)bits32;
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_F64 &&
        want == TURBOWASM_COMPONENT_FLAT_I64) {
        memcpy(&bits64, &input->as.f64, sizeof(bits64));
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)bits64;
        return TURBOWASM_OK;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status coerce_lift_variant_carrier(
    turbowasm_component_flat_type have,
    turbowasm_component_flat_type want,
    const turbowasm_value *input,
    turbowasm_value *out) {
    uint32_t bits32;
    uint64_t bits64;
    turbowasm_value_kind have_kind;

    if (input == NULL || out == NULL ||
        !flat_type_value_kind(have, &have_kind) ||
        input->kind != have_kind)
        return TURBOWASM_TYPE_MISMATCH;

    if (have == want) {
        *out = *input;
        return TURBOWASM_OK;
    }

    memset(out, 0, sizeof(*out));
    if (have == TURBOWASM_COMPONENT_FLAT_I32 &&
        want == TURBOWASM_COMPONENT_FLAT_F32) {
        bits32 = (uint32_t)input->as.i32;
        out->kind = TURBOWASM_VALUE_F32;
        memcpy(&out->as.f32, &bits32, sizeof(bits32));
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_I64 &&
        want == TURBOWASM_COMPONENT_FLAT_I32) {
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)input->as.i64;
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_I64 &&
        want == TURBOWASM_COMPONENT_FLAT_F32) {
        bits32 = (uint32_t)input->as.i64;
        out->kind = TURBOWASM_VALUE_F32;
        memcpy(&out->as.f32, &bits32, sizeof(bits32));
        return TURBOWASM_OK;
    }
    if (have == TURBOWASM_COMPONENT_FLAT_I64 &&
        want == TURBOWASM_COMPONENT_FLAT_F64) {
        bits64 = (uint64_t)input->as.i64;
        out->kind = TURBOWASM_VALUE_F64;
        memcpy(&out->as.f64, &bits64, sizeof(bits64));
        return TURBOWASM_OK;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status lower_flat_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t depth,
    uint32_t *out_count);

static turbowasm_status lift_flat_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    uint32_t depth,
    turbowasm_component_value *out);

static turbowasm_status lower_flat_dynamic_pair(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t depth,
    uint32_t *out_count) {
    turbowasm_instance_impl *instance = NULL;
    uint64_t pointer = 0u;
    uint64_t length = 0u;
    turbowasm_status status;

    if (out_capacity < 2u)
        return TURBOWASM_INVALID_ARGUMENT;
    status = canonical_memory_validate(memory, &instance);
    if (status != TURBOWASM_OK)
        return status;

    if (kind == TURBOWASM_COMPONENT_TYPE_STRING) {
        status = lower_string_range(
            memory, instance, value, &pointer, &length);
    } else if (kind == TURBOWASM_COMPONENT_TYPE_LIST) {
        if (type == NULL)
            return TURBOWASM_MALFORMED_MODULE;
        status = lower_list_range(
            graph, type, memory, instance,
            depth, value, &pointer, &length);
    } else {
        return TURBOWASM_INVALID_ARGUMENT;
    }
    if (status != TURBOWASM_OK)
        return status;

    status = flat_pointer_value(
        memory->pointer_type, pointer, &out[0]);
    if (status != TURBOWASM_OK)
        return status;
    status = flat_pointer_value(
        memory->pointer_type, length, &out[1]);
    if (status != TURBOWASM_OK)
        return status;

    *out_count = 2u;
    return TURBOWASM_OK;
}

static turbowasm_status lift_flat_dynamic_pair(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_instance_impl *instance = NULL;
    const turbowasm_module_impl *module;
    turbowasm_runtime_scope scope;
    uint64_t pointer;
    uint64_t length;
    turbowasm_status status;

    if (value_count != 2u)
        return TURBOWASM_TYPE_MISMATCH;
    status = canonical_memory_validate(memory, &instance);
    if (status != TURBOWASM_OK)
        return status;
    status = flat_pointer_read(
        memory->pointer_type, &values[0], &pointer);
    if (status != TURBOWASM_OK)
        return status;
    status = flat_pointer_read(
        memory->pointer_type, &values[1], &length);
    if (status != TURBOWASM_OK)
        return status;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(&module->config);
    if (kind == TURBOWASM_COMPONENT_TYPE_STRING) {
        status = lift_string_range(
            memory, instance, pointer, length, out);
    } else if (kind == TURBOWASM_COMPONENT_TYPE_LIST) {
        if (type == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
        } else {
            status = lift_list_range(
                graph, type, memory, instance,
                pointer, length, depth, out);
        }
    } else {
        status = TURBOWASM_INVALID_ARGUMENT;
    }
    turbowasm_runtime_scope_leave(scope);
    return status;
}

static turbowasm_status lower_flat_sequence(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t depth,
    uint32_t *out_count) {
    const turbowasm_component_value_list *sequence;
    uint32_t count = 0u;
    uint32_t cursor = 0u;
    uint32_t i;
    turbowasm_status status;

    if (value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    status = composite_sequence_info(type, kind, &count);
    if (status != TURBOWASM_OK)
        return status;
    sequence = kind == TURBOWASM_COMPONENT_TYPE_RECORD
        ? &value->as.record
        : &value->as.tuple;
    if (sequence->count != count ||
        (count != 0u && sequence->items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < count; ++i) {
        uint32_t part_count = 0u;
        if (cursor > out_capacity)
            return TURBOWASM_INVALID_ARGUMENT;
        status = lower_flat_value_inner(
            graph,
            composite_sequence_ref(type, kind, i),
            memory,
            &sequence->items[i],
            out + cursor,
            out_capacity - cursor,
            depth + 1u,
            &part_count);
        if (status != TURBOWASM_OK)
            return status;
        if (part_count > out_capacity - cursor)
            return TURBOWASM_INVALID_ARGUMENT;
        cursor += part_count;
    }

    *out_count = cursor;
    return TURBOWASM_OK;
}

static turbowasm_status lift_flat_sequence(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_value *items = NULL;
    uint32_t count = 0u;
    uint32_t cursor = 0u;
    uint32_t i;
    turbowasm_status status;

    status = composite_sequence_info(type, kind, &count);
    if (status != TURBOWASM_OK)
        return status;
    if ((size_t)count > SIZE_MAX / sizeof(*items))
        return TURBOWASM_OUT_OF_MEMORY;
    items = (turbowasm_component_value *)turbowasm_rt_calloc(
        (size_t)count, sizeof(*items));
    if (items == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_flat_type_list flat;
        uint32_t part_count;

        status = canonical_flatten_type_inner(
            graph,
            composite_sequence_ref(type, kind, i),
            memory != NULL
                ? memory->pointer_type
                : TURBOWASM_COMPONENT_POINTER_I32,
            depth + 1u,
            &flat);
        if (status != TURBOWASM_OK)
            goto fail;
        part_count = flat.count;
        if (part_count > value_count - cursor) {
            status = TURBOWASM_TYPE_MISMATCH;
            goto fail;
        }
        status = lift_flat_value_inner(
            graph,
            composite_sequence_ref(type, kind, i),
            memory,
            values + cursor,
            part_count,
            depth + 1u,
            &items[i]);
        if (status != TURBOWASM_OK)
            goto fail;
        cursor += part_count;
    }

    if (cursor != value_count) {
        status = TURBOWASM_TYPE_MISMATCH;
        goto fail;
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD) {
        out->as.record.items = items;
        out->as.record.count = count;
    } else {
        out->as.tuple.items = items;
        out->as.tuple.count = count;
    }
    return TURBOWASM_OK;

fail:
    while (i != 0u) {
        --i;
        turbowasm_component_value_destroy(&items[i]);
    }
    turbowasm_rt_free(items);
    return status;
}

static turbowasm_status lower_flat_variant(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t depth,
    uint32_t *out_count) {
    const turbowasm_component_value_variant *variant;
    turbowasm_component_flat_type_list full_flat;
    turbowasm_component_flat_type_list payload_flat = {{0}, 0u};
    turbowasm_value
        payload_values[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
    bool has_payload = false;
    turbowasm_component_type_ref payload_type;
    uint32_t payload_count = 0u;
    uint32_t joined_count;
    uint32_t i;
    turbowasm_status status;

    if (value == NULL || value->kind != kind)
        return TURBOWASM_TYPE_MISMATCH;
    variant = kind == TURBOWASM_COMPONENT_TYPE_OPTION
        ? &value->as.option
        : &value->as.result;
    if (variant->case_index >= 2u)
        return TURBOWASM_TYPE_MISMATCH;

    status = canonical_flatten_type_inner(
        graph, ref,
        memory != NULL
            ? memory->pointer_type
            : TURBOWASM_COMPONENT_POINTER_I32,
        depth, &full_flat);
    if (status != TURBOWASM_OK)
        return status;
    if (full_flat.count == 0u ||
        full_flat.types[0] != TURBOWASM_COMPONENT_FLAT_I32 ||
        out_capacity < full_flat.count)
        return TURBOWASM_INVALID_ARGUMENT;

    status = variant_case_ref(
        type, kind, variant->case_index,
        &has_payload, &payload_type);
    if (status != TURBOWASM_OK)
        return status;
    if (has_payload != (variant->payload != NULL))
        return TURBOWASM_TYPE_MISMATCH;

    out[0].kind = TURBOWASM_VALUE_I32;
    out[0].as.i32 = (int32_t)variant->case_index;
    joined_count = full_flat.count - 1u;

    if (has_payload) {
        status = canonical_flatten_type_inner(
            graph, payload_type,
            memory != NULL
                ? memory->pointer_type
                : TURBOWASM_COMPONENT_POINTER_I32,
            depth + 1u, &payload_flat);
        if (status != TURBOWASM_OK)
            return status;
        status = lower_flat_value_inner(
            graph, payload_type, memory,
            variant->payload,
            payload_values,
            TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS,
            depth + 1u,
            &payload_count);
        if (status != TURBOWASM_OK)
            return status;
        if (payload_count != payload_flat.count ||
            payload_count > joined_count)
            return TURBOWASM_MALFORMED_MODULE;
    }

    for (i = 0u; i < joined_count; ++i) {
        if (i < payload_count) {
            status = coerce_lower_variant_carrier(
                payload_flat.types[i],
                full_flat.types[i + 1u],
                &payload_values[i],
                &out[i + 1u]);
            if (status != TURBOWASM_OK)
                return status;
        } else {
            zero_flat_value(
                full_flat.types[i + 1u],
                &out[i + 1u]);
        }
    }

    *out_count = full_flat.count;
    return TURBOWASM_OK;
}

static turbowasm_status lift_flat_variant(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    turbowasm_component_type_kind kind,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_flat_type_list full_flat;
    turbowasm_component_flat_type_list payload_flat = {{0}, 0u};
    turbowasm_value
        payload_values[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
    turbowasm_component_value *payload = NULL;
    bool has_payload = false;
    turbowasm_component_type_ref payload_type;
    uint32_t case_index;
    uint32_t i;
    turbowasm_status status;

    status = canonical_flatten_type_inner(
        graph, ref,
        memory != NULL
            ? memory->pointer_type
            : TURBOWASM_COMPONENT_POINTER_I32,
        depth, &full_flat);
    if (status != TURBOWASM_OK)
        return status;
    if (full_flat.count != value_count ||
        value_count == 0u ||
        values[0].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    case_index = (uint32_t)values[0].as.i32;
    if (case_index >= 2u)
        return TURBOWASM_TRAPPED;

    status = variant_case_ref(
        type, kind, case_index,
        &has_payload, &payload_type);
    if (status != TURBOWASM_OK)
        return status;

    if (has_payload) {
        status = canonical_flatten_type_inner(
            graph, payload_type,
            memory != NULL
                ? memory->pointer_type
                : TURBOWASM_COMPONENT_POINTER_I32,
            depth + 1u, &payload_flat);
        if (status != TURBOWASM_OK)
            return status;
        if (payload_flat.count > value_count - 1u)
            return TURBOWASM_MALFORMED_MODULE;

        for (i = 0u; i < payload_flat.count; ++i) {
            status = coerce_lift_variant_carrier(
                full_flat.types[i + 1u],
                payload_flat.types[i],
                &values[i + 1u],
                &payload_values[i]);
            if (status != TURBOWASM_OK)
                return status;
        }

        payload = (turbowasm_component_value *)
            turbowasm_rt_calloc(1u, sizeof(*payload));
        if (payload == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        status = lift_flat_value_inner(
            graph, payload_type, memory,
            payload_values, payload_flat.count,
            depth + 1u, payload);
        if (status != TURBOWASM_OK) {
            turbowasm_component_value_destroy(payload);
            turbowasm_rt_free(payload);
            return status;
        }
    }

    for (i = 1u; i < value_count; ++i) {
        turbowasm_value_kind expected_kind;
        if (!flat_type_value_kind(
                full_flat.types[i], &expected_kind) ||
            values[i].kind != expected_kind) {
            if (payload != NULL) {
                turbowasm_component_value_destroy(payload);
                turbowasm_rt_free(payload);
            }
            return TURBOWASM_TYPE_MISMATCH;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION) {
        out->as.option.case_index = case_index;
        out->as.option.payload = payload;
    } else {
        out->as.result.case_index = case_index;
        out->as.result.payload = payload;
    }
    return TURBOWASM_OK;
}

static turbowasm_status lower_flat_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t depth,
    uint32_t *out_count) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    turbowasm_status status;

    if (graph == NULL || value == NULL || out == NULL ||
        out_count == NULL || depth >=
            TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_INVALID_ARGUMENT;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    if (kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
        kind <= TURBOWASM_COMPONENT_TYPE_CHAR) {
        if (out_capacity < 1u)
            return TURBOWASM_INVALID_ARGUMENT;
        status = lower_flat_scalar(kind, value, &out[0]);
        if (status == TURBOWASM_OK)
            *out_count = 1u;
        return status;
    }

    if (kind == TURBOWASM_COMPONENT_TYPE_STRING ||
        kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return lower_flat_dynamic_pair(
            graph, type, kind, memory, value,
            out, out_capacity, depth, out_count);

    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD ||
        kind == TURBOWASM_COMPONENT_TYPE_TUPLE)
        return lower_flat_sequence(
            graph, type, kind, memory, value,
            out, out_capacity, depth, out_count);

    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION ||
        kind == TURBOWASM_COMPONENT_TYPE_RESULT)
        return lower_flat_variant(
            graph, type, kind, ref, memory, value,
            out, out_capacity, depth, out_count);

    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM ||
        kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        if (out_capacity < 1u)
            return TURBOWASM_INVALID_ARGUMENT;
        status = lower_flat_label_value(
            type, kind, value, &out[0]);
        if (status == TURBOWASM_OK)
            *out_count = 1u;
        return status;
    }

    if (kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        kind == TURBOWASM_COMPONENT_TYPE_BORROW) {
        uint32_t handle;
        if (out_capacity < 1u)
            return TURBOWASM_INVALID_ARGUMENT;
        if (memory == NULL || memory->resource_lower == NULL)
            return TURBOWASM_UNSUPPORTED;
        status = memory->resource_lower(
            memory->resource_context,
            graph, ref, value, &handle);
        if (status != TURBOWASM_OK)
            return status;
        out[0].kind = TURBOWASM_VALUE_I32;
        out[0].as.i32 = (int32_t)handle;
        *out_count = 1u;
        return TURBOWASM_OK;
    }

    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status lift_flat_value_inner(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_type_kind kind;
    const turbowasm_component_type *type;
    turbowasm_status status;

    if (graph == NULL || values == NULL || out == NULL ||
        depth >= TURBOWASM_COMPONENT_CANONICAL_MAX_DEPTH)
        return TURBOWASM_INVALID_ARGUMENT;

    status = resolved_kind(graph, ref, &kind, &type);
    if (status != TURBOWASM_OK)
        return status;

    if (kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
        kind <= TURBOWASM_COMPONENT_TYPE_CHAR) {
        if (value_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return lift_flat_scalar(kind, &values[0], out);
    }

    if (kind == TURBOWASM_COMPONENT_TYPE_STRING ||
        kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return lift_flat_dynamic_pair(
            graph, type, kind, memory,
            values, value_count, depth, out);

    if (kind == TURBOWASM_COMPONENT_TYPE_RECORD ||
        kind == TURBOWASM_COMPONENT_TYPE_TUPLE)
        return lift_flat_sequence(
            graph, type, kind, memory,
            values, value_count, depth, out);

    if (kind == TURBOWASM_COMPONENT_TYPE_OPTION ||
        kind == TURBOWASM_COMPONENT_TYPE_RESULT)
        return lift_flat_variant(
            graph, type, kind, ref, memory,
            values, value_count, depth, out);

    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM ||
        kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        if (value_count != 1u)
            return TURBOWASM_TYPE_MISMATCH;
        return lift_flat_label_value(
            type, kind, &values[0], out);
    }

    if (kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        kind == TURBOWASM_COMPONENT_TYPE_BORROW) {
        if (value_count != 1u ||
            values[0].kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;
        if (memory == NULL || memory->resource_lift == NULL)
            return TURBOWASM_UNSUPPORTED;
        return memory->resource_lift(
            memory->resource_context,
            graph, ref,
            (uint32_t)values[0].as.i32,
            out);
    }

    return TURBOWASM_UNSUPPORTED;
}

turbowasm_status turbowasm_component_canonical_lower_flat_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t *out_count) {
    turbowasm_component_flat_type_list flat;
    turbowasm_component_pointer_type pointer_type =
        memory != NULL
            ? memory->pointer_type
            : TURBOWASM_COMPONENT_POINTER_I32;
    turbowasm_status status;
    uint32_t actual_count = 0u;

    if (graph == NULL || value == NULL ||
        out == NULL || out_count == NULL ||
        out_capacity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    status = canonical_flatten_type_inner(
        graph, ref, pointer_type, 0u, &flat);
    if (status != TURBOWASM_OK)
        return status;
    if (flat.count > out_capacity)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, (size_t)out_capacity * sizeof(*out));
    status = lower_flat_value_inner(
        graph, ref, memory, value,
        out, out_capacity, 0u, &actual_count);
    if (status != TURBOWASM_OK)
        return status;
    if (actual_count != flat.count)
        return TURBOWASM_MALFORMED_MODULE;

    *out_count = actual_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_canonical_lift_flat_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    turbowasm_component_value *out) {
    turbowasm_component_flat_type_list flat;
    turbowasm_component_pointer_type pointer_type =
        memory != NULL
            ? memory->pointer_type
            : TURBOWASM_COMPONENT_POINTER_I32;
    turbowasm_status status;

    if (graph == NULL || values == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = canonical_flatten_type_inner(
        graph, ref, pointer_type, 0u, &flat);
    if (status != TURBOWASM_OK)
        return status;
    if (flat.count != value_count)
        return TURBOWASM_TYPE_MISMATCH;

    memset(out, 0, sizeof(*out));
    status = lift_flat_value_inner(
        graph, ref, memory, values, value_count, 0u, out);
    if (status != TURBOWASM_OK)
        turbowasm_component_value_destroy(out);
    return status;
}

turbowasm_status turbowasm_component_canonical_lift_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    turbowasm_component_value *out) {
    turbowasm_instance_impl *instance;
    const turbowasm_module_impl *module;
    turbowasm_component_layout layout;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    uint8_t *range = NULL;

    if (graph == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = canonical_memory_validate(memory, &instance);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_component_canonical_layout(
        graph, type, memory->pointer_type, &layout);
    if (status != TURBOWASM_OK)
        return status;
    if (layout.alignment == 0u ||
        address % layout.alignment != 0u ||
        layout.size > (uint64_t)SIZE_MAX)
        return TURBOWASM_TRAPPED;
    status = turbowasm_instance_memory_bounds(
        instance, memory->memory_index,
        address, 0u, (size_t)layout.size, &range);
    if (status != TURBOWASM_OK)
        return status;

    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    scope = turbowasm_runtime_scope_enter(&module->config);
    status = lift_value_inner(
        graph, type, memory, instance, address, 0u, out);
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK)
        turbowasm_component_value_destroy(out);
    return status;
}

turbowasm_status turbowasm_component_canonical_lower_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    const turbowasm_component_value *value) {
    turbowasm_instance_impl *instance;
    turbowasm_component_layout layout;
    turbowasm_status status;
    uint8_t *range = NULL;

    if (graph == NULL || value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = canonical_memory_validate(memory, &instance);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_component_canonical_layout(
        graph, type, memory->pointer_type, &layout);
    if (status != TURBOWASM_OK)
        return status;
    if (layout.alignment == 0u ||
        address % layout.alignment != 0u ||
        layout.size > (uint64_t)SIZE_MAX)
        return TURBOWASM_TRAPPED;
    status = turbowasm_instance_memory_bounds(
        instance, memory->memory_index,
        address, 0u, (size_t)layout.size, &range);
    if (status != TURBOWASM_OK)
        return status;

    return lower_value_inner(
        graph, type, memory, instance,
        address, 0u, value);
}
