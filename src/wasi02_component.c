#include "wasi02_component.h"

#include "runtime_alloc.h"

#include <stdio.h>
#include <string.h>

static const turbowasm_wasi02_interface_desc *
find_interface_by_component_name(turbowasm_component_name name) {
    size_t i;

    if (name.size != 0u && name.bytes == NULL)
        return NULL;

    for (i = 0u; i < turbowasm_wasi02_interface_count(); ++i) {
        const turbowasm_wasi02_interface_desc *iface =
            turbowasm_wasi02_interface_at(i);
        char expected[160];
        int written;

        if (iface == NULL)
            continue;
        written = snprintf(
            expected,
            sizeof(expected),
            "%s/%s@%u.%u.%u",
            iface->package_name,
            iface->interface_name,
            (unsigned)iface->version.major,
            (unsigned)iface->version.minor,
            (unsigned)iface->version.patch);
        if (written < 0 ||
            (size_t)written >= sizeof(expected))
            continue;
        if ((uint32_t)written == name.size &&
            (name.size == 0u ||
             memcmp(expected, name.bytes, name.size) == 0))
            return iface;
    }

    return NULL;
}

static const turbowasm_wasi02_function_desc *
find_function_by_component_name(
    const turbowasm_wasi02_interface_desc *iface,
    turbowasm_component_name name) {
    uint32_t i;

    if (iface == NULL ||
        (name.size != 0u && name.bytes == NULL))
        return NULL;

    for (i = 0u; i < iface->function_count; ++i) {
        const turbowasm_wasi02_function_desc *function =
            &iface->functions[i];
        size_t function_size = strlen(function->name);

        if (function_size == name.size &&
            (name.size == 0u ||
             memcmp(function->name, name.bytes, name.size) == 0))
            return function;
    }
    return NULL;
}

static const turbowasm_wasi02_type_desc *wasi_type_base(
    const turbowasm_wasi02_type_desc *type) {
    uint32_t depth = 0u;

    while (type != NULL &&
           type->kind == TURBOWASM_WASI02_TYPE_ALIAS) {
        if (++depth > 32u)
            return NULL;
        type = type->as.alias.target;
    }
    return type;
}

static const turbowasm_component_type *component_type_from_ref(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type *inline_storage) {
    if (graph == NULL || inline_storage == NULL)
        return NULL;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        memset(inline_storage, 0, sizeof(*inline_storage));
        inline_storage->kind = ref.as.inline_type;
        return inline_storage;
    }
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return NULL;
    return turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
}

static bool component_type_matches_wasi_depth(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref component_ref,
    const turbowasm_wasi02_type_desc *wasi_type,
    uint32_t depth) {
    turbowasm_component_type inline_storage;
    const turbowasm_component_type *component_type;
    const turbowasm_wasi02_type_desc *base;
    uint32_t i;

    if (depth > 64u)
        return false;
    base = wasi_type_base(wasi_type);
    component_type = component_type_from_ref(
        graph, component_ref, &inline_storage);
    if (base == NULL || component_type == NULL)
        return false;

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
            return component_type->kind ==
                   TURBOWASM_COMPONENT_TYPE_BOOL;
        case TURBOWASM_WASI02_TYPE_U8:
            return component_type->kind ==
                   TURBOWASM_COMPONENT_TYPE_U8;
        case TURBOWASM_WASI02_TYPE_U32:
            return component_type->kind ==
                   TURBOWASM_COMPONENT_TYPE_U32;
        case TURBOWASM_WASI02_TYPE_U64:
            return component_type->kind ==
                   TURBOWASM_COMPONENT_TYPE_U64;
        case TURBOWASM_WASI02_TYPE_STRING:
            return component_type->kind ==
                   TURBOWASM_COMPONENT_TYPE_STRING;

        case TURBOWASM_WASI02_TYPE_LIST:
            return component_type->kind ==
                       TURBOWASM_COMPONENT_TYPE_LIST &&
                   component_type_matches_wasi_depth(
                       graph,
                       component_type->as.list.element_type,
                       base->as.list.element,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (component_type->kind !=
                    TURBOWASM_COMPONENT_TYPE_TUPLE ||
                component_type->as.tuple.count !=
                    base->as.tuple.count)
                return false;
            for (i = 0u; i < base->as.tuple.count; ++i) {
                if (!component_type_matches_wasi_depth(
                        graph,
                        component_type->as.tuple.elements[i],
                        base->as.tuple.elements[i],
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_RECORD:
            if (component_type->kind !=
                    TURBOWASM_COMPONENT_TYPE_RECORD ||
                component_type->as.record.count !=
                    base->as.record.count)
                return false;
            for (i = 0u; i < base->as.record.count; ++i) {
                const turbowasm_component_record_field *field =
                    &component_type->as.record.fields[i];
                const turbowasm_wasi02_record_field *wasi_field =
                    &base->as.record.fields[i];
                size_t name_size;

                if (wasi_field->name == NULL)
                    return false;
                name_size = strlen(wasi_field->name);
                if (name_size != field->name_size ||
                    (name_size != 0u &&
                     (field->name == NULL ||
                      memcmp(
                          field->name,
                          wasi_field->name,
                          name_size) != 0)) ||
                    !component_type_matches_wasi_depth(
                        graph,
                        field->type,
                        wasi_field->type,
                        depth + 1u))
                    return false;
            }
            return true;

        case TURBOWASM_WASI02_TYPE_OPTION:
            return component_type->kind ==
                       TURBOWASM_COMPONENT_TYPE_OPTION &&
                   component_type_matches_wasi_depth(
                       graph,
                       component_type->as.option.payload,
                       base->as.option.payload,
                       depth + 1u);

        case TURBOWASM_WASI02_TYPE_RESULT:
            if (component_type->kind !=
                    TURBOWASM_COMPONENT_TYPE_RESULT ||
                component_type->as.result.has_ok !=
                    (base->as.result.ok != NULL) ||
                component_type->as.result.has_error !=
                    (base->as.result.error != NULL))
                return false;
            if (component_type->as.result.has_ok &&
                !component_type_matches_wasi_depth(
                    graph,
                    component_type->as.result.ok,
                    base->as.result.ok,
                    depth + 1u))
                return false;
            if (component_type->as.result.has_error &&
                !component_type_matches_wasi_depth(
                    graph,
                    component_type->as.result.error,
                    base->as.result.error,
                    depth + 1u))
                return false;
            return true;

        case TURBOWASM_WASI02_TYPE_ENUM:
        case TURBOWASM_WASI02_TYPE_FLAGS: {
            const turbowasm_component_label *labels;
            const char *const *wasi_labels;
            uint32_t count;
            if (base->kind == TURBOWASM_WASI02_TYPE_ENUM) {
                if (component_type->kind != TURBOWASM_COMPONENT_TYPE_ENUM)
                    return false;
                labels = component_type->as.enumeration.labels;
                count = component_type->as.enumeration.count;
                wasi_labels = base->as.enumeration.labels;
                if (count != base->as.enumeration.count)
                    return false;
            } else {
                if (component_type->kind != TURBOWASM_COMPONENT_TYPE_FLAGS)
                    return false;
                labels = component_type->as.flags.labels;
                count = component_type->as.flags.count;
                wasi_labels = base->as.flags.labels;
                if (count != base->as.flags.count)
                    return false;
            }
            if (labels == NULL || wasi_labels == NULL || count == 0u)
                return false;
            for (i = 0u; i < count; ++i) {
                size_t name_size;
                if (wasi_labels[i] == NULL)
                    return false;
                name_size = strlen(wasi_labels[i]);
                if (name_size != labels[i].name_size ||
                    labels[i].name == NULL ||
                    memcmp(labels[i].name, wasi_labels[i], name_size) != 0)
                    return false;
            }
            return true;
        }

        case TURBOWASM_WASI02_TYPE_UNIT:
        case TURBOWASM_WASI02_TYPE_RESOURCE:
        case TURBOWASM_WASI02_TYPE_ALIAS:
        default:
            return false;
    }
}

static bool binding_matches_descriptor(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_wasi02_function_desc *function) {
    const turbowasm_component_type *function_type;
    uint32_t i;

    if (graph == NULL || function == NULL)
        return false;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count !=
            function->param_count ||
        function_type->as.function.has_result !=
            (function->result != NULL))
        return false;

    for (i = 0u; i < function->param_count; ++i) {
        if (!component_type_matches_wasi_depth(
                graph,
                function_type->as.function.params[i],
                function->params[i].type,
                0u))
            return false;
    }

    if (function->result != NULL &&
        !component_type_matches_wasi_depth(
            graph,
            function_type->as.function.result,
            function->result,
            0u))
        return false;

    return true;
}

static bool wasi02_component_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_provider *provider =
        (turbowasm_wasi02_provider *)context;
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;

    if (provider == NULL || !provider->initialized)
        return false;

    iface = find_interface_by_component_name(instance_name);
    function = find_function_by_component_name(
        iface, function_name);
    return binding_matches_descriptor(
        graph, function_type, function);
}

static turbowasm_status copy_bytes(
    const uint8_t *data,
    size_t size,
    uint8_t **out) {
    uint8_t *copy = NULL;

    if (out == NULL || (size != 0u && data == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (size != 0u) {
        copy = (uint8_t *)turbowasm_rt_malloc(size);
        if (copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(copy, data, size);
    }
    *out = copy;
    return TURBOWASM_OK;
}

static turbowasm_status component_to_wasi_value(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_component_value *value,
    turbowasm_wasi02_value *out);

static turbowasm_status wasi_to_component_value(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    turbowasm_component_value *out);

static turbowasm_status component_sequence_to_wasi(
    const turbowasm_wasi02_type_desc *base,
    const turbowasm_component_value_list *sequence,
    turbowasm_wasi02_value_kind kind,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value *items = NULL;
    uint32_t expected_count = 0u;
    uint64_t i;
    turbowasm_status status;

    if (base == NULL || sequence == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (kind == TURBOWASM_WASI02_VALUE_TUPLE)
        expected_count = base->as.tuple.count;
    else if (kind == TURBOWASM_WASI02_VALUE_RECORD)
        expected_count = base->as.record.count;
    else
        return TURBOWASM_INVALID_ARGUMENT;

    if (sequence->count != expected_count ||
        (sequence->count != 0u && sequence->items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    if (sequence->count != 0u) {
        if (sequence->count >
            (uint64_t)SIZE_MAX / sizeof(*items))
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
            (size_t)sequence->count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < sequence->count; ++i) {
        const turbowasm_wasi02_type_desc *child =
            kind == TURBOWASM_WASI02_VALUE_TUPLE
                ? base->as.tuple.elements[i]
                : base->as.record.fields[i].type;
        status = component_to_wasi_value(
            child, &sequence->items[i], &items[i]);
        if (status != TURBOWASM_OK) {
            while (i != 0u) {
                --i;
                turbowasm_wasi02_value_destroy(&items[i]);
            }
            turbowasm_rt_free(items);
            return status;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_WASI02_VALUE_TUPLE) {
        out->as.tuple.items = items;
        out->as.tuple.count = (size_t)sequence->count;
    } else {
        out->as.record.items = items;
        out->as.record.count = (size_t)sequence->count;
    }
    return TURBOWASM_OK;
}

static turbowasm_status component_to_wasi_value(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_component_value *value,
    turbowasm_wasi02_value *out) {
    const turbowasm_wasi02_type_desc *base =
        wasi_type_base(type);
    turbowasm_status status;

    if (base == NULL || value == NULL || out == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    memset(out, 0, sizeof(*out));

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_BOOL)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_BOOL;
            out->as.boolean = value->as.boolean;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U8:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_U8)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_U8;
            out->as.u8 = value->as.u8;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U32:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_U32)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_U32;
            out->as.u32 = value->as.u32;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U64:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_U64)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_U64;
            out->as.u64 = value->as.u64;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_STRING:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_STRING)
                return TURBOWASM_TYPE_MISMATCH;
            status = copy_bytes(
                value->as.string.data,
                value->as.string.size,
                &out->as.string.data);
            if (status != TURBOWASM_OK)
                return status;
            out->kind = TURBOWASM_WASI02_VALUE_STRING;
            out->as.string.size = value->as.string.size;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_LIST: {
            uint64_t i;
            turbowasm_wasi02_value *items = NULL;

            if (value->kind != TURBOWASM_COMPONENT_TYPE_LIST ||
                (value->as.list.count != 0u &&
                 value->as.list.items == NULL))
                return TURBOWASM_TYPE_MISMATCH;
            if (value->as.list.count >
                (uint64_t)SIZE_MAX / sizeof(*items))
                return TURBOWASM_OUT_OF_MEMORY;
            if (value->as.list.count != 0u) {
                items = (turbowasm_wasi02_value *)
                    turbowasm_rt_calloc(
                        (size_t)value->as.list.count,
                        sizeof(*items));
                if (items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            for (i = 0u; i < value->as.list.count; ++i) {
                status = component_to_wasi_value(
                    base->as.list.element,
                    &value->as.list.items[i],
                    &items[i]);
                if (status != TURBOWASM_OK) {
                    while (i != 0u) {
                        --i;
                        turbowasm_wasi02_value_destroy(&items[i]);
                    }
                    turbowasm_rt_free(items);
                    return status;
                }
            }
            out->kind = TURBOWASM_WASI02_VALUE_LIST;
            out->as.list.items = items;
            out->as.list.count = (size_t)value->as.list.count;
            return TURBOWASM_OK;
        }

        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_TUPLE)
                return TURBOWASM_TYPE_MISMATCH;
            return component_sequence_to_wasi(
                base, &value->as.tuple,
                TURBOWASM_WASI02_VALUE_TUPLE, out);

        case TURBOWASM_WASI02_TYPE_RECORD:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_RECORD)
                return TURBOWASM_TYPE_MISMATCH;
            return component_sequence_to_wasi(
                base, &value->as.record,
                TURBOWASM_WASI02_VALUE_RECORD, out);

        case TURBOWASM_WASI02_TYPE_OPTION:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_OPTION ||
                value->as.option.case_index >= 2u)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_OPTION;
            out->as.option.has_value =
                value->as.option.case_index == 1u;
            if (!out->as.option.has_value)
                return value->as.option.payload == NULL
                    ? TURBOWASM_OK
                    : TURBOWASM_TYPE_MISMATCH;
            if (value->as.option.payload == NULL)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.option.value =
                (turbowasm_wasi02_value *)turbowasm_rt_calloc(
                    1u, sizeof(*out->as.option.value));
            if (out->as.option.value == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
            status = component_to_wasi_value(
                base->as.option.payload,
                value->as.option.payload,
                out->as.option.value);
            if (status != TURBOWASM_OK)
                turbowasm_wasi02_value_destroy(out);
            return status;

        case TURBOWASM_WASI02_TYPE_RESULT: {
            uint32_t case_index;
            const turbowasm_wasi02_type_desc *arm;

            if (value->kind != TURBOWASM_COMPONENT_TYPE_RESULT ||
                value->as.result.case_index >= 2u)
                return TURBOWASM_TYPE_MISMATCH;
            case_index = value->as.result.case_index;
            arm = case_index == 0u
                ? base->as.result.ok
                : base->as.result.error;
            out->kind = TURBOWASM_WASI02_VALUE_RESULT;
            out->as.result.is_error = case_index != 0u;

            if (arm == NULL)
                return value->as.result.payload == NULL
                    ? TURBOWASM_OK
                    : TURBOWASM_TYPE_MISMATCH;
            if (value->as.result.payload == NULL)
                return TURBOWASM_TYPE_MISMATCH;

            out->as.result.value =
                (turbowasm_wasi02_value *)turbowasm_rt_calloc(
                    1u, sizeof(*out->as.result.value));
            if (out->as.result.value == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
            status = component_to_wasi_value(
                arm,
                value->as.result.payload,
                out->as.result.value);
            if (status != TURBOWASM_OK)
                turbowasm_wasi02_value_destroy(out);
            return status;
        }

        case TURBOWASM_WASI02_TYPE_ENUM:
            if (value->kind != TURBOWASM_COMPONENT_TYPE_ENUM ||
                value->as.enum_index >= base->as.enumeration.count)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_ENUM;
            out->as.enum_index = value->as.enum_index;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_FLAGS: {
            uint32_t count = base->as.flags.count;
            uint32_t mask = count >= 32u
                ? UINT32_MAX
                : (count == 0u
                    ? 0u
                    : ((UINT32_C(1) << count) - UINT32_C(1)));
            if (value->kind != TURBOWASM_COMPONENT_TYPE_FLAGS ||
                count == 0u || count > 32u ||
                (value->as.flags & ~mask) != 0u)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_FLAGS;
            out->as.flags = value->as.flags;
            return TURBOWASM_OK;
        }

        case TURBOWASM_WASI02_TYPE_RESOURCE:
            if ((value->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
                 value->kind != TURBOWASM_COMPONENT_TYPE_BORROW) ||
                value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_WASI02_VALUE_RESOURCE;
            out->as.resource =
                (uint32_t)value->as.resource_rep.as.i32;
            return TURBOWASM_OK;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status wasi_sequence_to_component(
    const turbowasm_wasi02_type_desc *base,
    const turbowasm_wasi02_value_sequence *sequence,
    turbowasm_component_type_kind kind,
    turbowasm_component_value *out) {
    turbowasm_component_value *items = NULL;
    uint32_t expected_count;
    size_t i;
    turbowasm_status status;

    if (base == NULL || sequence == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    expected_count = kind == TURBOWASM_COMPONENT_TYPE_TUPLE
        ? base->as.tuple.count
        : base->as.record.count;
    if (sequence->count != expected_count ||
        (sequence->count != 0u && sequence->items == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    if (sequence->count != 0u) {
        if (sequence->count > SIZE_MAX / sizeof(*items))
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_component_value *)turbowasm_rt_calloc(
            sequence->count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < sequence->count; ++i) {
        const turbowasm_wasi02_type_desc *child =
            kind == TURBOWASM_COMPONENT_TYPE_TUPLE
                ? base->as.tuple.elements[i]
                : base->as.record.fields[i].type;
        status = wasi_to_component_value(
            child, &sequence->items[i], &items[i]);
        if (status != TURBOWASM_OK) {
            while (i != 0u) {
                --i;
                turbowasm_component_value_destroy(&items[i]);
            }
            turbowasm_rt_free(items);
            return status;
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_COMPONENT_TYPE_TUPLE) {
        out->as.tuple.items = items;
        out->as.tuple.count = sequence->count;
    } else {
        out->as.record.items = items;
        out->as.record.count = sequence->count;
    }
    return TURBOWASM_OK;
}

static turbowasm_status wasi_to_component_value(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    turbowasm_component_value *out) {
    const turbowasm_wasi02_type_desc *base =
        wasi_type_base(type);
    turbowasm_status status;

    if (base == NULL || value == NULL || out == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    memset(out, 0, sizeof(*out));

    switch (base->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
            if (value->kind != TURBOWASM_WASI02_VALUE_BOOL)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_BOOL;
            out->as.boolean = value->as.boolean;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U8:
            if (value->kind != TURBOWASM_WASI02_VALUE_U8)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_U8;
            out->as.u8 = value->as.u8;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U32:
            if (value->kind != TURBOWASM_WASI02_VALUE_U32)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_U32;
            out->as.u32 = value->as.u32;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_U64:
            if (value->kind != TURBOWASM_WASI02_VALUE_U64)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_U64;
            out->as.u64 = value->as.u64;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_STRING:
            if (value->kind != TURBOWASM_WASI02_VALUE_STRING)
                return TURBOWASM_TYPE_MISMATCH;
            status = copy_bytes(
                value->as.string.data,
                value->as.string.size,
                &out->as.string.data);
            if (status != TURBOWASM_OK)
                return status;
            out->kind = TURBOWASM_COMPONENT_TYPE_STRING;
            out->as.string.size = value->as.string.size;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_LIST: {
            turbowasm_component_value *items = NULL;
            size_t i;

            if (value->kind != TURBOWASM_WASI02_VALUE_LIST ||
                (value->as.list.count != 0u &&
                 value->as.list.items == NULL))
                return TURBOWASM_TYPE_MISMATCH;
            if (value->as.list.count != 0u) {
                if (value->as.list.count >
                    SIZE_MAX / sizeof(*items))
                    return TURBOWASM_OUT_OF_MEMORY;
                items = (turbowasm_component_value *)
                    turbowasm_rt_calloc(
                        value->as.list.count,
                        sizeof(*items));
                if (items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            for (i = 0u; i < value->as.list.count; ++i) {
                status = wasi_to_component_value(
                    base->as.list.element,
                    &value->as.list.items[i],
                    &items[i]);
                if (status != TURBOWASM_OK) {
                    while (i != 0u) {
                        --i;
                        turbowasm_component_value_destroy(&items[i]);
                    }
                    turbowasm_rt_free(items);
                    return status;
                }
            }
            out->kind = TURBOWASM_COMPONENT_TYPE_LIST;
            out->as.list.items = items;
            out->as.list.count = value->as.list.count;
            return TURBOWASM_OK;
        }

        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (value->kind != TURBOWASM_WASI02_VALUE_TUPLE)
                return TURBOWASM_TYPE_MISMATCH;
            return wasi_sequence_to_component(
                base, &value->as.tuple,
                TURBOWASM_COMPONENT_TYPE_TUPLE, out);

        case TURBOWASM_WASI02_TYPE_RECORD:
            if (value->kind != TURBOWASM_WASI02_VALUE_RECORD)
                return TURBOWASM_TYPE_MISMATCH;
            return wasi_sequence_to_component(
                base, &value->as.record,
                TURBOWASM_COMPONENT_TYPE_RECORD, out);

        case TURBOWASM_WASI02_TYPE_OPTION:
            if (value->kind != TURBOWASM_WASI02_VALUE_OPTION)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_OPTION;
            out->as.option.case_index =
                value->as.option.has_value ? 1u : 0u;
            if (!value->as.option.has_value)
                return value->as.option.value == NULL
                    ? TURBOWASM_OK
                    : TURBOWASM_TYPE_MISMATCH;
            if (value->as.option.value == NULL)
                return TURBOWASM_TYPE_MISMATCH;
            out->as.option.payload =
                (turbowasm_component_value *)turbowasm_rt_calloc(
                    1u, sizeof(*out->as.option.payload));
            if (out->as.option.payload == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
            status = wasi_to_component_value(
                base->as.option.payload,
                value->as.option.value,
                out->as.option.payload);
            if (status != TURBOWASM_OK)
                turbowasm_component_value_destroy(out);
            return status;

        case TURBOWASM_WASI02_TYPE_RESULT: {
            const turbowasm_wasi02_type_desc *arm;

            if (value->kind != TURBOWASM_WASI02_VALUE_RESULT)
                return TURBOWASM_TYPE_MISMATCH;
            arm = value->as.result.is_error
                ? base->as.result.error
                : base->as.result.ok;
            out->kind = TURBOWASM_COMPONENT_TYPE_RESULT;
            out->as.result.case_index =
                value->as.result.is_error ? 1u : 0u;

            if (arm == NULL)
                return value->as.result.value == NULL
                    ? TURBOWASM_OK
                    : TURBOWASM_TYPE_MISMATCH;
            if (value->as.result.value == NULL)
                return TURBOWASM_TYPE_MISMATCH;

            out->as.result.payload =
                (turbowasm_component_value *)turbowasm_rt_calloc(
                    1u, sizeof(*out->as.result.payload));
            if (out->as.result.payload == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
            status = wasi_to_component_value(
                arm,
                value->as.result.value,
                out->as.result.payload);
            if (status != TURBOWASM_OK)
                turbowasm_component_value_destroy(out);
            return status;
        }

        case TURBOWASM_WASI02_TYPE_ENUM:
            if (value->kind != TURBOWASM_WASI02_VALUE_ENUM ||
                base->as.enumeration.labels == NULL ||
                base->as.enumeration.count == 0u ||
                value->as.enum_index >= base->as.enumeration.count)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_ENUM;
            out->as.enum_index = value->as.enum_index;
            return TURBOWASM_OK;

        case TURBOWASM_WASI02_TYPE_FLAGS: {
            uint32_t count = base->as.flags.count;
            uint32_t mask = count == 32u
                ? UINT32_MAX
                : (count == 0u
                    ? 0u
                    : ((UINT32_C(1) << count) - UINT32_C(1)));
            if (value->kind != TURBOWASM_WASI02_VALUE_FLAGS ||
                base->as.flags.labels == NULL ||
                count == 0u || count > 32u ||
                (value->as.flags & ~mask) != 0u)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_FLAGS;
            out->as.flags = value->as.flags;
            return TURBOWASM_OK;
        }

        case TURBOWASM_WASI02_TYPE_RESOURCE:
            if (value->kind != TURBOWASM_WASI02_VALUE_RESOURCE)
                return TURBOWASM_TYPE_MISMATCH;
            out->kind = TURBOWASM_COMPONENT_TYPE_OWN;
            out->as.resource_rep.kind = TURBOWASM_VALUE_I32;
            out->as.resource_rep.as.i32 =
                (int32_t)value->as.resource;
            return TURBOWASM_OK;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

turbowasm_status turbowasm_wasi02_component_value_to_wasi(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_component_value *value,
    turbowasm_wasi02_value *out) {
    return component_to_wasi_value(type, value, out);
}

turbowasm_status turbowasm_wasi02_component_value_from_wasi(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    turbowasm_component_value *out) {
    return wasi_to_component_value(type, value, out);
}

static turbowasm_status wasi02_component_invoke(
    void *context,
    turbowasm_host_call *call,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    turbowasm_wasi02_provider *provider =
        (turbowasm_wasi02_provider *)context;
    const turbowasm_wasi02_interface_desc *iface;
    const turbowasm_wasi02_function_desc *function;
    const turbowasm_component_type *function_type;
    turbowasm_wasi02_value
        wasi_arguments[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {{0}};
    turbowasm_wasi02_value wasi_result = {0};
    turbowasm_runtime_scope scope;
    size_t i;
    turbowasm_status status = TURBOWASM_OK;

    (void)call;

    if (provider == NULL || !provider->initialized ||
        graph == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    iface = find_interface_by_component_name(instance_name);
    function = find_function_by_component_name(
        iface, function_name);
    if (!binding_matches_descriptor(
            graph, function_type_index, function))
        return TURBOWASM_TYPE_MISMATCH;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        argument_count != function->param_count ||
        argument_count > TURBOWASM_COMPONENT_MAX_FLAT_PARAMS ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;
    scope = turbowasm_runtime_scope_enter(
        &provider->runtime_config);

    for (i = 0u; i < argument_count; ++i) {
        status = component_to_wasi_value(
            function->params[i].type,
            &arguments[i],
            &wasi_arguments[i]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_wasi02_provider_call(
        provider,
        iface->package_name,
        iface->interface_name,
        function->name,
        wasi_arguments,
        argument_count,
        function->result != NULL ? &wasi_result : NULL);
    if (status != TURBOWASM_OK)
        goto done;

    if (function->result != NULL) {
        if (out_result == NULL) {
            status = TURBOWASM_INVALID_ARGUMENT;
            goto done;
        }
        status = wasi_to_component_value(
            function->result,
            &wasi_result,
            out_result);
    }

done:
    for (i = 0u; i < argument_count; ++i)
        turbowasm_wasi02_value_destroy(&wasi_arguments[i]);
    turbowasm_wasi02_value_destroy(&wasi_result);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

turbowasm_status turbowasm_wasi02_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_provider *provider) {
    turbowasm_component_exec_imports imports;

    if (exec == NULL || binary == NULL ||
        provider == NULL || !provider->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(&imports, 0, sizeof(imports));
    imports.context = provider;
    imports.can_bind = wasi02_component_can_bind;
    imports.invoke = wasi02_component_invoke;

    return turbowasm_component_exec_init_with_imports(
        exec, binary, &imports);
}
