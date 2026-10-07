#include "component_binary.h"

#include <turbowasm/module.h>

#include "reader.h"
#include "runtime_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    TW_COMPONENT_SECTION_CUSTOM = 0u,
    TW_COMPONENT_SECTION_CORE_MODULE = 1u,
    TW_COMPONENT_SECTION_MAX = 12u
};

static bool utf8_cont(uint8_t byte) {
    return (byte & UINT8_C(0xc0)) == UINT8_C(0x80);
}

static bool utf8_valid(const uint8_t *bytes, size_t size) {
    size_t i = 0u;

    if (size != 0u && bytes == NULL)
        return false;

    while (i < size) {
        uint8_t a = bytes[i++];

        if (a < UINT8_C(0x80))
            continue;

        if (a >= UINT8_C(0xc2) && a <= UINT8_C(0xdf)) {
            if (i >= size || !utf8_cont(bytes[i]))
                return false;
            ++i;
            continue;
        }

        if (a == UINT8_C(0xe0)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0xa0) ||
                bytes[i] > UINT8_C(0xbf) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if ((a >= UINT8_C(0xe1) && a <= UINT8_C(0xec)) ||
            (a >= UINT8_C(0xee) && a <= UINT8_C(0xef))) {
            if (i + 1u >= size ||
                !utf8_cont(bytes[i]) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == UINT8_C(0xed)) {
            if (i + 1u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x9f) ||
                !utf8_cont(bytes[i + 1u]))
                return false;
            i += 2u;
            continue;
        }

        if (a == UINT8_C(0xf0)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x90) ||
                bytes[i] > UINT8_C(0xbf) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a >= UINT8_C(0xf1) && a <= UINT8_C(0xf3)) {
            if (i + 2u >= size ||
                !utf8_cont(bytes[i]) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        if (a == UINT8_C(0xf4)) {
            if (i + 2u >= size ||
                bytes[i] < UINT8_C(0x80) ||
                bytes[i] > UINT8_C(0x8f) ||
                !utf8_cont(bytes[i + 1u]) ||
                !utf8_cont(bytes[i + 2u]))
                return false;
            i += 3u;
            continue;
        }

        return false;
    }

    return true;
}

static bool component_preamble(
    turbowasm_reader *reader) {
    uint32_t magic;
    uint8_t version_lo;
    uint8_t version_hi;
    uint8_t layer_lo;
    uint8_t layer_hi;

    return reader != NULL &&
           turbowasm_reader_u32le(reader, &magic) &&
           magic == UINT32_C(0x6d736100) &&
           turbowasm_reader_u8(reader, &version_lo) &&
           turbowasm_reader_u8(reader, &version_hi) &&
           turbowasm_reader_u8(reader, &layer_lo) &&
           turbowasm_reader_u8(reader, &layer_hi) &&
           version_lo == UINT8_C(0x0d) &&
           version_hi == 0u &&
           layer_lo == UINT8_C(0x01) &&
           layer_hi == 0u;
}

static bool reserve_array(
    void **items,
    uint32_t *capacity,
    uint32_t required,
    size_t item_size) {
    uint32_t next;
    void *grown;

    if (items == NULL || capacity == NULL || item_size == 0u)
        return false;
    if (required <= *capacity)
        return true;

    next = *capacity == 0u ? 4u : *capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((size_t)next > SIZE_MAX / item_size)
        return false;

    grown = turbowasm_rt_realloc(
        *items, (size_t)next * item_size);
    if (grown == NULL)
        return false;

    *items = grown;
    *capacity = next;
    return true;
}

static bool append_section(
    turbowasm_component_binary *component,
    uint8_t id,
    const uint8_t *payload,
    uint32_t size) {
    turbowasm_component_section *section;
    uint32_t required;

    if (component == NULL ||
        component->section_count == UINT32_MAX)
        return false;

    required = component->section_count + 1u;
    if (!reserve_array(
            (void **)&component->sections,
            &component->section_capacity,
            required,
            sizeof(*component->sections)))
        return false;

    section = &component->sections[component->section_count++];
    section->id = id;
    section->payload = payload;
    section->size = size;
    return true;
}

static bool append_core_module(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    uint32_t size) {
    turbowasm_component_core_module *module;
    uint32_t required;

    if (component == NULL ||
        component->core_module_count == UINT32_MAX)
        return false;

    required = component->core_module_count + 1u;
    if (!reserve_array(
            (void **)&component->core_modules,
            &component->core_module_capacity,
            required,
            sizeof(*component->core_modules)))
        return false;

    module =
        &component->core_modules[component->core_module_count++];
    module->bytes = bytes;
    module->size = size;
    return true;
}

static bool append_core_instance(
    turbowasm_component_binary *component,
    turbowasm_component_core_instance_def definition) {
    uint32_t required;

    if (component == NULL ||
        component->core_instance_count == UINT32_MAX)
        return false;
    required = component->core_instance_count + 1u;
    if (!reserve_array(
            (void **)&component->core_instances,
            &component->core_instance_capacity,
            required,
            sizeof(*component->core_instances)))
        return false;
    component->core_instances[component->core_instance_count++] =
        definition;
    return true;
}

static bool append_core_function_alias(
    turbowasm_component_binary *component,
    turbowasm_component_core_function_alias alias) {
    uint32_t required;

    if (component == NULL ||
        component->core_function_alias_count == UINT32_MAX)
        return false;
    required = component->core_function_alias_count + 1u;
    if (!reserve_array(
            (void **)&component->core_function_aliases,
            &component->core_function_alias_capacity,
            required,
            sizeof(*component->core_function_aliases)))
        return false;
    component->core_function_aliases[
        component->core_function_alias_count++] = alias;
    return true;
}

static bool append_core_memory_alias(
    turbowasm_component_binary *component,
    turbowasm_component_core_memory_alias alias) {
    uint32_t required;

    if (component == NULL ||
        component->core_memory_alias_count == UINT32_MAX)
        return false;
    required = component->core_memory_alias_count + 1u;
    if (!reserve_array(
            (void **)&component->core_memory_aliases,
            &component->core_memory_alias_capacity,
            required,
            sizeof(*component->core_memory_aliases)))
        return false;
    component->core_memory_aliases[
        component->core_memory_alias_count++] = alias;
    return true;
}

static bool append_canon_lift(
    turbowasm_component_binary *component,
    turbowasm_component_canon_lift lift) {
    uint32_t required;

    if (component == NULL ||
        component->canon_lift_count == UINT32_MAX)
        return false;
    required = component->canon_lift_count + 1u;
    if (!reserve_array(
            (void **)&component->canon_lifts,
            &component->canon_lift_capacity,
            required,
            sizeof(*component->canon_lifts)))
        return false;
    component->canon_lifts[component->canon_lift_count++] = lift;
    return true;
}

static bool append_canon_lower(
    turbowasm_component_binary *component,
    turbowasm_component_canon_lower lower) {
    uint32_t required;

    if (component == NULL ||
        component->canon_lower_count == UINT32_MAX)
        return false;
    required = component->canon_lower_count + 1u;
    if (!reserve_array(
            (void **)&component->canon_lowers,
            &component->canon_lower_capacity,
            required,
            sizeof(*component->canon_lowers)))
        return false;
    component->canon_lowers[component->canon_lower_count++] = lower;
    return true;
}

static bool append_resource_builtin(
    turbowasm_component_binary *component,
    turbowasm_component_resource_builtin builtin) {
    uint32_t required;

    if (component == NULL ||
        component->resource_builtin_count == UINT32_MAX)
        return false;
    required = component->resource_builtin_count + 1u;
    if (!reserve_array(
            (void **)&component->resource_builtins,
            &component->resource_builtin_capacity,
            required,
            sizeof(*component->resource_builtins)))
        return false;
    component->resource_builtins[
        component->resource_builtin_count++] = builtin;
    return true;
}

static bool append_component_instance(
    turbowasm_component_binary *component,
    turbowasm_component_instance_def definition) {
    uint32_t required;

    if (component == NULL ||
        component->component_instance_count == UINT32_MAX)
        return false;
    required = component->component_instance_count + 1u;
    if (!reserve_array(
            (void **)&component->component_instances,
            &component->component_instance_capacity,
            required,
            sizeof(*component->component_instances)))
        return false;
    component->component_instances[
        component->component_instance_count++] = definition;
    return true;
}

static bool append_component_function_alias(
    turbowasm_component_binary *component,
    turbowasm_component_function_alias alias) {
    uint32_t required;

    if (component == NULL ||
        component->component_function_alias_count == UINT32_MAX)
        return false;
    required = component->component_function_alias_count + 1u;
    if (!reserve_array(
            (void **)&component->component_function_aliases,
            &component->component_function_alias_capacity,
            required,
            sizeof(*component->component_function_aliases)))
        return false;
    component->component_function_aliases[
        component->component_function_alias_count++] = alias;
    return true;
}

static turbowasm_status validate_custom_section(
    turbowasm_reader section) {
    uint32_t name_size;
    turbowasm_reader name;

    if (!turbowasm_reader_uleb32(&section, &name_size) ||
        !turbowasm_reader_slice(&section, (size_t)name_size, &name))
        return TURBOWASM_MALFORMED_MODULE;

    if (!utf8_valid(name.cursor, turbowasm_reader_remaining(&name)))
        return TURBOWASM_MALFORMED_MODULE;

    return TURBOWASM_OK;
}

static turbowasm_status validate_core_module(
    const uint8_t *bytes,
    uint32_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_module module = {0};
    turbowasm_status status;

    status = turbowasm_module_load_borrowed_with_config(
        &module, bytes, (size_t)size, config);
    if (status == TURBOWASM_OK)
        turbowasm_module_destroy(&module);
    return status;
}

static bool primitive_kind_from_sleb(
    int32_t value,
    turbowasm_component_type_kind *out) {
    if (out == NULL)
        return false;

    switch (value) {
        case -1: *out = TURBOWASM_COMPONENT_TYPE_BOOL; return true;
        case -2: *out = TURBOWASM_COMPONENT_TYPE_S8; return true;
        case -3: *out = TURBOWASM_COMPONENT_TYPE_U8; return true;
        case -4: *out = TURBOWASM_COMPONENT_TYPE_S16; return true;
        case -5: *out = TURBOWASM_COMPONENT_TYPE_U16; return true;
        case -6: *out = TURBOWASM_COMPONENT_TYPE_S32; return true;
        case -7: *out = TURBOWASM_COMPONENT_TYPE_U32; return true;
        case -8: *out = TURBOWASM_COMPONENT_TYPE_S64; return true;
        case -9: *out = TURBOWASM_COMPONENT_TYPE_U64; return true;
        case -10: *out = TURBOWASM_COMPONENT_TYPE_F32; return true;
        case -11: *out = TURBOWASM_COMPONENT_TYPE_F64; return true;
        case -12: *out = TURBOWASM_COMPONENT_TYPE_CHAR; return true;
        case -13: *out = TURBOWASM_COMPONENT_TYPE_STRING; return true;
        default: return false;
    }
}

static turbowasm_status read_component_type_ref(
    turbowasm_reader *reader,
    uint32_t current_type_count,
    turbowasm_component_type_ref *out) {
    int32_t value;
    turbowasm_component_type_kind kind;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_sleb32(reader, &value))
        return TURBOWASM_MALFORMED_MODULE;

    if (value >= 0) {
        if ((uint32_t)value >= current_type_count)
            return TURBOWASM_MALFORMED_MODULE;
        *out = turbowasm_component_type_ref_indexed(
            (uint32_t)value);
        return TURBOWASM_OK;
    }

    if (!primitive_kind_from_sleb(value, &kind))
        return TURBOWASM_UNSUPPORTED;

    *out = turbowasm_component_type_ref_inline(kind);
    return TURBOWASM_OK;
}

static turbowasm_status read_component_name(
    turbowasm_reader *reader,
    turbowasm_component_name *out) {
    uint32_t size;
    turbowasm_reader bytes;

    if (reader == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &size) ||
        !turbowasm_reader_slice(reader, (size_t)size, &bytes))
        return TURBOWASM_MALFORMED_MODULE;
    if (!utf8_valid(bytes.cursor, turbowasm_reader_remaining(&bytes)))
        return TURBOWASM_MALFORMED_MODULE;

    out->bytes = bytes.cursor;
    out->size = size;
    return TURBOWASM_OK;
}

static turbowasm_status read_name_attributes(
    turbowasm_reader *reader,
    turbowasm_component_name *out) {
    uint8_t prefix;

    if (!turbowasm_reader_u8(reader, &prefix))
        return TURBOWASM_MALFORMED_MODULE;
    if (prefix == 2u)
        return TURBOWASM_UNSUPPORTED;
    if (prefix > 1u)
        return TURBOWASM_MALFORMED_MODULE;
    return read_component_name(reader, out);
}

static bool core_resource_rep_type_supported(uint8_t type) {
    switch (type) {
        case 0x7fu: /* i32 */
        case 0x7eu: /* i64 */
        case 0x7du: /* f32 */
        case 0x7cu: /* f64 */
        case 0x7bu: /* v128 */
        case 0x70u: /* funcref */
        case 0x6fu: /* externref */
        case 0x69u: /* exnref */
            return true;
        default:
            return false;
    }
}

static turbowasm_status decode_function_type_into_graph(
    turbowasm_reader *reader,
    turbowasm_component_type_graph *graph,
    uint32_t type_index) {
    uint32_t param_count;
    turbowasm_component_type_ref *params = NULL;
    turbowasm_component_type_ref result =
        turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_BOOL);
    bool has_result = false;
    uint32_t i;
    uint8_t result_tag;
    turbowasm_status status = TURBOWASM_OK;

    if (reader == NULL || graph == NULL ||
        type_index >= graph->count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &param_count))
        return TURBOWASM_MALFORMED_MODULE;

    if (param_count != 0u) {
        if ((size_t)param_count > SIZE_MAX / sizeof(*params))
            return TURBOWASM_OUT_OF_MEMORY;
        params = (turbowasm_component_type_ref *)turbowasm_rt_calloc(
            (size_t)param_count, sizeof(*params));
        if (params == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < param_count; ++i) {
        turbowasm_component_name label = {0};
        status = read_component_name(reader, &label);
        if (status != TURBOWASM_OK)
            goto done;
        status = read_component_type_ref(
            reader, type_index, &params[i]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    if (!turbowasm_reader_u8(reader, &result_tag)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    if (result_tag == 0u) {
        has_result = true;
        status = read_component_type_ref(
            reader, type_index, &result);
        if (status != TURBOWASM_OK)
            goto done;
    } else if (result_tag == 1u) {
        uint8_t empty;
        if (!turbowasm_reader_u8(reader, &empty) || empty != 0u) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }
    } else {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }

    if (!turbowasm_component_type_graph_define_function(
            graph,
            type_index,
            params,
            param_count,
            has_result,
            result))
        status = TURBOWASM_OUT_OF_MEMORY;

done:
    turbowasm_rt_free(params);
    return status;
}

static turbowasm_status decode_component_function_type(
    turbowasm_reader *reader,
    turbowasm_component_binary *component,
    uint32_t type_index) {
    if (component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return decode_function_type_into_graph(
        reader, &component->type_graph, type_index);
}

static turbowasm_status read_optional_component_type_ref(
    turbowasm_reader *reader,
    uint32_t current_type_count,
    bool *out_present,
    turbowasm_component_type_ref *out_ref) {
    uint8_t tag;

    if (reader == NULL || out_present == NULL || out_ref == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_u8(reader, &tag))
        return TURBOWASM_MALFORMED_MODULE;
    if (tag == 0u) {
        *out_present = false;
        memset(out_ref, 0, sizeof(*out_ref));
        return TURBOWASM_OK;
    }
    if (tag != 1u)
        return TURBOWASM_MALFORMED_MODULE;

    *out_present = true;
    return read_component_type_ref(
        reader, current_type_count, out_ref);
}

static turbowasm_status decode_composite_type_into_graph(
    turbowasm_reader *reader,
    turbowasm_component_type_graph *graph,
    uint32_t type_index,
    uint8_t opcode) {
    turbowasm_status status;

    if (reader == NULL || graph == NULL ||
        type_index >= graph->count)
        return TURBOWASM_INVALID_ARGUMENT;

    if (opcode == 0x70u) {
        turbowasm_component_type_ref element;
        status = read_component_type_ref(
            reader, type_index, &element);
        if (status != TURBOWASM_OK)
            return status;
        return turbowasm_component_type_graph_define_list_ref(
                   graph, type_index, element)
            ? TURBOWASM_OK
            : TURBOWASM_OUT_OF_MEMORY;
    }

    if (opcode == 0x72u) {
        turbowasm_component_record_field *fields = NULL;
        uint32_t count;
        uint32_t i;

        if (!turbowasm_reader_uleb32(reader, &count) || count == 0u)
            return TURBOWASM_MALFORMED_MODULE;
        if ((size_t)count > SIZE_MAX / sizeof(*fields))
            return TURBOWASM_OUT_OF_MEMORY;
        fields = (turbowasm_component_record_field *)
            turbowasm_rt_calloc((size_t)count, sizeof(*fields));
        if (fields == NULL)
            return TURBOWASM_OUT_OF_MEMORY;

        for (i = 0u; i < count; ++i) {
            turbowasm_component_name name = {0};
            status = read_component_name(reader, &name);
            if (status != TURBOWASM_OK)
                goto record_done;
            fields[i].name = name.bytes;
            fields[i].name_size = name.size;
            status = read_component_type_ref(
                reader, type_index, &fields[i].type);
            if (status != TURBOWASM_OK)
                goto record_done;
        }

        if (!turbowasm_component_type_graph_define_record(
                graph, type_index, fields, count))
            status = TURBOWASM_OUT_OF_MEMORY;
        else
            status = TURBOWASM_OK;

record_done:
        turbowasm_rt_free(fields);
        return status;
    }

    if (opcode == 0x6fu) {
        turbowasm_component_type_ref *elements = NULL;
        uint32_t count;
        uint32_t i;

        if (!turbowasm_reader_uleb32(reader, &count) || count == 0u)
            return TURBOWASM_MALFORMED_MODULE;
        if ((size_t)count > SIZE_MAX / sizeof(*elements))
            return TURBOWASM_OUT_OF_MEMORY;
        elements = (turbowasm_component_type_ref *)
            turbowasm_rt_calloc((size_t)count, sizeof(*elements));
        if (elements == NULL)
            return TURBOWASM_OUT_OF_MEMORY;

        for (i = 0u; i < count; ++i) {
            status = read_component_type_ref(
                reader, type_index, &elements[i]);
            if (status != TURBOWASM_OK) {
                turbowasm_rt_free(elements);
                return status;
            }
        }

        if (!turbowasm_component_type_graph_define_tuple(
                graph, type_index, elements, count))
            status = TURBOWASM_OUT_OF_MEMORY;
        else
            status = TURBOWASM_OK;
        turbowasm_rt_free(elements);
        return status;
    }

    if (opcode == 0x71u) {
        turbowasm_component_variant_case *cases = NULL;
        uint32_t count;
        uint32_t i;

        if (!turbowasm_reader_uleb32(reader, &count) || count == 0u)
            return TURBOWASM_MALFORMED_MODULE;
        if ((size_t)count > SIZE_MAX / sizeof(*cases))
            return TURBOWASM_OUT_OF_MEMORY;
        cases = (turbowasm_component_variant_case *)
            turbowasm_rt_calloc((size_t)count, sizeof(*cases));
        if (cases == NULL)
            return TURBOWASM_OUT_OF_MEMORY;

        for (i = 0u; i < count; ++i) {
            turbowasm_component_name name = {0};
            uint8_t refines = 0xffu;

            status = read_component_name(reader, &name);
            if (status != TURBOWASM_OK)
                goto variant_done;
            cases[i].name = name.bytes;
            cases[i].name_size = name.size;

            status = read_optional_component_type_ref(
                reader,
                type_index,
                &cases[i].has_payload,
                &cases[i].payload);
            if (status != TURBOWASM_OK)
                goto variant_done;

            if (!turbowasm_reader_u8(reader, &refines)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto variant_done;
            }
            if (refines != 0x00u) {
                status = TURBOWASM_UNSUPPORTED;
                goto variant_done;
            }
        }

        if (!turbowasm_component_variant_cases_valid(graph, cases, count))
            status = TURBOWASM_MALFORMED_MODULE;
        else if (!turbowasm_component_type_graph_define_variant(
                     graph, type_index, cases, count))
            status = TURBOWASM_OUT_OF_MEMORY;
        else
            status = TURBOWASM_OK;

variant_done:
        turbowasm_rt_free(cases);
        return status;
    }

    if (opcode == 0x6bu) {
        turbowasm_component_type_ref payload;
        status = read_component_type_ref(
            reader, type_index, &payload);
        if (status != TURBOWASM_OK)
            return status;
        return turbowasm_component_type_graph_define_option(
                   graph, type_index, payload)
            ? TURBOWASM_OK
            : TURBOWASM_OUT_OF_MEMORY;
    }

    if (opcode == 0x6au) {
        bool has_ok = false;
        bool has_error = false;
        turbowasm_component_type_ref ok;
        turbowasm_component_type_ref error;

        status = read_optional_component_type_ref(
            reader, type_index, &has_ok, &ok);
        if (status != TURBOWASM_OK)
            return status;
        status = read_optional_component_type_ref(
            reader, type_index, &has_error, &error);
        if (status != TURBOWASM_OK)
            return status;

        return turbowasm_component_type_graph_define_result(
                   graph,
                   type_index,
                   has_ok,
                   ok,
                   has_error,
                   error)
            ? TURBOWASM_OK
            : TURBOWASM_OUT_OF_MEMORY;
    }

    return TURBOWASM_UNSUPPORTED;
}

static turbowasm_status decode_label_type_into_graph(
    turbowasm_reader *reader,
    turbowasm_component_type_graph *graph,
    uint32_t type_index,
    turbowasm_component_type_kind kind) {
    turbowasm_component_label *labels = NULL;
    uint32_t count;
    uint32_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (reader == NULL || graph == NULL ||
        type_index >= graph->count ||
        (kind != TURBOWASM_COMPONENT_TYPE_ENUM &&
         kind != TURBOWASM_COMPONENT_TYPE_FLAGS))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &count) ||
        count == 0u ||
        (kind == TURBOWASM_COMPONENT_TYPE_FLAGS && count > 32u))
        return TURBOWASM_MALFORMED_MODULE;
    if ((size_t)count > SIZE_MAX / sizeof(*labels))
        return TURBOWASM_OUT_OF_MEMORY;

    labels = (turbowasm_component_label *)turbowasm_rt_calloc(
        count, sizeof(*labels));
    if (labels == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_name name = {0};
        status = read_component_name(reader, &name);
        if (status != TURBOWASM_OK)
            goto done;
        labels[i].name = name.bytes;
        labels[i].name_size = name.size;
    }

    if (!turbowasm_component_labels_valid(labels, count)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto done;
    }
    if (kind == TURBOWASM_COMPONENT_TYPE_ENUM) {
        if (!turbowasm_component_type_graph_define_enum(
                graph, type_index, labels, count))
            status = TURBOWASM_OUT_OF_MEMORY;
    } else {
        if (!turbowasm_component_type_graph_define_flags(
                graph, type_index, labels, count))
            status = TURBOWASM_OUT_OF_MEMORY;
    }

done:
    turbowasm_rt_free(labels);
    return status;
}


static turbowasm_status decode_core_instance_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_core_modules,
    uint32_t current_core_functions) {
    uint32_t count;
    uint32_t i;

    if (component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_core_instance_def definition = {0};
        uint8_t opcode;
        turbowasm_status status = TURBOWASM_OK;

        if (!turbowasm_reader_u8(&section, &opcode))
            return TURBOWASM_MALFORMED_MODULE;

        if (opcode == 0x00u) {
            uint32_t argument_count;
            uint32_t j;

            definition.kind =
                TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE;

            if (!turbowasm_reader_uleb32(
                    &section, &definition.module_index) ||
                definition.module_index >= current_core_modules ||
                !turbowasm_reader_uleb32(
                    &section, &argument_count))
                return TURBOWASM_MALFORMED_MODULE;

            if (argument_count != 0u) {
                if ((size_t)argument_count >
                    SIZE_MAX / sizeof(*definition.arguments))
                    return TURBOWASM_OUT_OF_MEMORY;
                definition.arguments =
                    (turbowasm_component_core_instantiate_arg *)
                        turbowasm_rt_calloc(
                            argument_count,
                            sizeof(*definition.arguments));
                if (definition.arguments == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            definition.argument_count = argument_count;

            for (j = 0u; j < argument_count; ++j) {
                uint8_t sort;
                status = read_component_name(
                    &section, &definition.arguments[j].name);
                if (status != TURBOWASM_OK)
                    goto fail_definition;
                if (!turbowasm_reader_u8(&section, &sort) ||
                    sort != 0x12u ||
                    !turbowasm_reader_uleb32(
                        &section,
                        &definition.arguments[j].instance_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail_definition;
                }
                /*
                 * Core instantiate arguments can only reference an already
                 * retained Core instance. This includes inline provider
                 * instances introduced earlier in the stream.
                 */
                if (definition.arguments[j].instance_index >=
                    component->core_instance_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail_definition;
                }
            }
        } else if (opcode == 0x01u) {
            uint32_t export_count;
            uint32_t j;

            definition.kind =
                TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE;

            if (!turbowasm_reader_uleb32(
                    &section, &export_count))
                return TURBOWASM_MALFORMED_MODULE;

            if (export_count != 0u) {
                if ((size_t)export_count >
                    SIZE_MAX / sizeof(*definition.exports))
                    return TURBOWASM_OUT_OF_MEMORY;
                definition.exports =
                    (turbowasm_component_core_inline_export *)
                        turbowasm_rt_calloc(
                            export_count,
                            sizeof(*definition.exports));
                if (definition.exports == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            definition.export_count = export_count;

            for (j = 0u; j < export_count; ++j) {
                uint8_t sort;
                status = read_component_name(
                    &section, &definition.exports[j].name);
                if (status != TURBOWASM_OK)
                    goto fail_definition;
                if (!turbowasm_reader_u8(&section, &sort) ||
                    sort != 0x00u ||
                    !turbowasm_reader_uleb32(
                        &section,
                        &definition.exports[j].item_index)) {
                    status = TURBOWASM_UNSUPPORTED;
                    goto fail_definition;
                }
                definition.exports[j].sort = sort;
                if (definition.exports[j].item_index >=
                    current_core_functions) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail_definition;
                }
            }
        } else {
            return TURBOWASM_MALFORMED_MODULE;
        }

        if (!append_core_instance(component, definition)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail_definition;
        }
        continue;

fail_definition:
        turbowasm_rt_free(definition.arguments);
        turbowasm_rt_free(definition.exports);
        return status;
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static bool clone_type_between_graphs(
    turbowasm_component_type_graph *destination_graph,
    uint32_t destination,
    const turbowasm_component_type_graph *source_graph,
    uint32_t source);

static const turbowasm_component_import *
find_imported_component_instance(
    const turbowasm_component_binary *component,
    uint32_t instance_index) {
    uint32_t i;

    if (component == NULL)
        return NULL;
    for (i = 0u; i < component->import_count; ++i) {
        const turbowasm_component_import *import_desc =
            &component->imports[i];
        if (import_desc->kind == TURBOWASM_COMPONENT_EXTERN_INSTANCE &&
            import_desc->item_index == instance_index)
            return import_desc;
    }
    return NULL;
}

static const turbowasm_component_instance_type_export *
find_instance_type_export(
    const turbowasm_component_instance_type *instance_type,
    turbowasm_component_instance_type_export_kind kind,
    turbowasm_component_name name) {
    uint32_t i;

    if (instance_type == NULL)
        return NULL;
    for (i = 0u; i < instance_type->export_count; ++i) {
        const turbowasm_component_instance_type_export *export_desc =
            &instance_type->exports[i];
        if (export_desc->kind == kind &&
            export_desc->name_size == name.size &&
            (name.size == 0u ||
             (export_desc->name != NULL &&
              name.bytes != NULL &&
              memcmp(export_desc->name, name.bytes, name.size) == 0)))
            return export_desc;
    }
    return NULL;
}

static turbowasm_status decode_alias_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_core_instances,
    uint32_t current_component_instances,
    uint32_t *next_core_function_index,
    uint32_t *next_core_memory_index,
    uint32_t *next_component_function_index,
    uint32_t *next_type_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL ||
        next_core_function_index == NULL ||
        next_core_memory_index == NULL ||
        next_component_function_index == NULL ||
        next_type_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        uint8_t sort;

        if (!turbowasm_reader_u8(&section, &sort))
            return TURBOWASM_MALFORMED_MODULE;

        if (sort == 0x00u) {
            uint8_t core_sort;
            uint8_t alias_kind;
            uint32_t instance_index;
            turbowasm_component_name name = {0};
            turbowasm_status status;

            if (!turbowasm_reader_u8(&section, &core_sort) ||
                !turbowasm_reader_u8(&section, &alias_kind))
                return TURBOWASM_MALFORMED_MODULE;

            if (alias_kind != 0x01u)
                return TURBOWASM_UNSUPPORTED;
            if (core_sort != 0x00u && core_sort != 0x02u)
                return TURBOWASM_UNSUPPORTED;

            if (!turbowasm_reader_uleb32(
                    &section, &instance_index) ||
                instance_index >= current_core_instances)
                return TURBOWASM_MALFORMED_MODULE;

            status = read_component_name(&section, &name);
            if (status != TURBOWASM_OK)
                return status;

            if (core_sort == 0x00u) {
                turbowasm_component_core_function_alias alias = {0};
                alias.instance_index = instance_index;
                alias.name = name;
                alias.core_function_index = *next_core_function_index;
                if (!append_core_function_alias(component, alias))
                    return TURBOWASM_OUT_OF_MEMORY;
                if (*next_core_function_index == UINT32_MAX)
                    return TURBOWASM_OUT_OF_MEMORY;
                ++*next_core_function_index;
            } else {
                turbowasm_component_core_memory_alias alias = {0};
                alias.instance_index = instance_index;
                alias.name = name;
                alias.core_memory_index = *next_core_memory_index;
                if (!append_core_memory_alias(component, alias))
                    return TURBOWASM_OUT_OF_MEMORY;
                if (*next_core_memory_index == UINT32_MAX)
                    return TURBOWASM_OUT_OF_MEMORY;
                ++*next_core_memory_index;
            }
        } else if (sort == 0x01u) {
            turbowasm_component_function_alias alias = {0};
            uint8_t alias_kind;
            turbowasm_status status;

            if (!turbowasm_reader_u8(&section, &alias_kind))
                return TURBOWASM_MALFORMED_MODULE;
            if (alias_kind != 0x00u)
                return TURBOWASM_UNSUPPORTED;

            if (!turbowasm_reader_uleb32(
                    &section, &alias.instance_index) ||
                alias.instance_index >= current_component_instances)
                return TURBOWASM_MALFORMED_MODULE;

            status = read_component_name(&section, &alias.name);
            if (status != TURBOWASM_OK)
                return status;

            alias.component_function_index =
                *next_component_function_index;
            if (!append_component_function_alias(component, alias))
                return TURBOWASM_OUT_OF_MEMORY;
            if (*next_component_function_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            ++*next_component_function_index;
        } else if (sort == 0x03u) {
            uint8_t alias_kind;
            uint32_t instance_index;
            turbowasm_component_name name = {0};
            const turbowasm_component_import *import_desc;
            const turbowasm_component_type *instance_wrapper;
            const turbowasm_component_instance_type_export *export_desc;
            turbowasm_status status;

            if (!turbowasm_reader_u8(&section, &alias_kind) ||
                alias_kind != 0x00u ||
                !turbowasm_reader_uleb32(
                    &section, &instance_index) ||
                instance_index >= current_component_instances)
                return TURBOWASM_UNSUPPORTED;

            status = read_component_name(&section, &name);
            if (status != TURBOWASM_OK)
                return status;

            import_desc = find_imported_component_instance(
                component, instance_index);
            if (import_desc == NULL ||
                import_desc->type_index >= *next_type_index ||
                *next_type_index >= component->type_graph.count)
                return TURBOWASM_UNSUPPORTED;

            instance_wrapper =
                turbowasm_component_type_graph_get(
                    &component->type_graph,
                    import_desc->type_index);
            if (instance_wrapper == NULL ||
                instance_wrapper->kind !=
                    TURBOWASM_COMPONENT_TYPE_INSTANCE ||
                instance_wrapper->as.instance == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            export_desc = find_instance_type_export(
                instance_wrapper->as.instance,
                TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE,
                name);
            if (export_desc == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            if (!clone_type_between_graphs(
                    &component->type_graph,
                    *next_type_index,
                    &instance_wrapper->as.instance->type_graph,
                    export_desc->type_index))
                return TURBOWASM_UNSUPPORTED;
            ++*next_type_index;
        } else {
            return TURBOWASM_UNSUPPORTED;
        }
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_canon_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_types,
    uint32_t *next_core_function_index,
    uint32_t current_core_memories,
    uint32_t *next_component_function_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL ||
        next_core_function_index == NULL ||
        next_component_function_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        uint8_t opcode;

        if (!turbowasm_reader_u8(&section, &opcode))
            return TURBOWASM_MALFORMED_MODULE;

        if (opcode == 0x00u) {
            turbowasm_component_canon_lift lift = {0};
            const turbowasm_component_type *type;
            uint8_t sort;
            uint32_t option_count;
            uint32_t option_index;
            bool string_encoding_seen = false;

            lift.string_encoding =
                TURBOWASM_COMPONENT_STRING_UTF8;

            if (!turbowasm_reader_u8(&section, &sort) ||
                sort != 0x00u)
                return TURBOWASM_UNSUPPORTED;

            if (!turbowasm_reader_uleb32(
                    &section, &lift.core_function_index) ||
                lift.core_function_index >=
                    *next_core_function_index ||
                !turbowasm_reader_uleb32(
                    &section, &option_count))
                return TURBOWASM_MALFORMED_MODULE;

            for (option_index = 0u;
                 option_index < option_count;
                 ++option_index) {
                uint8_t option;

                if (!turbowasm_reader_u8(&section, &option))
                    return TURBOWASM_MALFORMED_MODULE;

                switch (option) {
                    case 0x00u: /* string-encoding=utf8 */
                    case 0x01u: /* utf16 */
                    case 0x02u: /* latin1+utf16 */
                        if (string_encoding_seen)
                            return TURBOWASM_MALFORMED_MODULE;
                        string_encoding_seen = true;
                        lift.string_encoding = (turbowasm_component_string_encoding)option;
                        break;

                    case 0x03u: /* memory */
                        if (lift.has_memory ||
                            !turbowasm_reader_uleb32(
                                &section, &lift.memory_index) ||
                            lift.memory_index >=
                                current_core_memories)
                            return TURBOWASM_MALFORMED_MODULE;
                        lift.has_memory = true;
                        break;

                    case 0x04u: /* realloc */
                        if (lift.has_realloc ||
                            !turbowasm_reader_uleb32(
                                &section,
                                &lift.realloc_function_index) ||
                            lift.realloc_function_index >=
                                *next_core_function_index)
                            return TURBOWASM_MALFORMED_MODULE;
                        lift.has_realloc = true;
                        break;

                    case 0x05u: /* post-return */
                    case 0x06u: /* async */
                    case 0x07u: /* callback */
                        return TURBOWASM_UNSUPPORTED;

                    default:
                        return TURBOWASM_MALFORMED_MODULE;
                }
            }

            if (!turbowasm_reader_uleb32(
                    &section, &lift.type_index) ||
                lift.type_index >= current_types)
                return TURBOWASM_MALFORMED_MODULE;

            type = turbowasm_component_type_graph_get(
                &component->type_graph, lift.type_index);
            if (type == NULL ||
                type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
                return TURBOWASM_MALFORMED_MODULE;

            lift.component_function_index =
                *next_component_function_index;
            if (!append_canon_lift(component, lift))
                return TURBOWASM_OUT_OF_MEMORY;
            if (*next_component_function_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            ++*next_component_function_index;
        } else if (opcode == 0x01u) {
            turbowasm_component_canon_lower lower = {0};
            uint8_t sort;
            uint32_t option_count;
            uint32_t option_index;
            bool string_encoding_seen = false;

            lower.string_encoding =
                TURBOWASM_COMPONENT_STRING_UTF8;

            if (!turbowasm_reader_u8(&section, &sort) ||
                sort != 0x00u ||
                !turbowasm_reader_uleb32(
                    &section, &lower.component_function_index) ||
                lower.component_function_index >=
                    *next_component_function_index ||
                !turbowasm_reader_uleb32(
                    &section, &option_count))
                return TURBOWASM_MALFORMED_MODULE;

            for (option_index = 0u;
                 option_index < option_count;
                 ++option_index) {
                uint8_t option;

                if (!turbowasm_reader_u8(&section, &option))
                    return TURBOWASM_MALFORMED_MODULE;

                switch (option) {
                    case 0x00u: /* string-encoding=utf8 */
                    case 0x01u: /* utf16 */
                    case 0x02u: /* latin1+utf16 */
                        if (string_encoding_seen)
                            return TURBOWASM_MALFORMED_MODULE;
                        string_encoding_seen = true;
                        lower.string_encoding = (turbowasm_component_string_encoding)option;
                        break;

                    case 0x03u: /* memory */
                        if (lower.has_memory ||
                            !turbowasm_reader_uleb32(
                                &section, &lower.memory_index) ||
                            lower.memory_index >= current_core_memories)
                            return TURBOWASM_MALFORMED_MODULE;
                        lower.has_memory = true;
                        break;

                    case 0x04u: /* realloc */
                        if (lower.has_realloc ||
                            !turbowasm_reader_uleb32(
                                &section,
                                &lower.realloc_function_index) ||
                            lower.realloc_function_index >=
                                *next_core_function_index)
                            return TURBOWASM_MALFORMED_MODULE;
                        lower.has_realloc = true;
                        break;

                    case 0x05u: /* post-return */
                    case 0x06u: /* async */
                    case 0x07u: /* callback */
                        return TURBOWASM_UNSUPPORTED;

                    default:
                        return TURBOWASM_MALFORMED_MODULE;
                }
            }

            if (lower.has_realloc && !lower.has_memory)
                return TURBOWASM_MALFORMED_MODULE;

            lower.core_function_index =
                *next_core_function_index;
            if (!append_canon_lower(component, lower))
                return TURBOWASM_OUT_OF_MEMORY;
            if (*next_core_function_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            ++*next_core_function_index;
        } else if (opcode == 0x02u ||
                   opcode == 0x03u ||
                   opcode == 0x04u) {
            turbowasm_component_resource_builtin builtin = {0};
            const turbowasm_component_type *resource_type;

            if (!turbowasm_reader_uleb32(
                    &section, &builtin.resource_type) ||
                builtin.resource_type >= current_types)
                return TURBOWASM_MALFORMED_MODULE;

            resource_type = turbowasm_component_type_graph_get(
                &component->type_graph, builtin.resource_type);
            if (resource_type == NULL ||
                resource_type->kind !=
                    TURBOWASM_COMPONENT_TYPE_RESOURCE)
                return TURBOWASM_MALFORMED_MODULE;

            builtin.kind =
                opcode == 0x02u
                    ? TURBOWASM_COMPONENT_RESOURCE_BUILTIN_NEW
                    : (opcode == 0x03u
                        ? TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP
                        : TURBOWASM_COMPONENT_RESOURCE_BUILTIN_REP);
            builtin.core_function_index =
                *next_core_function_index;

            if (!append_resource_builtin(component, builtin))
                return TURBOWASM_OUT_OF_MEMORY;
            if (*next_core_function_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            ++*next_core_function_index;
        } else {
            return TURBOWASM_UNSUPPORTED;
        }
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_component_instance_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_component_functions,
    uint32_t *next_component_instance_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL || next_component_instance_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_instance_def definition = {0};
        uint8_t opcode;
        uint32_t export_count;
        uint32_t j;
        turbowasm_status status = TURBOWASM_OK;

        if (!turbowasm_reader_u8(&section, &opcode))
            return TURBOWASM_MALFORMED_MODULE;

        /*
         * C5c3 intentionally supports only inline Component instances.
         * Nested Component instantiation needs recursive execution/import
         * binding and therefore fails closed for now.
         */
        if (opcode == 0x00u)
            return TURBOWASM_UNSUPPORTED;
        if (opcode != 0x01u)
            return TURBOWASM_MALFORMED_MODULE;

        if (!turbowasm_reader_uleb32(&section, &export_count))
            return TURBOWASM_MALFORMED_MODULE;

        if (export_count != 0u) {
            if ((size_t)export_count >
                SIZE_MAX / sizeof(*definition.exports))
                return TURBOWASM_OUT_OF_MEMORY;
            definition.exports =
                (turbowasm_component_inline_export *)
                    turbowasm_rt_calloc(
                        export_count,
                        sizeof(*definition.exports));
            if (definition.exports == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }
        definition.export_count = export_count;

        for (j = 0u; j < export_count; ++j) {
            uint8_t sort;
            status = read_name_attributes(
                &section, &definition.exports[j].name);
            if (status != TURBOWASM_OK)
                goto fail_definition;

            if (!turbowasm_reader_u8(&section, &sort)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail_definition;
            }
            if (sort != 0x01u) {
                status = TURBOWASM_UNSUPPORTED;
                goto fail_definition;
            }

            definition.exports[j].kind =
                TURBOWASM_COMPONENT_EXTERN_FUNCTION;
            if (!turbowasm_reader_uleb32(
                    &section,
                    &definition.exports[j].item_index) ||
                definition.exports[j].item_index >=
                    current_component_functions) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail_definition;
            }
        }

        if (*next_component_instance_index == UINT32_MAX) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail_definition;
        }
        definition.component_instance_index =
            *next_component_instance_index;
        if (!append_component_instance(component, definition)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail_definition;
        }
        ++*next_component_instance_index;
        continue;

fail_definition:
        turbowasm_rt_free(definition.exports);
        return status;
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static bool clone_type_between_graphs(
    turbowasm_component_type_graph *destination_graph,
    uint32_t destination,
    const turbowasm_component_type_graph *source_graph,
    uint32_t source) {
    const turbowasm_component_type *type;
    bool same_graph;

    if (destination_graph == NULL || source_graph == NULL)
        return false;
    type = turbowasm_component_type_graph_get(source_graph, source);
    if (type == NULL)
        return false;
    same_graph = destination_graph == source_graph;

    if (type->kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
        type->kind <= TURBOWASM_COMPONENT_TYPE_CHAR)
        return turbowasm_component_type_graph_define_scalar(
            destination_graph, destination, type->kind);

    switch (type->kind) {
        case TURBOWASM_COMPONENT_TYPE_STRING:
            return turbowasm_component_type_graph_define_string(
                destination_graph, destination);

        case TURBOWASM_COMPONENT_TYPE_RESOURCE:
            return turbowasm_component_type_graph_define_resource_alias(
                destination_graph,
                destination,
                type->as.resource.identity,
                type->as.resource.rep_type,
                type->as.resource.has_destructor,
                type->as.resource.destructor_index);

        case TURBOWASM_COMPONENT_TYPE_LIST:
            return same_graph &&
                turbowasm_component_type_graph_define_list_ref(
                    destination_graph,
                    destination,
                    type->as.list.element_type);

        case TURBOWASM_COMPONENT_TYPE_RECORD:
            return same_graph &&
                turbowasm_component_type_graph_define_record(
                    destination_graph,
                    destination,
                    type->as.record.fields,
                    type->as.record.count);

        case TURBOWASM_COMPONENT_TYPE_TUPLE:
            return same_graph &&
                turbowasm_component_type_graph_define_tuple(
                    destination_graph,
                    destination,
                    type->as.tuple.elements,
                    type->as.tuple.count);

        case TURBOWASM_COMPONENT_TYPE_VARIANT:
            return same_graph &&
                turbowasm_component_type_graph_define_variant(
                    destination_graph,
                    destination,
                    type->as.variant.cases,
                    type->as.variant.count);

        case TURBOWASM_COMPONENT_TYPE_OPTION:
            return same_graph &&
                turbowasm_component_type_graph_define_option(
                    destination_graph,
                    destination,
                    type->as.option.payload);

        case TURBOWASM_COMPONENT_TYPE_RESULT:
            return same_graph &&
                turbowasm_component_type_graph_define_result(
                    destination_graph,
                    destination,
                    type->as.result.has_ok,
                    type->as.result.ok,
                    type->as.result.has_error,
                    type->as.result.error);

        case TURBOWASM_COMPONENT_TYPE_ENUM:
            return turbowasm_component_type_graph_define_enum(
                destination_graph,
                destination,
                type->as.enumeration.labels,
                type->as.enumeration.count);

        case TURBOWASM_COMPONENT_TYPE_FLAGS:
            return turbowasm_component_type_graph_define_flags(
                destination_graph,
                destination,
                type->as.flags.labels,
                type->as.flags.count);

        case TURBOWASM_COMPONENT_TYPE_FUNCTION:
            return same_graph &&
                turbowasm_component_type_graph_define_function(
                    destination_graph,
                    destination,
                    type->as.function.params,
                    type->as.function.param_count,
                    type->as.function.has_result,
                    type->as.function.result);

        case TURBOWASM_COMPONENT_TYPE_OWN:
        case TURBOWASM_COMPONENT_TYPE_BORROW:
            return same_graph &&
                turbowasm_component_type_graph_define_handle(
                    destination_graph,
                    destination,
                    type->kind,
                    type->as.handle.resource_type);

        default:
            return false;
    }
}

static bool clone_local_type(
    turbowasm_component_type_graph *graph,
    uint32_t destination,
    uint32_t source) {
    if (graph == NULL || source >= destination)
        return false;
    return clone_type_between_graphs(
        graph, destination, graph, source);
}

static turbowasm_status decode_flat_instance_type(
    turbowasm_reader *reader,
    turbowasm_component_type_graph *outer_graph,
    uint32_t outer_type_index,
    uint64_t *next_resource_identity) {
    turbowasm_component_instance_type *instance_type = NULL;
    uint32_t declaration_count;
    uint32_t next_local_type = 0u;
    uint32_t export_capacity = 0u;
    uint32_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (reader == NULL || outer_graph == NULL ||
        next_resource_identity == NULL ||
        outer_type_index >= outer_graph->count)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(reader, &declaration_count))
        return TURBOWASM_MALFORMED_MODULE;

    instance_type = (turbowasm_component_instance_type *)
        turbowasm_rt_calloc(1u, sizeof(*instance_type));
    if (instance_type == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    /*
     * Every supported declaration introduces at most one local type index.
     * Allocate the declaration count as stable construction capacity, then
     * trim the visible graph count to the indices actually introduced.
     */
    if (!turbowasm_component_type_graph_allocate(
            &instance_type->type_graph, declaration_count)) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto fail;
    }

    for (i = 0u; i < declaration_count; ++i) {
        uint8_t declaration;

        if (!turbowasm_reader_u8(reader, &declaration)) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        if (declaration == 0x01u) {
            uint8_t opcode;
            turbowasm_component_type_kind kind;

            if (next_local_type >= declaration_count ||
                !turbowasm_reader_u8(reader, &opcode)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            if (opcode >= 0x73u && opcode <= 0x7fu) {
                int32_t sleb = (int32_t)opcode - 0x80;
                if (!primitive_kind_from_sleb(sleb, &kind)) {
                    status = TURBOWASM_UNSUPPORTED;
                    goto fail;
                }
                if (kind == TURBOWASM_COMPONENT_TYPE_STRING) {
                    if (!turbowasm_component_type_graph_define_string(
                            &instance_type->type_graph,
                            next_local_type)) {
                        status = TURBOWASM_OUT_OF_MEMORY;
                        goto fail;
                    }
                } else if (!turbowasm_component_type_graph_define_scalar(
                               &instance_type->type_graph,
                               next_local_type,
                               kind)) {
                    status = TURBOWASM_OUT_OF_MEMORY;
                    goto fail;
                }
            } else if (opcode == 0x40u) {
                status = decode_function_type_into_graph(
                    reader,
                    &instance_type->type_graph,
                    next_local_type);
                if (status != TURBOWASM_OK)
                    goto fail;
            } else if (opcode == 0x70u ||
                       opcode == 0x72u ||
                       opcode == 0x71u ||
                       opcode == 0x6fu ||
                       opcode == 0x6bu ||
                       opcode == 0x6au) {
                status = decode_composite_type_into_graph(
                    reader,
                    &instance_type->type_graph,
                    next_local_type,
                    opcode);
                if (status != TURBOWASM_OK)
                    goto fail;
            } else if (opcode == 0x6du || opcode == 0x6eu) {
                status = decode_label_type_into_graph(
                    reader,
                    &instance_type->type_graph,
                    next_local_type,
                    opcode == 0x6du
                        ? TURBOWASM_COMPONENT_TYPE_ENUM
                        : TURBOWASM_COMPONENT_TYPE_FLAGS);
                if (status != TURBOWASM_OK)
                    goto fail;
            } else if (opcode == 0x69u || opcode == 0x68u) {
                uint32_t resource_index;
                const turbowasm_component_type *resource;

                if (!turbowasm_reader_uleb32(
                        reader, &resource_index) ||
                    resource_index >= next_local_type) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }
                resource = turbowasm_component_type_graph_get(
                    &instance_type->type_graph, resource_index);
                if (resource == NULL ||
                    resource->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
                    !turbowasm_component_type_graph_define_handle(
                        &instance_type->type_graph,
                        next_local_type,
                        opcode == 0x69u
                            ? TURBOWASM_COMPONENT_TYPE_OWN
                            : TURBOWASM_COMPONENT_TYPE_BORROW,
                        resource_index)) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }
            } else {
                /*
                 * Resources/variants/flags/enums/stream/future remain outside
                 * this retained composite slice.
                 */
                status = TURBOWASM_UNSUPPORTED;
                goto fail;
            }
            ++next_local_type;
        } else if (declaration == 0x04u) {
            turbowasm_component_name name = {0};
            uint8_t external_kind;

            status = read_name_attributes(reader, &name);
            if (status != TURBOWASM_OK)
                goto fail;
            if (!turbowasm_reader_u8(reader, &external_kind)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            if (external_kind == 0x01u) {
                uint32_t function_type;
                turbowasm_component_instance_type_export *export_desc;

                if (!turbowasm_reader_uleb32(
                        reader, &function_type) ||
                    function_type >= next_local_type) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }
                {
                    const turbowasm_component_type *type =
                        turbowasm_component_type_graph_get(
                            &instance_type->type_graph,
                            function_type);
                    if (type == NULL ||
                        type->kind !=
                            TURBOWASM_COMPONENT_TYPE_FUNCTION) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        goto fail;
                    }
                }

                if (!reserve_array(
                        (void **)&instance_type->exports,
                        &export_capacity,
                        instance_type->export_count + 1u,
                        sizeof(*instance_type->exports))) {
                    status = TURBOWASM_OUT_OF_MEMORY;
                    goto fail;
                }
                export_desc =
                    &instance_type->exports[
                        instance_type->export_count++];
                export_desc->name = name.bytes;
                export_desc->name_size = name.size;
                export_desc->kind =
                    TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION;
                export_desc->type_index = function_type;
            } else if (external_kind == 0x03u) {
                uint8_t bound;
                uint32_t exported_type;
                turbowasm_component_instance_type_export *export_desc;

                if (!turbowasm_reader_u8(reader, &bound) ||
                    next_local_type >= declaration_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }

                exported_type = next_local_type;
                if (bound == 0x00u) {
                    uint32_t source_type;
                    if (!turbowasm_reader_uleb32(
                            reader, &source_type) ||
                        source_type >= next_local_type) {
                        status = TURBOWASM_MALFORMED_MODULE;
                        goto fail;
                    }
                    if (!clone_local_type(
                            &instance_type->type_graph,
                            next_local_type,
                            source_type)) {
                        status = TURBOWASM_UNSUPPORTED;
                        goto fail;
                    }
                } else if (bound == 0x01u) {
                    if (*next_resource_identity == 0u ||
                        *next_resource_identity == UINT64_MAX ||
                        !turbowasm_component_type_graph_define_resource_full(
                            &instance_type->type_graph,
                            next_local_type,
                            *next_resource_identity,
                            0x7fu,
                            false,
                            UINT32_MAX)) {
                        status = TURBOWASM_OUT_OF_MEMORY;
                        goto fail;
                    }
                    ++*next_resource_identity;
                } else {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }
                ++next_local_type;

                if (!reserve_array(
                        (void **)&instance_type->exports,
                        &export_capacity,
                        instance_type->export_count + 1u,
                        sizeof(*instance_type->exports))) {
                    status = TURBOWASM_OUT_OF_MEMORY;
                    goto fail;
                }
                export_desc =
                    &instance_type->exports[
                        instance_type->export_count++];
                export_desc->name = name.bytes;
                export_desc->name_size = name.size;
                export_desc->kind =
                    TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE;
                export_desc->type_index = exported_type;
            } else {
                status = TURBOWASM_UNSUPPORTED;
                goto fail;
            }
        } else if (declaration == 0x02u) {
            uint8_t sort;
            uint8_t alias_kind;
            uint32_t component_depth;
            uint32_t source_type;

            if (next_local_type >= declaration_count ||
                !turbowasm_reader_u8(reader, &sort) ||
                sort != 0x03u ||
                !turbowasm_reader_u8(reader, &alias_kind) ||
                alias_kind != 0x02u ||
                !turbowasm_reader_uleb32(
                    reader, &component_depth) ||
                component_depth != 1u ||
                !turbowasm_reader_uleb32(
                    reader, &source_type) ||
                source_type >= outer_type_index) {
                status = TURBOWASM_UNSUPPORTED;
                goto fail;
            }

            if (!clone_type_between_graphs(
                    &instance_type->type_graph,
                    next_local_type,
                    outer_graph,
                    source_type)) {
                status = TURBOWASM_UNSUPPORTED;
                goto fail;
            }
            ++next_local_type;
        } else {
            /* core type declarations remain outside the W3 synchronous subset. */
            status = TURBOWASM_UNSUPPORTED;
            goto fail;
        }
    }

    instance_type->type_graph.count = next_local_type;
    if (!turbowasm_component_type_graph_define_instance(
            outer_graph, outer_type_index, instance_type)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto fail;
    }

    return TURBOWASM_OK;

fail:
    if (instance_type != NULL) {
        /*
         * Restore construction count so every defined slot is visited by the
         * recursive destroy path after a partial parse.
         */
        if (instance_type->type_graph.types != NULL &&
            instance_type->type_graph.count < next_local_type)
            instance_type->type_graph.count = next_local_type;
        turbowasm_component_type_graph_destroy(
            &instance_type->type_graph);
        turbowasm_rt_free(instance_type->exports);
        turbowasm_rt_free(instance_type);
    }
    return status;
}

static turbowasm_status decode_component_type_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_core_functions,
    uint32_t *next_type_index,
    uint64_t *next_resource_identity) {
    uint32_t count;
    uint32_t i;

    if (component == NULL || next_type_index == NULL ||
        next_resource_identity == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        uint8_t opcode;
        uint32_t id = *next_type_index;
        turbowasm_status status;

        if (id >= component->type_graph.count ||
            !turbowasm_reader_u8(&section, &opcode))
            return TURBOWASM_MALFORMED_MODULE;

        if (opcode >= 0x73u && opcode <= 0x7fu) {
            int32_t sleb = (int32_t)opcode - 0x80;
            turbowasm_component_type_kind kind;
            if (!primitive_kind_from_sleb(sleb, &kind))
                return TURBOWASM_UNSUPPORTED;
            if (kind == TURBOWASM_COMPONENT_TYPE_STRING) {
                if (!turbowasm_component_type_graph_define_string(
                        &component->type_graph, id))
                    return TURBOWASM_OUT_OF_MEMORY;
            } else if (!turbowasm_component_type_graph_define_scalar(
                           &component->type_graph, id, kind)) {
                return TURBOWASM_OUT_OF_MEMORY;
            }
        } else if (opcode == 0x70u ||
                   opcode == 0x72u ||
                   opcode == 0x71u ||
                   opcode == 0x6fu ||
                   opcode == 0x6bu ||
                   opcode == 0x6au) {
            status = decode_composite_type_into_graph(
                &section,
                &component->type_graph,
                id,
                opcode);
            if (status != TURBOWASM_OK)
                return status;
        } else if (opcode == 0x6du || opcode == 0x6eu) {
            status = decode_label_type_into_graph(
                &section,
                &component->type_graph,
                id,
                opcode == 0x6du
                    ? TURBOWASM_COMPONENT_TYPE_ENUM
                    : TURBOWASM_COMPONENT_TYPE_FLAGS);
            if (status != TURBOWASM_OK)
                return status;
        } else if (opcode == 0x69u || opcode == 0x68u) {
            uint32_t resource_index;
            const turbowasm_component_type *resource;
            if (!turbowasm_reader_uleb32(
                    &section, &resource_index) ||
                resource_index >= id)
                return TURBOWASM_MALFORMED_MODULE;
            resource = turbowasm_component_type_graph_get(
                &component->type_graph, resource_index);
            if (resource == NULL ||
                resource->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
                return TURBOWASM_MALFORMED_MODULE;
            if (!turbowasm_component_type_graph_define_handle(
                    &component->type_graph, id,
                    opcode == 0x69u
                        ? TURBOWASM_COMPONENT_TYPE_OWN
                        : TURBOWASM_COMPONENT_TYPE_BORROW,
                    resource_index))
                return TURBOWASM_OUT_OF_MEMORY;
        } else if (opcode == 0x3fu) {
            uint8_t rep_type;
            uint8_t dtor_flag;
            uint32_t dtor_index = UINT32_MAX;

            if (!turbowasm_reader_u8(&section, &rep_type))
                return TURBOWASM_MALFORMED_MODULE;
            if (!core_resource_rep_type_supported(rep_type))
                return TURBOWASM_UNSUPPORTED;
            if (!turbowasm_reader_u8(&section, &dtor_flag))
                return TURBOWASM_MALFORMED_MODULE;
            if (dtor_flag > 1u)
                return TURBOWASM_MALFORMED_MODULE;
            if (dtor_flag != 0u) {
                if (!turbowasm_reader_uleb32(
                        &section, &dtor_index) ||
                    dtor_index >= current_core_functions)
                    return TURBOWASM_MALFORMED_MODULE;
            }

            if (!turbowasm_component_type_graph_define_resource_full(
                    &component->type_graph,
                    id,
                    (uint64_t)id + UINT64_C(1),
                    rep_type,
                    dtor_flag != 0u,
                    dtor_index))
                return TURBOWASM_OUT_OF_MEMORY;
        } else if (opcode == 0x40u) {
            status = decode_component_function_type(
                &section, component, id);
            if (status != TURBOWASM_OK)
                return status;
        } else if (opcode == 0x42u) {
            status = decode_flat_instance_type(
                &section, &component->type_graph, id,
                next_resource_identity);
            if (status != TURBOWASM_OK)
                return status;
        } else if (opcode == 0x43u) {
            return TURBOWASM_UNSUPPORTED;
        } else {
            return TURBOWASM_UNSUPPORTED;
        }

        ++*next_type_index;
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static bool append_import(
    turbowasm_component_binary *component,
    turbowasm_component_import import_desc) {
    uint32_t required;

    if (component == NULL || component->import_count == UINT32_MAX)
        return false;
    required = component->import_count + 1u;
    if (!reserve_array(
            (void **)&component->imports,
            &component->import_capacity,
            required,
            sizeof(*component->imports)))
        return false;
    component->imports[component->import_count++] = import_desc;
    return true;
}

static bool append_export(
    turbowasm_component_binary *component,
    turbowasm_component_export export_desc) {
    uint32_t required;

    if (component == NULL || component->export_count == UINT32_MAX)
        return false;
    required = component->export_count + 1u;
    if (!reserve_array(
            (void **)&component->exports,
            &component->export_capacity,
            required,
            sizeof(*component->exports)))
        return false;
    component->exports[component->export_count++] = export_desc;
    return true;
}

static turbowasm_status read_function_externtype(
    turbowasm_reader *reader,
    const turbowasm_component_binary *component,
    uint32_t current_type_count,
    uint32_t *out_type_index) {
    uint8_t kind;
    uint32_t type_index;
    const turbowasm_component_type *type;

    if (!turbowasm_reader_u8(reader, &kind))
        return TURBOWASM_MALFORMED_MODULE;
    if (kind != 0x01u)
        return TURBOWASM_UNSUPPORTED;
    if (!turbowasm_reader_uleb32(reader, &type_index) ||
        type_index >= current_type_count)
        return TURBOWASM_MALFORMED_MODULE;

    type = turbowasm_component_type_graph_get(
        &component->type_graph, type_index);
    if (type == NULL ||
        type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_MALFORMED_MODULE;

    *out_type_index = type_index;
    return TURBOWASM_OK;
}

static turbowasm_status decode_component_import_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_type_count,
    uint32_t *next_component_function_index,
    uint32_t *next_component_instance_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL ||
        next_component_function_index == NULL ||
        next_component_instance_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_import import_desc;
        const turbowasm_component_type *type;
        uint8_t external_kind;
        turbowasm_status status;

        memset(&import_desc, 0, sizeof(import_desc));

        status = read_name_attributes(
            &section, &import_desc.name);
        if (status != TURBOWASM_OK)
            return status;

        if (!turbowasm_reader_u8(
                &section, &external_kind))
            return TURBOWASM_MALFORMED_MODULE;

        /*
         * Only func and instance externtypes carry a plain typeidx here.
         * Other external kinds have different payload grammars (for example
         * type imports carry a typebound) and remain fail-closed until their
         * own retained semantics are implemented.
         */
        if (external_kind != 0x01u &&
            external_kind != 0x05u)
            return TURBOWASM_UNSUPPORTED;

        if (!turbowasm_reader_uleb32(
                &section, &import_desc.type_index) ||
            import_desc.type_index >= current_type_count)
            return TURBOWASM_MALFORMED_MODULE;

        type = turbowasm_component_type_graph_get(
            &component->type_graph,
            import_desc.type_index);
        if (type == NULL)
            return TURBOWASM_MALFORMED_MODULE;

        if (external_kind == 0x01u) {
            if (type->kind !=
                    TURBOWASM_COMPONENT_TYPE_FUNCTION)
                return TURBOWASM_MALFORMED_MODULE;
            if (*next_component_function_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;

            import_desc.kind =
                TURBOWASM_COMPONENT_EXTERN_FUNCTION;
            import_desc.item_index =
                *next_component_function_index;
            ++*next_component_function_index;
        } else {
            if (type->kind !=
                    TURBOWASM_COMPONENT_TYPE_INSTANCE)
                return TURBOWASM_MALFORMED_MODULE;
            if (*next_component_instance_index == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;

            import_desc.kind =
                TURBOWASM_COMPONENT_EXTERN_INSTANCE;
            import_desc.item_index =
                *next_component_instance_index;
            ++*next_component_instance_index;
        }

        if (!append_import(component, import_desc))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_component_export_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t *current_type_count,
    uint32_t *current_component_function_count) {
    uint32_t count;
    uint32_t i;

    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_export export_desc = {0};
        uint8_t sort, optional_type;
        uint32_t index_limit;
        turbowasm_status status;

        export_desc.type_index = UINT32_MAX;
        status = read_name_attributes(&section, &export_desc.name);
        if (status != TURBOWASM_OK)
            return status;
        if (!turbowasm_reader_u8(&section, &sort))
            return TURBOWASM_MALFORMED_MODULE;
        if (sort != TURBOWASM_COMPONENT_EXTERN_FUNCTION &&
            sort != TURBOWASM_COMPONENT_EXTERN_TYPE)
            return TURBOWASM_UNSUPPORTED;
        export_desc.kind = (turbowasm_component_external_kind)sort;
        index_limit = sort == TURBOWASM_COMPONENT_EXTERN_TYPE
            ? *current_type_count : *current_component_function_count;
        if (!turbowasm_reader_uleb32(&section, &export_desc.item_index) ||
            export_desc.item_index >= index_limit ||
            !turbowasm_reader_u8(&section, &optional_type))
            return TURBOWASM_MALFORMED_MODULE;
        if (optional_type == 1u) {
            if (sort != TURBOWASM_COMPONENT_EXTERN_FUNCTION)
                return TURBOWASM_UNSUPPORTED;
            export_desc.has_ascribed_type = true;
            status = read_function_externtype(&section, component,
                *current_type_count, &export_desc.type_index);
            if (status != TURBOWASM_OK)
                return status;
        } else if (optional_type != 0u) {
            return TURBOWASM_MALFORMED_MODULE;
        }

        /* Exports introduce aliases in their own index space. Keep them in
         * declaration order so later canon lifts/lowers see the correct IDs.
         * https://github.com/WebAssembly/component-model/blob/main/design/mvp/Binary.md#import-and-export-definitions */
        if (sort == TURBOWASM_COMPONENT_EXTERN_TYPE) {
            const turbowasm_component_type *type = turbowasm_component_type_graph_get(
                &component->type_graph, export_desc.item_index);
            if (type == NULL || *current_type_count >= component->type_graph.count)
                return TURBOWASM_MALFORMED_MODULE;
            if (type->kind == TURBOWASM_COMPONENT_TYPE_INSTANCE)
                return TURBOWASM_UNSUPPORTED;
            if (!clone_local_type(&component->type_graph,
                    *current_type_count, export_desc.item_index))
                return TURBOWASM_OUT_OF_MEMORY;
            ++*current_type_count;
        } else {
            turbowasm_component_function_alias alias = {0};
            if (*current_component_function_count == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            alias.component_function_index = *current_component_function_count;
            alias.local_source = true;
            alias.source_function_index = export_desc.item_index;
            if (!append_component_function_alias(component, alias))
                return TURBOWASM_OUT_OF_MEMORY;
            ++*current_component_function_count;
        }
        if (!append_export(component, export_desc))
            return TURBOWASM_OUT_OF_MEMORY;
    }
    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_component_semantics(
    turbowasm_component_binary *component) {
    uint32_t total_types = 0u;
    uint32_t current_types = 0u;
    uint32_t current_core_modules = 0u;
    uint32_t next_core_function_index = 0u;
    uint32_t next_core_memory_index = 0u;
    uint32_t next_component_function_index = 0u;
    uint32_t next_component_instance_index = 0u;
    uint64_t next_resource_identity = 1u;
    uint32_t i;

    if (component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < component->section_count; ++i) {
        const turbowasm_component_section *section =
            &component->sections[i];

        if (section->id == 7u || section->id == 6u || section->id == 11u) {
            turbowasm_reader reader;
            uint32_t count;
            turbowasm_reader_init(
                &reader, section->payload, section->size);
            if (!turbowasm_reader_uleb32(&reader, &count) ||
                count > UINT32_MAX - total_types)
                return TURBOWASM_MALFORMED_MODULE;
            /*
             * Alias/export sections are an upper bound: only type entries
             * consume Component type indices. The graph is trimmed to current_types
             * after semantic decoding.
             */
            total_types += count;
        }
    }

    next_resource_identity = (uint64_t)total_types + UINT64_C(1);
    if (next_resource_identity == 0u)
        return TURBOWASM_OUT_OF_MEMORY;

    if (!turbowasm_component_type_graph_allocate(
            &component->type_graph, total_types))
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < component->section_count; ++i) {
        const turbowasm_component_section *section =
            &component->sections[i];
        turbowasm_reader reader;
        turbowasm_status status = TURBOWASM_OK;

        turbowasm_reader_init(
            &reader, section->payload, section->size);

        if (section->id == 1u) {
            if (current_core_modules == UINT32_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            ++current_core_modules;
        } else if (section->id == 2u) {
            status = decode_core_instance_section(
                reader,
                component,
                current_core_modules,
                next_core_function_index);
        } else if (section->id == 5u) {
            status = decode_component_instance_section(
                reader,
                component,
                next_component_function_index,
                &next_component_instance_index);
        } else if (section->id == 6u) {
            status = decode_alias_section(
                reader,
                component,
                component->core_instance_count,
                next_component_instance_index,
                &next_core_function_index,
                &next_core_memory_index,
                &next_component_function_index,
                &current_types);
        } else if (section->id == 7u) {
            status = decode_component_type_section(
                reader,
                component,
                next_core_function_index,
                &current_types,
                &next_resource_identity);
        } else if (section->id == 8u) {
            status = decode_canon_section(
                reader,
                component,
                current_types,
                &next_core_function_index,
                next_core_memory_index,
                &next_component_function_index);
        } else if (section->id == 10u) {
            status = decode_component_import_section(
                reader,
                component,
                current_types,
                &next_component_function_index,
                &next_component_instance_index);
        } else if (section->id == 11u) {
            status = decode_component_export_section(
                reader,
                component,
                &current_types,
                &next_component_function_index);
        }

        if (status != TURBOWASM_OK)
            return status;
    }

    if (current_core_modules != component->core_module_count)
        return TURBOWASM_MALFORMED_MODULE;
    component->type_graph.count = current_types;
    if (!turbowasm_component_type_graph_validate(
            &component->type_graph))
        return TURBOWASM_MALFORMED_MODULE;

    component->core_function_count = next_core_function_index;
    component->component_function_count =
        next_component_function_index;
    component->component_instance_index_count =
        next_component_instance_index;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_binary_load(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size) {
    return turbowasm_component_binary_load_with_config(
        component, bytes, size, NULL);
}

turbowasm_status turbowasm_component_binary_load_with_config(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_reader reader;
    turbowasm_status status = TURBOWASM_OK;

    if (component == NULL || bytes == NULL ||
        component->bytes != NULL ||
        component->sections != NULL ||
        component->core_modules != NULL ||
        component->core_instances != NULL ||
        component->core_function_aliases != NULL ||
        component->core_memory_aliases != NULL ||
        component->canon_lifts != NULL ||
        component->canon_lowers != NULL ||
        component->resource_builtins != NULL ||
        component->component_instances != NULL ||
        component->component_function_aliases != NULL ||
        component->type_graph.types != NULL ||
        component->type_graph.count != 0u ||
        component->imports != NULL ||
        component->exports != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;
    if (normalized.limits.max_module_bytes != 0u &&
        size > normalized.limits.max_module_bytes)
        return TURBOWASM_OUT_OF_MEMORY;

    scope = turbowasm_runtime_scope_enter(&normalized);
    turbowasm_reader_init(&reader, bytes, size);

    if (!component_preamble(&reader)) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto fail;
    }

    component->bytes = bytes;
    component->size = size;
    component->config = normalized;

    while (turbowasm_reader_remaining(&reader) != 0u) {
        uint8_t id;
        uint32_t payload_size;
        turbowasm_reader payload;

        if (!turbowasm_reader_u8(&reader, &id) ||
            id > TW_COMPONENT_SECTION_MAX ||
            !turbowasm_reader_uleb32(&reader, &payload_size) ||
            !turbowasm_reader_slice(
                &reader, (size_t)payload_size, &payload)) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        if (!append_section(
                component, id, payload.cursor, payload_size)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }

        if (id == TW_COMPONENT_SECTION_CUSTOM) {
            status = validate_custom_section(payload);
            if (status != TURBOWASM_OK)
                goto fail;
        } else if (id == TW_COMPONENT_SECTION_CORE_MODULE) {
            status = validate_core_module(
                payload.cursor, payload_size, &normalized);
            if (status != TURBOWASM_OK)
                goto fail;
            if (!append_core_module(
                    component, payload.cursor, payload_size)) {
                status = TURBOWASM_OUT_OF_MEMORY;
                goto fail;
            }
        } else if (id == 4u) {
            /*
             * C2a only establishes the nested-component boundary. Full nested
             * section validation is part of later Component decoding.
             */
            turbowasm_reader nested = payload;
            if (!component_preamble(&nested)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }
        }
    }

    status = decode_component_semantics(component);
    if (status != TURBOWASM_OK)
        goto fail;

    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

fail:
    {
        uint32_t index;
        for (index = 0u;
             index < component->core_instance_count;
             ++index) {
            turbowasm_rt_free(
                component->core_instances[index].arguments);
            turbowasm_rt_free(
                component->core_instances[index].exports);
        }
        for (index = 0u;
             index < component->component_instance_count;
             ++index)
            turbowasm_rt_free(
                component->component_instances[index].exports);
    }
    turbowasm_component_type_graph_destroy(
        &component->type_graph);
    turbowasm_rt_free(component->imports);
    turbowasm_rt_free(component->exports);
    turbowasm_rt_free(component->sections);
    turbowasm_rt_free(component->core_modules);
    turbowasm_rt_free(component->core_instances);
    turbowasm_rt_free(component->core_function_aliases);
    turbowasm_rt_free(component->core_memory_aliases);
    turbowasm_rt_free(component->canon_lifts);
    turbowasm_rt_free(component->canon_lowers);
    turbowasm_rt_free(component->resource_builtins);
    turbowasm_rt_free(component->component_instances);
    turbowasm_rt_free(component->component_function_aliases);
    memset(component, 0, sizeof(*component));
    turbowasm_runtime_scope_leave(scope);
    return status;
}

void turbowasm_component_binary_destroy(
    turbowasm_component_binary *component) {
    turbowasm_runtime_scope scope;

    if (component == NULL)
        return;

    scope = turbowasm_runtime_scope_enter(&component->config);
    {
        uint32_t index;
        for (index = 0u;
             index < component->core_instance_count;
             ++index) {
            turbowasm_rt_free(
                component->core_instances[index].arguments);
            turbowasm_rt_free(
                component->core_instances[index].exports);
        }
        for (index = 0u;
             index < component->component_instance_count;
             ++index)
            turbowasm_rt_free(
                component->component_instances[index].exports);
    }
    turbowasm_component_type_graph_destroy(
        &component->type_graph);
    turbowasm_rt_free(component->imports);
    turbowasm_rt_free(component->exports);
    turbowasm_rt_free(component->sections);
    turbowasm_rt_free(component->core_modules);
    turbowasm_rt_free(component->core_instances);
    turbowasm_rt_free(component->core_function_aliases);
    turbowasm_rt_free(component->core_memory_aliases);
    turbowasm_rt_free(component->canon_lifts);
    turbowasm_rt_free(component->canon_lowers);
    turbowasm_rt_free(component->resource_builtins);
    turbowasm_rt_free(component->component_instances);
    turbowasm_rt_free(component->component_function_aliases);
    memset(component, 0, sizeof(*component));
    turbowasm_runtime_scope_leave(scope);
}

const turbowasm_component_section *
turbowasm_component_binary_section_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->section_count)
        return NULL;
    return &component->sections[index];
}

const turbowasm_component_core_module *
turbowasm_component_binary_core_module_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->core_module_count)
        return NULL;
    return &component->core_modules[index];
}

const turbowasm_component_core_instance_def *
turbowasm_component_binary_core_instance_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->core_instance_count)
        return NULL;
    return &component->core_instances[index];
}

const turbowasm_component_core_function_alias *
turbowasm_component_binary_core_function_alias_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL ||
        index >= component->core_function_alias_count)
        return NULL;
    return &component->core_function_aliases[index];
}

const turbowasm_component_core_memory_alias *
turbowasm_component_binary_core_memory_alias_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL ||
        index >= component->core_memory_alias_count)
        return NULL;
    return &component->core_memory_aliases[index];
}

const turbowasm_component_canon_lift *
turbowasm_component_binary_canon_lift_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->canon_lift_count)
        return NULL;
    return &component->canon_lifts[index];
}

const turbowasm_component_canon_lower *
turbowasm_component_binary_canon_lower_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->canon_lower_count)
        return NULL;
    return &component->canon_lowers[index];
}

const turbowasm_component_resource_builtin *
turbowasm_component_binary_resource_builtin_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL ||
        index >= component->resource_builtin_count)
        return NULL;
    return &component->resource_builtins[index];
}

const turbowasm_component_instance_def *
turbowasm_component_binary_component_instance_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL ||
        index >= component->component_instance_count)
        return NULL;
    return &component->component_instances[index];
}

const turbowasm_component_function_alias *
turbowasm_component_binary_component_function_alias_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL ||
        index >= component->component_function_alias_count)
        return NULL;
    return &component->component_function_aliases[index];
}

const turbowasm_component_import *
turbowasm_component_binary_import_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->import_count)
        return NULL;
    return &component->imports[index];
}

const turbowasm_component_export *
turbowasm_component_binary_export_at(
    const turbowasm_component_binary *component,
    uint32_t index) {
    if (component == NULL || index >= component->export_count)
        return NULL;
    return &component->exports[index];
}
