#include "wasi02_component.h"

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

static const turbowasm_wasi02_type_desc *
wasi02_scalar_base(const turbowasm_wasi02_type_desc *type) {
    uint32_t depth = 0u;

    while (type != NULL &&
           type->kind == TURBOWASM_WASI02_TYPE_ALIAS) {
        if (++depth > 16u)
            return NULL;
        type = type->as.alias.target;
    }

    if (type == NULL)
        return NULL;
    switch (type->kind) {
        case TURBOWASM_WASI02_TYPE_BOOL:
        case TURBOWASM_WASI02_TYPE_U8:
        case TURBOWASM_WASI02_TYPE_U32:
        case TURBOWASM_WASI02_TYPE_U64:
            return type;
        default:
            return NULL;
    }
}

static const turbowasm_component_type *
component_scalar_base(
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

static bool scalar_types_match(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref component_ref,
    const turbowasm_wasi02_type_desc *wasi_type) {
    turbowasm_component_type inline_storage;
    const turbowasm_component_type *component_type =
        component_scalar_base(
            graph, component_ref, &inline_storage);
    const turbowasm_wasi02_type_desc *base =
        wasi02_scalar_base(wasi_type);

    if (component_type == NULL || base == NULL)
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
        if (!scalar_types_match(
                graph,
                function_type->as.function.params[i],
                function->params[i].type))
            return false;
    }

    if (function->result != NULL &&
        !scalar_types_match(
            graph,
            function_type->as.function.result,
            function->result))
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

static turbowasm_status component_to_wasi_scalar(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_component_value *value,
    turbowasm_wasi02_value *out) {
    const turbowasm_wasi02_type_desc *base =
        wasi02_scalar_base(type);

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
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status wasi_to_component_scalar(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    turbowasm_component_value *out) {
    const turbowasm_wasi02_type_desc *base =
        wasi02_scalar_base(type);

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
        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status wasi02_component_invoke(
    void *context,
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
    size_t i;
    turbowasm_status status;

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

    for (i = 0u; i < argument_count; ++i) {
        status = component_to_wasi_scalar(
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
        status = wasi_to_component_scalar(
            function->result,
            &wasi_result,
            out_result);
    }

done:
    for (i = 0u; i < argument_count; ++i)
        turbowasm_wasi02_value_destroy(&wasi_arguments[i]);
    turbowasm_wasi02_value_destroy(&wasi_result);
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
