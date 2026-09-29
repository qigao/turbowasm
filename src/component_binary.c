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

static turbowasm_status decode_component_function_type(
    turbowasm_reader *reader,
    turbowasm_component_binary *component,
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
            &component->type_graph,
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

static turbowasm_status decode_core_alias_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_core_instances,
    uint32_t *next_core_function_index,
    uint32_t *next_core_memory_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL ||
        next_core_function_index == NULL ||
        next_core_memory_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        uint8_t sort_prefix;
        uint8_t core_sort;
        uint8_t alias_kind;
        uint32_t instance_index;
        turbowasm_component_name name = {0};
        turbowasm_status status;

        if (!turbowasm_reader_u8(&section, &sort_prefix) ||
            !turbowasm_reader_u8(&section, &core_sort) ||
            !turbowasm_reader_u8(&section, &alias_kind))
            return TURBOWASM_MALFORMED_MODULE;

        /*
         * C5c2a supports:
         *   alias core export <core-instance> <name> (core func)
         *   alias core export <core-instance> <name> (core memory)
         */
        if (sort_prefix != 0x00u || alias_kind != 0x01u)
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
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_canon_lift_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t current_types,
    uint32_t current_core_functions,
    uint32_t current_core_memories,
    uint32_t *next_component_function_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL ||
        next_component_function_index == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_canon_lift lift = {0};
        const turbowasm_component_type *type;
        uint8_t opcode;
        uint8_t sort;
        uint32_t option_count;
        uint32_t option_index;
        bool string_encoding_seen = false;

        lift.string_encoding = TURBOWASM_COMPONENT_STRING_UTF8;

        if (!turbowasm_reader_u8(&section, &opcode) ||
            !turbowasm_reader_u8(&section, &sort))
            return TURBOWASM_MALFORMED_MODULE;

        if (opcode != 0x00u || sort != 0x00u)
            return TURBOWASM_UNSUPPORTED;

        if (!turbowasm_reader_uleb32(
                &section, &lift.core_function_index) ||
            lift.core_function_index >= current_core_functions ||
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
                    if (string_encoding_seen)
                        return TURBOWASM_MALFORMED_MODULE;
                    string_encoding_seen = true;
                    lift.string_encoding =
                        TURBOWASM_COMPONENT_STRING_UTF8;
                    break;

                case 0x01u: /* utf16 */
                case 0x02u: /* latin1+utf16 */
                    return TURBOWASM_UNSUPPORTED;

                case 0x03u: /* memory */
                    if (lift.has_memory ||
                        !turbowasm_reader_uleb32(
                            &section, &lift.memory_index) ||
                        lift.memory_index >= current_core_memories)
                        return TURBOWASM_MALFORMED_MODULE;
                    lift.has_memory = true;
                    break;

                case 0x04u: /* realloc */
                    if (lift.has_realloc ||
                        !turbowasm_reader_uleb32(
                            &section,
                            &lift.realloc_function_index) ||
                        lift.realloc_function_index >=
                            current_core_functions)
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
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_component_type_section(
    turbowasm_reader section,
    turbowasm_component_binary *component,
    uint32_t *next_type_index) {
    uint32_t count;
    uint32_t i;

    if (component == NULL || next_type_index == NULL)
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
        } else if (opcode == 0x70u) {
            turbowasm_component_type_ref element;
            status = read_component_type_ref(
                &section, id, &element);
            if (status != TURBOWASM_OK)
                return status;
            if (!turbowasm_component_type_graph_define_list_ref(
                    &component->type_graph, id, element))
                return TURBOWASM_OUT_OF_MEMORY;
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
            if (dtor_flag != 0u &&
                !turbowasm_reader_uleb32(
                    &section, &dtor_index))
                return TURBOWASM_MALFORMED_MODULE;

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
    uint32_t current_type_count) {
    uint32_t count;
    uint32_t i;

    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_import import_desc;
        turbowasm_status status;
        memset(&import_desc, 0, sizeof(import_desc));

        status = read_name_attributes(
            &section, &import_desc.name);
        if (status != TURBOWASM_OK)
            return status;

        import_desc.kind = TURBOWASM_COMPONENT_EXTERN_FUNCTION;
        status = read_function_externtype(
            &section, component, current_type_count,
            &import_desc.type_index);
        if (status != TURBOWASM_OK)
            return status;

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
    uint32_t current_type_count,
    uint32_t current_component_function_count) {
    uint32_t count;
    uint32_t i;

    if (!turbowasm_reader_uleb32(&section, &count))
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < count; ++i) {
        turbowasm_component_export export_desc;
        uint8_t sort;
        uint8_t optional_type;
        turbowasm_status status;

        memset(&export_desc, 0, sizeof(export_desc));
        export_desc.type_index = UINT32_MAX;

        status = read_name_attributes(
            &section, &export_desc.name);
        if (status != TURBOWASM_OK)
            return status;

        if (!turbowasm_reader_u8(&section, &sort))
            return TURBOWASM_MALFORMED_MODULE;
        if (sort != 0x01u)
            return TURBOWASM_UNSUPPORTED;
        export_desc.kind = TURBOWASM_COMPONENT_EXTERN_FUNCTION;

        if (!turbowasm_reader_uleb32(
                &section, &export_desc.item_index) ||
            export_desc.item_index >= current_component_function_count ||
            !turbowasm_reader_u8(&section, &optional_type))
            return TURBOWASM_MALFORMED_MODULE;

        if (optional_type == 1u) {
            export_desc.has_ascribed_type = true;
            status = read_function_externtype(
                &section, component, current_type_count,
                &export_desc.type_index);
            if (status != TURBOWASM_OK)
                return status;
        } else if (optional_type != 0u) {
            return TURBOWASM_MALFORMED_MODULE;
        }

        if (!append_export(component, export_desc))
            return TURBOWASM_OUT_OF_MEMORY;
    }

    return turbowasm_reader_remaining(&section) == 0u
        ? TURBOWASM_OK
        : TURBOWASM_MALFORMED_MODULE;
}

static turbowasm_status decode_component_semantics(
    turbowasm_component_binary *component) {
    uint32_t total_types = 0u;
    uint32_t current_types = 0u;
    uint32_t current_core_modules = 0u;
    uint32_t next_core_function_index = 0u;
    uint32_t next_core_memory_index = 0u;
    uint32_t next_component_function_index = 0u;
    uint32_t i;

    if (component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < component->section_count; ++i) {
        const turbowasm_component_section *section =
            &component->sections[i];

        if (section->id == 7u) {
            turbowasm_reader reader;
            uint32_t count;
            turbowasm_reader_init(
                &reader, section->payload, section->size);
            if (!turbowasm_reader_uleb32(&reader, &count) ||
                count > UINT32_MAX - total_types)
                return TURBOWASM_MALFORMED_MODULE;
            total_types += count;
        }
    }

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
                reader, component, current_core_modules);
        } else if (section->id == 6u) {
            status = decode_core_alias_section(
                reader,
                component,
                component->core_instance_count,
                &next_core_function_index,
                &next_core_memory_index);
        } else if (section->id == 7u) {
            status = decode_component_type_section(
                reader, component, &current_types);
        } else if (section->id == 8u) {
            status = decode_canon_lift_section(
                reader,
                component,
                current_types,
                next_core_function_index,
                next_core_memory_index,
                &next_component_function_index);
        } else if (section->id == 10u) {
            uint32_t before = component->import_count;
            status = decode_component_import_section(
                reader, component, current_types);
            if (status == TURBOWASM_OK) {
                uint32_t added = component->import_count - before;
                if (added > UINT32_MAX -
                        next_component_function_index)
                    return TURBOWASM_OUT_OF_MEMORY;
                next_component_function_index += added;
            }
        } else if (section->id == 11u) {
            status = decode_component_export_section(
                reader,
                component,
                current_types,
                next_component_function_index);
        }

        if (status != TURBOWASM_OK)
            return status;
    }

    if (current_core_modules != component->core_module_count ||
        current_types != total_types ||
        !turbowasm_component_type_graph_validate(
            &component->type_graph))
        return TURBOWASM_MALFORMED_MODULE;

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
             ++index)
            turbowasm_rt_free(
                component->core_instances[index].arguments);
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
             ++index)
            turbowasm_rt_free(
                component->core_instances[index].arguments);
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
