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

void turbowasm_component_value_destroy(
    turbowasm_component_value *value) {
    uint64_t i;

    if (value == NULL)
        return;

    if (value->kind == TURBOWASM_COMPONENT_TYPE_STRING) {
        turbowasm_rt_free(value->as.string.data);
    } else if (value->kind == TURBOWASM_COMPONENT_TYPE_LIST) {
        for (i = 0u; i < value->as.list.count; ++i)
            turbowasm_component_value_destroy(
                &value->as.list.items[i]);
        turbowasm_rt_free(value->as.list.items);
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

static turbowasm_status lift_string(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    turbowasm_component_value *out) {
    uint64_t pointer;
    uint64_t length;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;
    uint8_t *copy = NULL;

    status = read_pointer(instance, memory, address, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    status = read_pointer(
        instance, memory, address + ptr_width, &length);
    if (status != TURBOWASM_OK)
        return status;

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

static turbowasm_status lower_string(
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    const turbowasm_component_value *value) {
    uint64_t pointer = 0u;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    turbowasm_status status;

    if (value == NULL ||
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

    status = write_pointer(instance, memory, address, pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    return write_pointer(
        instance, memory, address + ptr_width,
        (uint64_t)value->as.string.size);
}

static turbowasm_status lift_list(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    turbowasm_component_value *out) {
    turbowasm_component_layout layout;
    uint64_t pointer;
    uint64_t count;
    uint64_t bytes;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
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

    status = read_pointer(instance, memory, address, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    status = read_pointer(
        instance, memory, address + ptr_width, &count);
    if (status != TURBOWASM_OK)
        return status;

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

static turbowasm_status lower_list(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *type,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_instance_impl *instance,
    uint64_t address,
    uint32_t depth,
    const turbowasm_component_value *value) {
    turbowasm_component_layout layout;
    uint64_t count;
    uint64_t bytes;
    uint64_t pointer = 0u;
    uint64_t ptr_width = pointer_size(memory->pointer_type);
    uint64_t i;
    turbowasm_status status;

    if (type == NULL || value == NULL ||
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

    status = write_pointer(instance, memory, address, pointer);
    if (status != TURBOWASM_OK)
        return status;
    if (address > UINT64_MAX - ptr_width)
        return TURBOWASM_TRAPPED;
    return write_pointer(
        instance, memory, address + ptr_width, count);
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

    return TURBOWASM_UNSUPPORTED;
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
