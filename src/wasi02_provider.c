#include "wasi02_provider.h"

#include "runtime_alloc.h"

#include <limits.h>
#include <string.h>

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

static void sequence_destroy(
    turbowasm_wasi02_value_sequence *sequence) {
    size_t i;

    if (sequence == NULL)
        return;

    for (i = 0u; i < sequence->count; ++i)
        turbowasm_wasi02_value_destroy(&sequence->items[i]);
    turbowasm_rt_free(sequence->items);
    sequence->items = NULL;
    sequence->count = 0u;
}

void turbowasm_wasi02_value_destroy(
    turbowasm_wasi02_value *value) {
    if (value == NULL)
        return;

    switch (value->kind) {
        case TURBOWASM_WASI02_VALUE_STRING:
            turbowasm_rt_free(value->as.string.data);
            break;
        case TURBOWASM_WASI02_VALUE_LIST:
            sequence_destroy(&value->as.list);
            break;
        case TURBOWASM_WASI02_VALUE_TUPLE:
            sequence_destroy(&value->as.tuple);
            break;
        case TURBOWASM_WASI02_VALUE_RECORD:
            sequence_destroy(&value->as.record);
            break;
        case TURBOWASM_WASI02_VALUE_OPTION:
            if (value->as.option.value != NULL) {
                turbowasm_wasi02_value_destroy(
                    value->as.option.value);
                turbowasm_rt_free(value->as.option.value);
            }
            break;
        case TURBOWASM_WASI02_VALUE_RESULT:
            if (value->as.result.value != NULL) {
                turbowasm_wasi02_value_destroy(
                    value->as.result.value);
                turbowasm_rt_free(value->as.result.value);
            }
            break;
        case TURBOWASM_WASI02_VALUE_VARIANT:
            if (value->as.variant.value != NULL) {
                turbowasm_wasi02_value_destroy(
                    value->as.variant.value);
                turbowasm_rt_free(value->as.variant.value);
            }
            break;
        default:
            break;
    }

    memset(value, 0, sizeof(*value));
}

static bool value_matches_type_depth(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    uint32_t depth) {
    size_t i;
    const turbowasm_wasi02_type_desc *arm;

    if (type == NULL || value == NULL || depth >= 64u)
        return false;

    switch (type->kind) {
        case TURBOWASM_WASI02_TYPE_UNIT:
            return value->kind == TURBOWASM_WASI02_VALUE_UNIT;
        case TURBOWASM_WASI02_TYPE_BOOL:
            return value->kind == TURBOWASM_WASI02_VALUE_BOOL;
        case TURBOWASM_WASI02_TYPE_U8:
            return value->kind == TURBOWASM_WASI02_VALUE_U8;
        case TURBOWASM_WASI02_TYPE_U16:
            return value->kind == TURBOWASM_WASI02_VALUE_U16;
        case TURBOWASM_WASI02_TYPE_U32:
            return value->kind == TURBOWASM_WASI02_VALUE_U32;
        case TURBOWASM_WASI02_TYPE_U64:
            return value->kind == TURBOWASM_WASI02_VALUE_U64;
        case TURBOWASM_WASI02_TYPE_STRING:
            return value->kind == TURBOWASM_WASI02_VALUE_STRING &&
                   utf8_valid(
                       value->as.string.data,
                       value->as.string.size);
        case TURBOWASM_WASI02_TYPE_ALIAS:
            return value_matches_type_depth(
                type->as.alias.target, value, depth + 1u);
        case TURBOWASM_WASI02_TYPE_LIST:
            if (value->kind != TURBOWASM_WASI02_VALUE_LIST)
                return false;
            for (i = 0u; i < value->as.list.count; ++i) {
                if (!value_matches_type_depth(
                        type->as.list.element,
                        &value->as.list.items[i],
                        depth + 1u))
                    return false;
            }
            return true;
        case TURBOWASM_WASI02_TYPE_TUPLE:
            if (value->kind != TURBOWASM_WASI02_VALUE_TUPLE ||
                value->as.tuple.count != type->as.tuple.count)
                return false;
            for (i = 0u; i < value->as.tuple.count; ++i) {
                if (!value_matches_type_depth(
                        type->as.tuple.elements[i],
                        &value->as.tuple.items[i],
                        depth + 1u))
                    return false;
            }
            return true;
        case TURBOWASM_WASI02_TYPE_RECORD:
            if (value->kind != TURBOWASM_WASI02_VALUE_RECORD ||
                value->as.record.count != type->as.record.count)
                return false;
            for (i = 0u; i < value->as.record.count; ++i) {
                if (!value_matches_type_depth(
                        type->as.record.fields[i].type,
                        &value->as.record.items[i],
                        depth + 1u))
                    return false;
            }
            return true;
        case TURBOWASM_WASI02_TYPE_OPTION:
            if (value->kind != TURBOWASM_WASI02_VALUE_OPTION)
                return false;
            if (!value->as.option.has_value)
                return value->as.option.value == NULL;
            return value->as.option.value != NULL &&
                   value_matches_type_depth(
                       type->as.option.payload,
                       value->as.option.value,
                       depth + 1u);
        case TURBOWASM_WASI02_TYPE_RESULT:
            if (value->kind != TURBOWASM_WASI02_VALUE_RESULT)
                return false;
            arm = value->as.result.is_error
                ? type->as.result.error
                : type->as.result.ok;
            if (arm == NULL)
                return value->as.result.value == NULL;
            return value->as.result.value != NULL &&
                   value_matches_type_depth(
                       arm, value->as.result.value, depth + 1u);
        case TURBOWASM_WASI02_TYPE_VARIANT: {
            const turbowasm_wasi02_variant_case *variant_case;
            if (value->kind != TURBOWASM_WASI02_VALUE_VARIANT ||
                type->as.variant.cases == NULL ||
                type->as.variant.count == 0u ||
                value->as.variant.case_index >=
                    type->as.variant.count)
                return false;
            variant_case =
                &type->as.variant.cases[
                    value->as.variant.case_index];
            if (variant_case->payload == NULL)
                return value->as.variant.value == NULL;
            return value->as.variant.value != NULL &&
                   value_matches_type_depth(
                       variant_case->payload,
                       value->as.variant.value,
                       depth + 1u);
        }
        case TURBOWASM_WASI02_TYPE_ENUM:
            return value->kind == TURBOWASM_WASI02_VALUE_ENUM &&
                   type->as.enumeration.labels != NULL &&
                   type->as.enumeration.count != 0u &&
                   value->as.enum_index < type->as.enumeration.count;
        case TURBOWASM_WASI02_TYPE_FLAGS:
            return value->kind == TURBOWASM_WASI02_VALUE_FLAGS &&
                   type->as.flags.labels != NULL &&
                   type->as.flags.count != 0u &&
                   type->as.flags.count <= 32u &&
                   (value->as.flags &
                    ~(type->as.flags.count == 32u
                        ? UINT32_MAX
                        : ((UINT32_C(1) << type->as.flags.count) -
                           UINT32_C(1)))) == 0u;
        case TURBOWASM_WASI02_TYPE_RESOURCE:
            return value->kind == TURBOWASM_WASI02_VALUE_RESOURCE;
        default:
            return false;
    }
}

bool turbowasm_wasi02_value_matches_type(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value) {
    return value_matches_type_depth(type, value, 0u);
}

turbowasm_status turbowasm_wasi02_provider_init(
    turbowasm_wasi02_provider *provider,
    const turbowasm_wasi02_provider_config *config,
    const turbowasm_runtime_config *runtime_config) {
    turbowasm_runtime_config normalized;

    if (provider == NULL || config == NULL ||
        provider->initialized ||
        !turbowasm_runtime_config_normalize(
            runtime_config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

    memset(provider, 0, sizeof(*provider));
    provider->config = *config;
    provider->runtime_config = normalized;
    provider->initialized = true;
    return TURBOWASM_OK;
}

void turbowasm_wasi02_provider_destroy(
    turbowasm_wasi02_provider *provider) {
    if (provider != NULL)
        memset(provider, 0, sizeof(*provider));
}

static turbowasm_status make_sequence(
    turbowasm_wasi02_value *out,
    turbowasm_wasi02_value_kind kind,
    size_t count) {
    turbowasm_wasi02_value *items = NULL;

    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (count != 0u) {
        if (count > SIZE_MAX / sizeof(*items))
            return TURBOWASM_OUT_OF_MEMORY;
        items = (turbowasm_wasi02_value *)turbowasm_rt_calloc(
            count, sizeof(*items));
        if (items == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == TURBOWASM_WASI02_VALUE_LIST) {
        out->as.list.items = items;
        out->as.list.count = count;
    } else if (kind == TURBOWASM_WASI02_VALUE_TUPLE) {
        out->as.tuple.items = items;
        out->as.tuple.count = count;
    } else if (kind == TURBOWASM_WASI02_VALUE_RECORD) {
        out->as.record.items = items;
        out->as.record.count = count;
    } else {
        turbowasm_rt_free(items);
        memset(out, 0, sizeof(*out));
        return TURBOWASM_INVALID_ARGUMENT;
    }
    return TURBOWASM_OK;
}

static turbowasm_status copy_string(
    turbowasm_wasi02_value *out,
    turbowasm_wasi02_string_view source) {
    uint8_t *copy = NULL;

    if (out == NULL || !utf8_valid(source.data, source.size))
        return TURBOWASM_TYPE_MISMATCH;

    if (source.size != 0u) {
        copy = (uint8_t *)turbowasm_rt_malloc(source.size);
        if (copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(copy, source.data, source.size);
    }

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_STRING;
    out->as.string.data = copy;
    out->as.string.size = source.size;
    return TURBOWASM_OK;
}

static turbowasm_status make_datetime(
    turbowasm_wasi02_value *out,
    uint64_t seconds,
    uint32_t nanoseconds) {
    turbowasm_status status;

    if (nanoseconds >= UINT32_C(1000000000))
        return TURBOWASM_TYPE_MISMATCH;

    status = make_sequence(
        out, TURBOWASM_WASI02_VALUE_RECORD, 2u);
    if (status != TURBOWASM_OK)
        return status;

    out->as.record.items[0].kind =
        TURBOWASM_WASI02_VALUE_U64;
    out->as.record.items[0].as.u64 = seconds;
    out->as.record.items[1].kind =
        TURBOWASM_WASI02_VALUE_U32;
    out->as.record.items[1].as.u32 = nanoseconds;
    return TURBOWASM_OK;
}

static turbowasm_status call_wall_clock(
    turbowasm_wasi02_provider *provider,
    const char *function_name,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_wall_clock_fn callback = NULL;
    uint64_t seconds = 0u;
    uint32_t nanoseconds = 0u;
    turbowasm_status status;

    if (strcmp(function_name, "now") == 0)
        callback = provider->config.wall_clock_now;
    else if (strcmp(function_name, "resolution") == 0)
        callback = provider->config.wall_clock_resolution;

    if (callback == NULL)
        return TURBOWASM_UNSUPPORTED;

    status = callback(
        provider->config.context, &seconds, &nanoseconds);
    if (status != TURBOWASM_OK)
        return status;
    return make_datetime(out, seconds, nanoseconds);
}

static turbowasm_status call_monotonic_clock(
    turbowasm_wasi02_provider *provider,
    const char *function_name,
    turbowasm_wasi02_value *out) {
    turbowasm_wasi02_monotonic_clock_fn callback = NULL;
    uint64_t value = 0u;
    turbowasm_status status;

    if (strcmp(function_name, "now") == 0)
        callback = provider->config.monotonic_clock_now;
    else if (strcmp(function_name, "resolution") == 0)
        callback = provider->config.monotonic_clock_resolution;
    else if (strcmp(function_name, "subscribe-instant") == 0 ||
             strcmp(function_name, "subscribe-duration") == 0)
        return TURBOWASM_UNSUPPORTED;

    if (callback == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = callback(provider->config.context, &value);
    if (status != TURBOWASM_OK)
        return status;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_U64;
    out->as.u64 = value;
    return TURBOWASM_OK;
}

static turbowasm_status call_random_bytes(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_random_bytes_fn callback,
    uint64_t requested,
    turbowasm_wasi02_value *out) {
    size_t count;
    uint8_t *bytes = NULL;
    size_t i;
    turbowasm_status status;

    if (callback == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (requested > (uint64_t)SIZE_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    count = (size_t)requested;

    status = make_sequence(
        out, TURBOWASM_WASI02_VALUE_LIST, count);
    if (status != TURBOWASM_OK)
        return status;

    if (count != 0u) {
        bytes = (uint8_t *)turbowasm_rt_malloc(count);
        if (bytes == NULL) {
            turbowasm_wasi02_value_destroy(out);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    status = callback(provider->config.context, bytes, count);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(bytes);
        turbowasm_wasi02_value_destroy(out);
        return status;
    }

    for (i = 0u; i < count; ++i) {
        out->as.list.items[i].kind =
            TURBOWASM_WASI02_VALUE_U8;
        out->as.list.items[i].as.u8 = bytes[i];
    }
    turbowasm_rt_free(bytes);
    return TURBOWASM_OK;
}

static turbowasm_status call_random_u64(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_random_u64_fn callback,
    turbowasm_wasi02_value *out) {
    uint64_t value = 0u;
    turbowasm_status status;

    if (callback == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = callback(provider->config.context, &value);
    if (status != TURBOWASM_OK)
        return status;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_U64;
    out->as.u64 = value;
    return TURBOWASM_OK;
}

static turbowasm_status call_seed(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_value *out) {
    uint64_t first = 0u;
    uint64_t second = 0u;
    turbowasm_status status;

    if (provider->config.insecure_seed == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = provider->config.insecure_seed(
        provider->config.context, &first, &second);
    if (status != TURBOWASM_OK)
        return status;

    status = make_sequence(
        out, TURBOWASM_WASI02_VALUE_TUPLE, 2u);
    if (status != TURBOWASM_OK)
        return status;
    out->as.tuple.items[0].kind =
        TURBOWASM_WASI02_VALUE_U64;
    out->as.tuple.items[0].as.u64 = first;
    out->as.tuple.items[1].kind =
        TURBOWASM_WASI02_VALUE_U64;
    out->as.tuple.items[1].as.u64 = second;
    return TURBOWASM_OK;
}

static turbowasm_status call_environment(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_value *out) {
    const turbowasm_wasi02_environment_entry_view *entries = NULL;
    size_t count = 0u;
    size_t i;
    turbowasm_status status;

    if (provider->config.environment == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = provider->config.environment(
        provider->config.context, &entries, &count);
    if (status != TURBOWASM_OK)
        return status;
    if (count != 0u && entries == NULL)
        return TURBOWASM_TYPE_MISMATCH;

    status = make_sequence(
        out, TURBOWASM_WASI02_VALUE_LIST, count);
    if (status != TURBOWASM_OK)
        return status;

    for (i = 0u; i < count; ++i) {
        turbowasm_wasi02_value *pair =
            &out->as.list.items[i];
        status = make_sequence(
            pair, TURBOWASM_WASI02_VALUE_TUPLE, 2u);
        if (status != TURBOWASM_OK)
            goto fail;
        status = copy_string(
            &pair->as.tuple.items[0], entries[i].name);
        if (status != TURBOWASM_OK)
            goto fail;
        status = copy_string(
            &pair->as.tuple.items[1], entries[i].value);
        if (status != TURBOWASM_OK)
            goto fail;
    }
    return TURBOWASM_OK;

fail:
    turbowasm_wasi02_value_destroy(out);
    return status;
}

static turbowasm_status call_arguments(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_value *out) {
    const turbowasm_wasi02_string_view *arguments = NULL;
    size_t count = 0u;
    size_t i;
    turbowasm_status status;

    if (provider->config.arguments == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = provider->config.arguments(
        provider->config.context, &arguments, &count);
    if (status != TURBOWASM_OK)
        return status;
    if (count != 0u && arguments == NULL)
        return TURBOWASM_TYPE_MISMATCH;

    status = make_sequence(
        out, TURBOWASM_WASI02_VALUE_LIST, count);
    if (status != TURBOWASM_OK)
        return status;
    for (i = 0u; i < count; ++i) {
        status = copy_string(
            &out->as.list.items[i], arguments[i]);
        if (status != TURBOWASM_OK) {
            turbowasm_wasi02_value_destroy(out);
            return status;
        }
    }
    return TURBOWASM_OK;
}

static turbowasm_status call_initial_cwd(
    turbowasm_wasi02_provider *provider,
    turbowasm_wasi02_value *out) {
    bool has_value = false;
    turbowasm_wasi02_string_view value = {0};
    turbowasm_status status;

    if (provider->config.initial_cwd == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = provider->config.initial_cwd(
        provider->config.context, &has_value, &value);
    if (status != TURBOWASM_OK)
        return status;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_WASI02_VALUE_OPTION;
    out->as.option.has_value = has_value;
    if (!has_value)
        return TURBOWASM_OK;

    out->as.option.value =
        (turbowasm_wasi02_value *)turbowasm_rt_calloc(
            1u, sizeof(*out->as.option.value));
    if (out->as.option.value == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    status = copy_string(out->as.option.value, value);
    if (status != TURBOWASM_OK) {
        turbowasm_wasi02_value_destroy(out);
        return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status validate_call(
    const turbowasm_wasi02_interface_desc *interface_desc,
    const turbowasm_wasi02_function_desc *function_desc,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    size_t i;

    if (interface_desc == NULL || function_desc == NULL)
        return TURBOWASM_UNSUPPORTED;
    if (argument_count != function_desc->param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < argument_count; ++i) {
        if (!turbowasm_wasi02_value_matches_type(
                function_desc->params[i].type, &arguments[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }
    if (function_desc->result != NULL && out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_provider_call(
    turbowasm_wasi02_provider *provider,
    const char *package_name,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result) {
    const turbowasm_wasi02_interface_desc *interface_desc;
    const turbowasm_wasi02_function_desc *function_desc;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (provider == NULL || !provider->initialized ||
        package_name == NULL || interface_name == NULL ||
        function_name == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    interface_desc = turbowasm_wasi02_find_interface(
        package_name, interface_name);
    function_desc = turbowasm_wasi02_find_function(
        interface_desc, function_name);

    status = validate_call(
        interface_desc, function_desc,
        arguments, argument_count, out_result);
    if (status != TURBOWASM_OK)
        return status;

    if (out_result != NULL)
        memset(out_result, 0, sizeof(*out_result));

    scope = turbowasm_runtime_scope_enter(
        &provider->runtime_config);

    if (strcmp(package_name, "wasi:clocks") == 0 &&
        strcmp(interface_name, "wall-clock") == 0) {
        status = call_wall_clock(
            provider, function_name, out_result);
    } else if (strcmp(package_name, "wasi:clocks") == 0 &&
               strcmp(interface_name, "monotonic-clock") == 0) {
        status = call_monotonic_clock(
            provider, function_name, out_result);
    } else if (strcmp(package_name, "wasi:random") == 0 &&
               strcmp(interface_name, "random") == 0) {
        if (strcmp(function_name, "get-random-bytes") == 0) {
            status = call_random_bytes(
                provider, provider->config.random_bytes,
                arguments[0].as.u64, out_result);
        } else if (strcmp(function_name, "get-random-u64") == 0) {
            status = call_random_u64(
                provider, provider->config.random_u64,
                out_result);
        } else {
            status = TURBOWASM_UNSUPPORTED;
        }
    } else if (strcmp(package_name, "wasi:random") == 0 &&
               strcmp(interface_name, "insecure") == 0) {
        if (strcmp(function_name, "get-insecure-random-bytes") == 0) {
            status = call_random_bytes(
                provider, provider->config.insecure_random_bytes,
                arguments[0].as.u64, out_result);
        } else if (strcmp(function_name, "get-insecure-random-u64") == 0) {
            status = call_random_u64(
                provider, provider->config.insecure_random_u64,
                out_result);
        } else {
            status = TURBOWASM_UNSUPPORTED;
        }
    } else if (strcmp(package_name, "wasi:random") == 0 &&
               strcmp(interface_name, "insecure-seed") == 0 &&
               strcmp(function_name, "insecure-seed") == 0) {
        status = call_seed(provider, out_result);
    } else if (strcmp(package_name, "wasi:cli") == 0 &&
               strcmp(interface_name, "environment") == 0) {
        if (strcmp(function_name, "get-environment") == 0)
            status = call_environment(provider, out_result);
        else if (strcmp(function_name, "get-arguments") == 0)
            status = call_arguments(provider, out_result);
        else if (strcmp(function_name, "initial-cwd") == 0)
            status = call_initial_cwd(provider, out_result);
        else
            status = TURBOWASM_UNSUPPORTED;
    } else if (strcmp(package_name, "wasi:cli") == 0 &&
               strcmp(interface_name, "exit") == 0 &&
               strcmp(function_name, "exit") == 0) {
        if (provider->config.exit == NULL) {
            status = TURBOWASM_UNSUPPORTED;
        } else {
            status = provider->config.exit(
                provider->config.context,
                !arguments[0].as.result.is_error);
            if (status == TURBOWASM_OK)
                status = TURBOWASM_INTERRUPTED;
        }
    } else {
        status = TURBOWASM_UNSUPPORTED;
    }

    if (status == TURBOWASM_OK &&
        function_desc->result != NULL &&
        !turbowasm_wasi02_value_matches_type(
            function_desc->result, out_result)) {
        turbowasm_wasi02_value_destroy(out_result);
        status = TURBOWASM_TYPE_MISMATCH;
    } else if (status != TURBOWASM_OK &&
               status != TURBOWASM_INTERRUPTED &&
               out_result != NULL) {
        turbowasm_wasi02_value_destroy(out_result);
    }

    turbowasm_runtime_scope_leave(scope);
    return status;
}
