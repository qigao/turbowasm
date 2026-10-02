#include <turbowasm/component.h>

#include "component_api_internal.h"
#include "component_type_graph.h"
#include "runtime_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TW_COMPONENT_PUBLIC_KIND_MATCH(public_kind, internal_kind) \
    _Static_assert( \
        (int)(public_kind) == (int)(internal_kind), \
        "public/internal Component value kind drift")

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_BOOL, TURBOWASM_COMPONENT_TYPE_BOOL);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S8, TURBOWASM_COMPONENT_TYPE_S8);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U8, TURBOWASM_COMPONENT_TYPE_U8);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S16, TURBOWASM_COMPONENT_TYPE_S16);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U16, TURBOWASM_COMPONENT_TYPE_U16);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S32, TURBOWASM_COMPONENT_TYPE_S32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U32, TURBOWASM_COMPONENT_TYPE_U32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S64, TURBOWASM_COMPONENT_TYPE_S64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U64, TURBOWASM_COMPONENT_TYPE_U64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_F32, TURBOWASM_COMPONENT_TYPE_F32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_F64, TURBOWASM_COMPONENT_TYPE_F64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_CHAR, TURBOWASM_COMPONENT_TYPE_CHAR);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_STRING, TURBOWASM_COMPONENT_TYPE_STRING);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_LIST, TURBOWASM_COMPONENT_TYPE_LIST);

#undef TW_COMPONENT_PUBLIC_KIND_MATCH

turbowasm_component_public_impl *
turbowasm_component_public_impl_get(
    const turbowasm_component *component) {
    return component != NULL
        ? (turbowasm_component_public_impl *)component->impl
        : NULL;
}

turbowasm_component_instance_public_impl *
turbowasm_component_instance_public_impl_get(
    const turbowasm_component_instance *instance) {
    return instance != NULL
        ? (turbowasm_component_instance_public_impl *)instance->impl
        : NULL;
}

bool turbowasm_component_public_impl_retain(
    turbowasm_component_public_impl *impl) {
    if (impl == NULL || impl->ref_count == 0u ||
        impl->ref_count == UINT32_MAX)
        return false;
    ++impl->ref_count;
    return true;
}

void turbowasm_component_public_impl_release(
    turbowasm_component_public_impl *impl) {
    turbowasm_runtime_config config;
    turbowasm_runtime_scope scope;

    if (impl == NULL || impl->ref_count == 0u)
        return;

    --impl->ref_count;
    if (impl->ref_count != 0u)
        return;

    config = impl->binary.config;
    turbowasm_component_binary_destroy(&impl->binary);

    scope = turbowasm_runtime_scope_enter(&config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
}

static bool public_kind_supported(
    turbowasm_component_host_value_kind kind) {
    return kind >= TURBOWASM_COMPONENT_HOST_BOOL &&
           kind <= TURBOWASM_COMPONENT_HOST_LIST;
}

static bool public_type_ref_supported(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t depth) {
    const turbowasm_component_type *type;
    turbowasm_component_type_kind kind;

    if (graph == NULL || depth >= 64u)
        return false;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE) {
        kind = ref.as.inline_type;
        return kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
               kind <= TURBOWASM_COMPONENT_TYPE_STRING;
    }

    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;
    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (type == NULL)
        return false;

    if (type->kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
        type->kind <= TURBOWASM_COMPONENT_TYPE_STRING)
        return true;
    if (type->kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return public_type_ref_supported(
            graph, type->as.list.element_type, depth + 1u);
    return false;
}

static void internal_input_destroy(
    turbowasm_component_value *value) {
    uint64_t i;

    if (value == NULL)
        return;

    if (value->kind == TURBOWASM_COMPONENT_TYPE_LIST) {
        for (i = 0u; i < value->as.list.count; ++i)
            internal_input_destroy(&value->as.list.items[i]);
        turbowasm_rt_free(value->as.list.items);
    }

    /*
     * Input strings borrow caller bytes and therefore must never flow through
     * turbowasm_component_value_destroy(), which owns lifted string storage.
     */
    memset(value, 0, sizeof(*value));
}

static turbowasm_status public_to_internal(
    const turbowasm_component_host_value *source,
    turbowasm_component_value *out,
    uint32_t depth) {
    size_t i;

    if (source == NULL || out == NULL ||
        !public_kind_supported(source->kind))
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= 64u)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = (turbowasm_component_type_kind)source->kind;

    switch (source->kind) {
        case TURBOWASM_COMPONENT_HOST_BOOL:
            out->as.boolean = source->as.boolean;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S8:
            out->as.s8 = source->as.s8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U8:
            out->as.u8 = source->as.u8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S16:
            out->as.s16 = source->as.s16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U16:
            out->as.u16 = source->as.u16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S32:
            out->as.s32 = source->as.s32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U32:
            out->as.u32 = source->as.u32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S64:
            out->as.s64 = source->as.s64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U64:
            out->as.u64 = source->as.u64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_F32:
            out->as.f32 = source->as.f32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_F64:
            out->as.f64 = source->as.f64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_CHAR:
            out->as.character = source->as.character;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_HOST_STRING:
            if (source->as.string.size != 0u &&
                source->as.string.data == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            out->as.string.data = source->as.string.data;
            out->as.string.size = source->as.string.size;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_HOST_LIST:
            if (source->as.list.count != 0u &&
                source->as.list.items == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            if (source->as.list.count >
                SIZE_MAX / sizeof(*out->as.list.items))
                return TURBOWASM_OUT_OF_MEMORY;

            if (source->as.list.count != 0u) {
                out->as.list.items =
                    (turbowasm_component_value *)turbowasm_rt_calloc(
                        source->as.list.count,
                        sizeof(*out->as.list.items));
                if (out->as.list.items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            out->as.list.count = source->as.list.count;

            for (i = 0u; i < source->as.list.count; ++i) {
                turbowasm_status status = public_to_internal(
                    &source->as.list.items[i],
                    &out->as.list.items[i],
                    depth + 1u);
                if (status != TURBOWASM_OK) {
                    internal_input_destroy(out);
                    return status;
                }
            }
            return TURBOWASM_OK;

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status internal_to_public(
    turbowasm_component_value *source,
    turbowasm_component_host_value *out,
    uint32_t depth) {
    uint64_t i;

    if (source == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= 64u)
        return TURBOWASM_TRAPPED;
    if (source->kind < TURBOWASM_COMPONENT_TYPE_BOOL ||
        source->kind > TURBOWASM_COMPONENT_TYPE_LIST)
        return TURBOWASM_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    out->kind =
        (turbowasm_component_host_value_kind)source->kind;

    switch (source->kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            out->as.boolean = source->as.boolean;
            break;
        case TURBOWASM_COMPONENT_TYPE_S8:
            out->as.s8 = source->as.s8;
            break;
        case TURBOWASM_COMPONENT_TYPE_U8:
            out->as.u8 = source->as.u8;
            break;
        case TURBOWASM_COMPONENT_TYPE_S16:
            out->as.s16 = source->as.s16;
            break;
        case TURBOWASM_COMPONENT_TYPE_U16:
            out->as.u16 = source->as.u16;
            break;
        case TURBOWASM_COMPONENT_TYPE_S32:
            out->as.s32 = source->as.s32;
            break;
        case TURBOWASM_COMPONENT_TYPE_U32:
            out->as.u32 = source->as.u32;
            break;
        case TURBOWASM_COMPONENT_TYPE_S64:
            out->as.s64 = source->as.s64;
            break;
        case TURBOWASM_COMPONENT_TYPE_U64:
            out->as.u64 = source->as.u64;
            break;
        case TURBOWASM_COMPONENT_TYPE_F32:
            out->as.f32 = source->as.f32;
            break;
        case TURBOWASM_COMPONENT_TYPE_F64:
            out->as.f64 = source->as.f64;
            break;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            out->as.character = source->as.character;
            break;

        case TURBOWASM_COMPONENT_TYPE_STRING:
            out->as.string.data = source->as.string.data;
            out->as.string.size = source->as.string.size;
            source->as.string.data = NULL;
            source->as.string.size = 0u;
            break;

        case TURBOWASM_COMPONENT_TYPE_LIST:
            if (source->as.list.count > (uint64_t)SIZE_MAX)
                return TURBOWASM_OUT_OF_MEMORY;
            if (source->as.list.count != 0u) {
                out->as.list.items =
                    (turbowasm_component_host_value *)turbowasm_rt_calloc(
                        (size_t)source->as.list.count,
                        sizeof(*out->as.list.items));
                if (out->as.list.items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            out->as.list.count = (size_t)source->as.list.count;

            for (i = 0u; i < source->as.list.count; ++i) {
                turbowasm_status status = internal_to_public(
                    &source->as.list.items[i],
                    &out->as.list.items[i],
                    depth + 1u);
                if (status != TURBOWASM_OK) {
                    turbowasm_component_host_value_destroy(out);
                    return status;
                }
            }

            turbowasm_rt_free(source->as.list.items);
            source->as.list.items = NULL;
            source->as.list.count = 0u;
            break;

        default:
            return TURBOWASM_UNSUPPORTED;
    }

    memset(source, 0, sizeof(*source));
    return TURBOWASM_OK;
}

void turbowasm_component_host_value_destroy(
    turbowasm_component_host_value *value) {
    size_t i;

    if (value == NULL)
        return;

    if (value->kind == TURBOWASM_COMPONENT_HOST_STRING) {
        turbowasm_rt_free(value->as.string.data);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_LIST) {
        for (i = 0u; i < value->as.list.count; ++i)
            turbowasm_component_host_value_destroy(
                &value->as.list.items[i]);
        turbowasm_rt_free(value->as.list.items);
    }

    memset(value, 0, sizeof(*value));
}

turbowasm_status turbowasm_component_load_borrowed(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size) {
    return turbowasm_component_load_borrowed_with_config(
        component, bytes, size, NULL);
}

turbowasm_status turbowasm_component_load_borrowed_with_config(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_component_public_impl *impl;
    turbowasm_status status;

    if (component == NULL || bytes == NULL ||
        component->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(&normalized);
    impl = (turbowasm_component_public_impl *)turbowasm_rt_calloc(
        1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    status = turbowasm_component_binary_load_with_config(
        &impl->binary, bytes, size, &normalized);
    if (status != TURBOWASM_OK) {
        scope = turbowasm_runtime_scope_enter(&normalized);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        return status;
    }

    impl->ref_count = 1u;
    component->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_component_destroy(
    turbowasm_component *component) {
    turbowasm_component_public_impl *impl;

    if (component == NULL)
        return;
    impl = turbowasm_component_public_impl_get(component);
    component->impl = NULL;
    turbowasm_component_public_impl_release(impl);
}

turbowasm_status turbowasm_component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component) {
    turbowasm_component_public_impl *component_state;
    turbowasm_component_instance_public_impl *impl;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (instance == NULL || component == NULL ||
        instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    component_state = turbowasm_component_public_impl_get(component);
    if (component_state == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(
        &component_state->binary.config);
    impl = (turbowasm_component_instance_public_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if (!turbowasm_component_public_impl_retain(
            component_state)) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    impl->component = component_state;

    status = turbowasm_component_exec_init(
        &impl->exec, &component_state->binary);
    if (status != TURBOWASM_OK) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        turbowasm_component_public_impl_release(component_state);
        return status;
    }

    instance->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_component_instance_destroy(
    turbowasm_component_instance *instance) {
    turbowasm_component_instance_public_impl *impl;
    turbowasm_component_public_impl *component_state;
    turbowasm_runtime_config config;
    turbowasm_runtime_scope scope;

    if (instance == NULL)
        return;

    impl = turbowasm_component_instance_public_impl_get(instance);
    instance->impl = NULL;
    if (impl == NULL)
        return;

    component_state = impl->component;
    config = component_state->binary.config;

    turbowasm_component_exec_destroy(&impl->exec);

    if (impl->owner_release != NULL)
        impl->owner_release(impl->owner_context);

    scope = turbowasm_runtime_scope_enter(&config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);

    turbowasm_component_public_impl_release(component_state);
}

static bool public_export_result_type(
    const turbowasm_component_instance_public_impl *instance,
    turbowasm_name export_name,
    bool *out_has_result) {
    const turbowasm_component_binary *binary;
    uint32_t i;

    if (instance == NULL || out_has_result == NULL)
        return false;

    binary = instance->exec.binary;
    if (binary == NULL)
        return false;

    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &binary->exports[i];
        uint32_t adapter_index;
        const turbowasm_component_core_call_adapter *adapter;
        const turbowasm_component_type *function;

        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->name.size != export_name.size ||
            (export_name.size != 0u &&
             (export_name.bytes == NULL ||
              memcmp(
                  export_desc->name.bytes,
                  export_name.bytes,
                  export_name.size) != 0)))
            continue;

        if (export_desc->item_index >=
                instance->exec.function_count ||
            instance->exec.function_adapter_indices == NULL)
            return false;

        adapter_index =
            instance->exec.function_adapter_indices[
                export_desc->item_index];
        if (adapter_index == UINT32_MAX ||
            adapter_index >= instance->exec.adapter_count)
            return false;

        adapter = &instance->exec.functions[adapter_index];
        function = turbowasm_component_type_graph_get(
            adapter->graph, adapter->function_type);
        if (function == NULL ||
            function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
            return false;

        {
            uint32_t param_index;
            for (param_index = 0u;
                 param_index < function->as.function.param_count;
                 ++param_index) {
                if (!public_type_ref_supported(
                        adapter->graph,
                        function->as.function.params[param_index],
                        0u))
                    return false;
            }
        }

        if (function->as.function.has_result &&
            !public_type_ref_supported(
                adapter->graph,
                function->as.function.result,
                0u))
            return false;

        *out_has_result = function->as.function.has_result;
        return true;
    }

    return false;
}

turbowasm_status turbowasm_component_instance_invoke(
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count,
    turbowasm_component_host_value *result,
    size_t result_capacity,
    size_t *out_result_count,
    turbowasm_trap *trap) {
    turbowasm_component_instance_public_impl *impl;
    turbowasm_component_value *internal_arguments = NULL;
    turbowasm_component_value internal_result = {0};
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    bool has_result;
    size_t i;

    if (instance == NULL || out_result_count == NULL ||
        trap == NULL ||
        (export_name.size != 0u && export_name.bytes == NULL) ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    impl = turbowasm_component_instance_public_impl_get(instance);
    if (impl == NULL || impl->component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!public_export_result_type(
            impl, export_name, &has_result))
        return TURBOWASM_UNSUPPORTED;

    if (has_result) {
        if (result == NULL || result_capacity < 1u)
            return TURBOWASM_INVALID_ARGUMENT;
        memset(result, 0, sizeof(*result));
    }

    *out_result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    scope = turbowasm_runtime_scope_enter(
        &impl->component->binary.config);

    if (argument_count != 0u) {
        internal_arguments =
            (turbowasm_component_value *)turbowasm_rt_calloc(
                argument_count,
                sizeof(*internal_arguments));
        if (internal_arguments == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto done;
        }
    }

    for (i = 0u; i < argument_count; ++i) {
        status = public_to_internal(
            &arguments[i], &internal_arguments[i], 0u);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_component_exec_invoke_export(
        &impl->exec,
        export_name.bytes,
        export_name.size,
        internal_arguments,
        argument_count,
        has_result ? &internal_result : NULL,
        trap);
    if (status != TURBOWASM_OK)
        goto done;

    if (has_result) {
        status = internal_to_public(
            &internal_result, result, 0u);
        if (status != TURBOWASM_OK)
            goto done;
        *out_result_count = 1u;
    }

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i]);
        turbowasm_rt_free(internal_arguments);
    }
    if (internal_result.kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(&internal_result);

    turbowasm_runtime_scope_leave(scope);
    return status;
}


typedef struct turbowasm_component_call_public_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_exec_call call;
    bool has_result;
} turbowasm_component_call_public_impl;

static turbowasm_component_call_public_impl *
component_call_public_impl_mut(
    turbowasm_component_call *call) {
    return call != NULL
        ? (turbowasm_component_call_public_impl *)call->impl
        : NULL;
}

static const turbowasm_component_call_public_impl *
component_call_public_impl_get(
    const turbowasm_component_call *call) {
    return call != NULL
        ? (const turbowasm_component_call_public_impl *)call->impl
        : NULL;
}

turbowasm_status turbowasm_component_call_create(
    turbowasm_component_call *call,
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count) {
    turbowasm_component_instance_public_impl *instance_impl;
    turbowasm_component_call_public_impl *call_impl = NULL;
    turbowasm_component_value *internal_arguments = NULL;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    bool has_result = false;
    size_t i;

    if (call == NULL || call->impl != NULL ||
        instance == NULL ||
        (export_name.size != 0u && export_name.bytes == NULL) ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    instance_impl =
        turbowasm_component_instance_public_impl_get(instance);
    if (instance_impl == NULL || instance_impl->component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!public_export_result_type(
            instance_impl, export_name, &has_result))
        return TURBOWASM_UNSUPPORTED;

    scope = turbowasm_runtime_scope_enter(
        &instance_impl->component->binary.config);

    call_impl =
        (turbowasm_component_call_public_impl *)
            turbowasm_rt_calloc(1u, sizeof(*call_impl));
    if (call_impl == NULL) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    if (argument_count != 0u) {
        internal_arguments =
            (turbowasm_component_value *)turbowasm_rt_calloc(
                argument_count, sizeof(*internal_arguments));
        if (internal_arguments == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto done;
        }
    }

    for (i = 0u; i < argument_count; ++i) {
        status = public_to_internal(
            &arguments[i], &internal_arguments[i], 0u);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_component_exec_call_create(
        &call_impl->call,
        &instance_impl->exec,
        export_name.bytes,
        export_name.size,
        internal_arguments,
        argument_count);
    if (status != TURBOWASM_OK)
        goto done;

    call_impl->instance = instance_impl;
    call_impl->has_result = has_result;
    call->impl = call_impl;
    call_impl = NULL;

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i]);
        turbowasm_rt_free(internal_arguments);
    }
    if (call_impl != NULL) {
        turbowasm_component_exec_call_destroy(
            &call_impl->call);
        turbowasm_rt_free(call_impl);
    }
    turbowasm_runtime_scope_leave(scope);
    return status;
}

void turbowasm_component_call_destroy(
    turbowasm_component_call *call) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);
    turbowasm_runtime_scope scope;

    if (impl == NULL)
        return;

    if (impl->instance == NULL ||
        impl->instance->component == NULL) {
        call->impl = NULL;
        return;
    }

    scope = turbowasm_runtime_scope_enter(
        &impl->instance->component->binary.config);
    turbowasm_component_exec_call_destroy(&impl->call);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    call->impl = NULL;
}

turbowasm_status turbowasm_component_call_resume(
    turbowasm_component_call *call,
    const turbowasm_execution_options *options) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);

    if (impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_exec_call_resume(
        &impl->call, options);
}

turbowasm_execution_state turbowasm_component_call_state_get(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_state_get(
              &impl->call)
        : TURBOWASM_EXECUTION_FAILED;
}

turbowasm_yield_reason turbowasm_component_call_yield_reason_get(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_yield_reason_get(
              &impl->call)
        : TURBOWASM_YIELD_NONE;
}

bool turbowasm_component_call_pending_host_wait(
    const turbowasm_component_call *call,
    turbowasm_host_wait *out_wait) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL &&
        turbowasm_component_exec_call_pending_host_wait(
            &impl->call, out_wait);
}

turbowasm_status turbowasm_component_call_complete_host_wait(
    turbowasm_component_call *call,
    turbowasm_host_wait wait,
    int status) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);

    if (impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_exec_call_complete_host_wait(
        &impl->call, wait, status);
}

turbowasm_status turbowasm_component_call_terminal_status(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_terminal_status(
              &impl->call)
        : TURBOWASM_INVALID_ARGUMENT;
}

turbowasm_trap turbowasm_component_call_trap(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_trap(
              &impl->call)
        : TURBOWASM_TRAP_NONE;
}

size_t turbowasm_component_call_result_count(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_result_count(
              &impl->call)
        : 0u;
}

turbowasm_status turbowasm_component_call_take_result(
    turbowasm_component_call *call,
    turbowasm_component_host_value *out_result) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);
    turbowasm_component_value internal_result = {0};
    turbowasm_status status;

    if (impl == NULL || out_result == NULL ||
        !impl->has_result ||
        turbowasm_component_exec_call_result_count(
            &impl->call) != 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_result, 0, sizeof(*out_result));
    status = turbowasm_component_exec_call_take_result(
        &impl->call, &internal_result);
    if (status != TURBOWASM_OK)
        return status;

    status = internal_to_public(
        &internal_result, out_result, 0u);
    if (internal_result.kind !=
        TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(
            &internal_result);
    if (status != TURBOWASM_OK)
        turbowasm_component_host_value_destroy(
            out_result);
    return status;
}
