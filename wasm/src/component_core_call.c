#include "component_core_call.h"
#include "component_string.h"
#include "execution_internal.h"

#include "instance_internal.h"
#include "module_internal.h"
#include "runtime_alloc.h"

#include <cmeta/cmeta.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool type_ref_is_resource_handle(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    const turbowasm_component_type *type;

    if (graph == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    return type != NULL &&
           (type->kind == TURBOWASM_COMPONENT_TYPE_OWN ||
            type->kind == TURBOWASM_COMPONENT_TYPE_BORROW);
}

static turbowasm_status resource_handle_info(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind *out_kind,
    uint64_t *out_identity,
    turbowasm_value_kind *out_rep_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;

    if (graph == NULL || out_kind == NULL ||
        out_identity == NULL || out_rep_kind == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return TURBOWASM_INVALID_ARGUMENT;

    handle_type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (handle_type == NULL ||
        (handle_type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
         handle_type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return TURBOWASM_TYPE_MISMATCH;

    resource_type = turbowasm_component_resource_definition(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    if (resource_type->as.resource.identity_alias || resource_type->as.resource.rep_type == 0x7fu) {
        *out_rep_kind = TURBOWASM_VALUE_I32;
    } else if (resource_type->as.resource.rep_type == 0x7eu) {
        *out_rep_kind = TURBOWASM_VALUE_I64;
    } else {
        return TURBOWASM_UNSUPPORTED;
    }

    *out_kind = handle_type->kind;
    *out_identity = resource_type->as.resource.identity;
    return TURBOWASM_OK;
}

static turbowasm_status lower_resource_argument(
    const turbowasm_component_core_call_adapter *adapter,
    turbowasm_component_type_ref ref,
    const turbowasm_component_value *value,
    turbowasm_component_resource_handle *out_handle,
    uint64_t *out_identity,
    bool *out_borrowed,
    turbowasm_value *out_flat) {
    turbowasm_component_type_kind kind;
    turbowasm_value_kind rep_kind;
    uint64_t identity;
    turbowasm_component_resource_handle handle;
    turbowasm_status status;

    if (adapter == NULL || adapter->resources == NULL ||
        value == NULL || out_handle == NULL ||
        out_identity == NULL || out_borrowed == NULL ||
        out_flat == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = resource_handle_info(
        adapter->graph, ref, &kind, &identity, &rep_kind);
    if (status != TURBOWASM_OK)
        return status;
    if (value->kind != kind ||
        value->as.resource_rep.kind != rep_kind)
        return TURBOWASM_TYPE_MISMATCH;

    if (kind == TURBOWASM_COMPONENT_TYPE_OWN) {
        status = turbowasm_component_resource_new_owned(
            adapter->resources, identity,
            value->as.resource_rep, &handle);
        *out_borrowed = false;
    } else {
        status = turbowasm_component_resource_new_borrowed(
            adapter->resources, identity,
            value->as.resource_rep, &handle);
        *out_borrowed = true;
    }
    if (status != TURBOWASM_OK)
        return status;

    memset(out_flat, 0, sizeof(*out_flat));
    out_flat->kind = TURBOWASM_VALUE_I32;
    out_flat->as.i32 = (int32_t)handle;
    *out_handle = handle;
    *out_identity = identity;
    return TURBOWASM_OK;
}

static turbowasm_status lift_owned_result(
    const turbowasm_component_core_call_adapter *adapter,
    turbowasm_component_type_ref ref,
    const turbowasm_value *flat,
    turbowasm_component_value *out) {
    turbowasm_component_type_kind kind;
    turbowasm_value_kind rep_kind;
    uint64_t identity;
    turbowasm_component_resource_handle handle;
    turbowasm_value rep = {0};
    turbowasm_status status;

    if (adapter == NULL || adapter->resources == NULL ||
        flat == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = resource_handle_info(
        adapter->graph, ref, &kind, &identity, &rep_kind);
    if (status != TURBOWASM_OK)
        return status;
    if (kind != TURBOWASM_COMPONENT_TYPE_OWN)
        return TURBOWASM_MALFORMED_MODULE;
    if (flat->kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle = (uint32_t)flat->as.i32;
    status = turbowasm_component_resource_take_owned(
        adapter->resources, handle, identity, &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (rep.kind != rep_kind)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_COMPONENT_TYPE_OWN;
    out->as.resource_rep = rep;
    out->resource_identity = identity;
    return TURBOWASM_OK;
}

typedef struct turbowasm_component_call_resource_entry {
    turbowasm_component_resource_handle handle;
    uint64_t identity;
    bool borrowed;
} turbowasm_component_call_resource_entry;

typedef struct turbowasm_component_call_resource_scope {
    const turbowasm_component_core_call_adapter *adapter;
    turbowasm_component_call_resource_entry *entries;
    uint32_t count;
    uint32_t capacity;
} turbowasm_component_call_resource_scope;

typedef struct turbowasm_component_resource_codec_context {
    const turbowasm_component_core_call_adapter *adapter;
    turbowasm_component_call_resource_scope *scope;
} turbowasm_component_resource_codec_context;

static const turbowasm_runtime_config *adapter_runtime_config(
    const turbowasm_component_core_call_adapter *adapter) {
    const turbowasm_module *module;
    const turbowasm_module_impl *impl;

    if (adapter == NULL || adapter->instance == NULL)
        return NULL;
    module = turbowasm_instance_module(adapter->instance);
    impl = turbowasm_module_impl_get(module);
    return impl != NULL ? &impl->config : NULL;
}

static turbowasm_status resource_scope_reserve(
    turbowasm_component_call_resource_scope *scope,
    uint32_t required) {
    const turbowasm_runtime_config *config;
    turbowasm_runtime_scope alloc_scope;
    turbowasm_component_call_resource_entry *grown;
    uint32_t next;

    if (scope == NULL || scope->adapter == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (required > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS)
        return TURBOWASM_OUT_OF_MEMORY;
    if (required <= scope->capacity)
        return TURBOWASM_OK;

    next = scope->capacity == 0u ? 8u : scope->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }
    if (next > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS)
        next = TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS;
    if ((size_t)next > SIZE_MAX / sizeof(*grown))
        return TURBOWASM_OUT_OF_MEMORY;

    config = adapter_runtime_config(scope->adapter);
    if (config == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    alloc_scope = turbowasm_runtime_scope_enter(config);
    grown = (turbowasm_component_call_resource_entry *)
        turbowasm_rt_realloc(
            scope->entries, (size_t)next * sizeof(*grown));
    turbowasm_runtime_scope_leave(alloc_scope);
    if (grown == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    scope->entries = grown;
    scope->capacity = next;
    return TURBOWASM_OK;
}

static turbowasm_status resource_scope_record(
    turbowasm_component_call_resource_scope *scope,
    turbowasm_component_resource_handle handle,
    uint64_t identity,
    bool borrowed) {
    turbowasm_status status;

    if (scope == NULL || handle == 0u || identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (scope->count == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;

    status = resource_scope_reserve(scope, scope->count + 1u);
    if (status != TURBOWASM_OK)
        return status;

    scope->entries[scope->count].handle = handle;
    scope->entries[scope->count].identity = identity;
    scope->entries[scope->count].borrowed = borrowed;
    ++scope->count;
    return TURBOWASM_OK;
}

static void resource_scope_rollback(
    turbowasm_component_call_resource_scope *scope) {
    uint32_t i;

    if (scope == NULL || scope->adapter == NULL ||
        scope->adapter->resources == NULL)
        return;

    for (i = 0u; i < scope->count; ++i) {
        const turbowasm_component_call_resource_entry *entry =
            &scope->entries[i];

        if (entry->borrowed) {
            (void)turbowasm_component_resource_drop(
                scope->adapter->resources,
                entry->handle, entry->identity,
                NULL, NULL);
        } else {
            turbowasm_value rep = {0};
            (void)turbowasm_component_resource_take_owned(
                scope->adapter->resources,
                entry->handle, entry->identity, &rep);
        }
    }
}

static void resource_scope_abort_borrows(
    turbowasm_component_call_resource_scope *scope) {
    uint32_t i;

    if (scope == NULL || scope->adapter == NULL ||
        scope->adapter->resources == NULL)
        return;

    for (i = 0u; i < scope->count; ++i) {
        const turbowasm_component_call_resource_entry *entry =
            &scope->entries[i];
        if (entry->borrowed) {
            (void)turbowasm_component_resource_drop(
                scope->adapter->resources,
                entry->handle, entry->identity,
                NULL, NULL);
        }
    }
}

static bool resource_scope_finalize_borrows(
    turbowasm_component_call_resource_scope *scope) {
    uint32_t i;
    bool clean = true;

    if (scope == NULL || scope->adapter == NULL ||
        scope->adapter->resources == NULL)
        return scope == NULL || scope->count == 0u;

    for (i = 0u; i < scope->count; ++i) {
        const turbowasm_component_call_resource_entry *entry =
            &scope->entries[i];
        turbowasm_value rep = {0};
        turbowasm_status status;

        if (!entry->borrowed)
            continue;

        status = turbowasm_component_resource_rep(
            scope->adapter->resources,
            entry->handle, entry->identity, &rep);
        if (status == TURBOWASM_OK) {
            clean = false;
            (void)turbowasm_component_resource_drop(
                scope->adapter->resources,
                entry->handle, entry->identity,
                NULL, NULL);
        } else if (status != TURBOWASM_TRAPPED) {
            clean = false;
        }
    }
    return clean;
}

static void resource_scope_destroy(
    turbowasm_component_call_resource_scope *scope) {
    const turbowasm_runtime_config *config;
    turbowasm_runtime_scope alloc_scope;

    if (scope == NULL)
        return;

    config = adapter_runtime_config(scope->adapter);
    if (scope->entries != NULL && config != NULL) {
        alloc_scope = turbowasm_runtime_scope_enter(config);
        turbowasm_rt_free(scope->entries);
        turbowasm_runtime_scope_leave(alloc_scope);
    }
    memset(scope, 0, sizeof(*scope));
}

static void rollback_one_created_handle(
    const turbowasm_component_core_call_adapter *adapter,
    turbowasm_component_resource_handle handle,
    uint64_t identity,
    bool borrowed) {
    if (adapter == NULL || adapter->resources == NULL)
        return;
    if (borrowed) {
        (void)turbowasm_component_resource_drop(
            adapter->resources, handle, identity,
            NULL, NULL);
    } else {
        turbowasm_value rep = {0};
        (void)turbowasm_component_resource_take_owned(
            adapter->resources, handle, identity, &rep);
    }
}

static turbowasm_status canonical_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_component_resource_codec_context *codec =
        (turbowasm_component_resource_codec_context *)context;
    turbowasm_component_resource_handle handle;
    uint64_t identity;
    bool borrowed;
    turbowasm_value flat = {0};
    turbowasm_status status;

    if (codec == NULL || codec->adapter == NULL ||
        codec->scope == NULL || graph != codec->adapter->graph ||
        out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (codec->adapter->defines_local_resources && value != NULL &&
            value->kind == TURBOWASM_COMPONENT_TYPE_BORROW &&
            ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED) {
        const turbowasm_component_type *type = turbowasm_component_type_graph_get(graph, ref.as.indexed);
        const turbowasm_component_type *resource = type != NULL && type->kind == value->kind
            ? turbowasm_component_resource_definition(graph, type->as.handle.resource_type) : NULL;
        if (resource == NULL)
            return TURBOWASM_TYPE_MISMATCH;
        if (!resource->as.resource.identity_alias) {
            if (value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_TYPE_MISMATCH;
            *out_handle = (uint32_t)value->as.resource_rep.as.i32;
            return TURBOWASM_OK;
        }
    }
    status = lower_resource_argument(
        codec->adapter, ref, value,
        &handle, &identity, &borrowed, &flat);
    if (status != TURBOWASM_OK)
        return status;

    status = resource_scope_record(
        codec->scope, handle, identity, borrowed);
    if (status != TURBOWASM_OK) {
        rollback_one_created_handle(
            codec->adapter, handle, identity, borrowed);
        return status;
    }

    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status canonical_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_component_resource_codec_context *codec = context;
    turbowasm_value flat = {0};
    turbowasm_status status;
    if (codec == NULL || codec->adapter == NULL ||
        graph != codec->adapter->graph || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    flat.kind = TURBOWASM_VALUE_I32;
    flat.as.i32 = (int32_t)handle;
    status = lift_owned_result(codec->adapter, ref, &flat, out);
    if (status == TURBOWASM_OK && codec->adapter->result_owner != NULL)
        status = codec->adapter->result_owner(
            codec->adapter->result_owner_context, graph, ref, out);
    return status;
}

static turbowasm_value_kind flat_value_kind(
    turbowasm_component_flat_type type) {
    switch (type) {
        case TURBOWASM_COMPONENT_FLAT_I32:
            return TURBOWASM_VALUE_I32;
        case TURBOWASM_COMPONENT_FLAT_I64:
            return TURBOWASM_VALUE_I64;
        case TURBOWASM_COMPONENT_FLAT_F32:
            return TURBOWASM_VALUE_F32;
        case TURBOWASM_COMPONENT_FLAT_F64:
            return TURBOWASM_VALUE_F64;
        default:
            return (turbowasm_value_kind)0;
    }
}

static bool core_type_matches(
    const cmeta_type_desc *actual,
    turbowasm_component_flat_type expected) {
    turbowasm_value_kind kind = flat_value_kind(expected);
    const cmeta_type_desc *wanted;

    if (kind == 0 || actual == NULL)
        return false;
    wanted = turbowasm_value_type_descriptor(kind);
    return wanted != NULL && cmeta_type_equal(actual, wanted);
}

static turbowasm_status validate_core_signature(
    const turbowasm_component_core_call_adapter *adapter) {
    const turbowasm_module *module;
    turbowasm_function_signature signature;
    uint32_t i;

    if (adapter == NULL || adapter->instance == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    module = turbowasm_instance_module(adapter->instance);
    if (module == NULL ||
        !turbowasm_module_function_signature_get(
            module, adapter->function_index, &signature))
        return TURBOWASM_INVALID_ARGUMENT;

    if (signature.param_count !=
            adapter->flat_signature.param_count ||
        signature.result_count !=
            adapter->flat_signature.result_count)
        return TURBOWASM_TYPE_MISMATCH;

    for (i = 0u; i < signature.param_count; ++i) {
        if (!core_type_matches(
                turbowasm_module_function_param_type(
                    module, adapter->function_index, i),
                adapter->flat_signature.params[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    for (i = 0u; i < signature.result_count; ++i) {
        if (!core_type_matches(
                turbowasm_module_function_result_type(
                    module, adapter->function_index, i),
                adapter->flat_signature.results[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }

    return TURBOWASM_OK;
}

static turbowasm_status pointer_to_core(
    turbowasm_component_pointer_type type,
    uint64_t pointer,
    turbowasm_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (pointer > UINT32_MAX)
            return TURBOWASM_TRAPPED;
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)pointer;
        return TURBOWASM_OK;
    }
    if (type == TURBOWASM_COMPONENT_POINTER_I64) {
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)pointer;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status pointer_from_core(
    turbowasm_component_pointer_type type,
    const turbowasm_value *value,
    uint64_t *out) {
    if (value == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value->kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint32_t)value->as.i32;
        return TURBOWASM_OK;
    }
    if (type == TURBOWASM_COMPONENT_POINTER_I64) {
        if (value->kind != TURBOWASM_VALUE_I64)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint64_t)value->as.i64;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status validate_memory_binding(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_canonical_memory *memory,
    bool needs_realloc) {
    const turbowasm_module *module;
    turbowasm_memory_desc desc;

    if (!adapter->uses_memory)
        return TURBOWASM_OK;
    if (memory == NULL ||
        memory->instance == NULL ||
        memory->instance->impl == NULL ||
        !turbowasm_component_string_encoding_valid(memory->string_encoding))
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Canonical memory is a Core memory option, not necessarily a memory owned
     * by the lifted callee's Core instance.
     */
    module = turbowasm_instance_module(memory->instance);
    if (module == NULL ||
        !turbowasm_module_memory_at(
            module, memory->memory_index, &desc))
        return TURBOWASM_INVALID_ARGUMENT;

    if ((desc.memory64 &&
         memory->pointer_type != TURBOWASM_COMPONENT_POINTER_I64) ||
        (!desc.memory64 &&
         memory->pointer_type != TURBOWASM_COMPONENT_POINTER_I32))
        return TURBOWASM_TYPE_MISMATCH;

    if (needs_realloc && memory->guest_realloc == NULL)
        return TURBOWASM_UNSUPPORTED;

    adapter->memory = *memory;
    return TURBOWASM_OK;
}

static turbowasm_status lower_indirect_parameters(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *arguments,
    uint64_t *out_pointer) {
    turbowasm_component_layout layout;
    uint64_t pointer;
    turbowasm_status status;
    if (adapter == NULL || memory == NULL || arguments == NULL || out_pointer == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (memory->guest_realloc == NULL)
        return TURBOWASM_UNSUPPORTED;
    status = turbowasm_component_canonical_parameter_layout(adapter->graph,
        adapter->function_type, memory->pointer_type, &layout);
    if (status != TURBOWASM_OK) return status;
    status = memory->guest_realloc(memory->realloc_context,
        0u, 0u, layout.alignment, layout.size, &pointer);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_component_canonical_lower_parameters(adapter->graph,
        adapter->function_type, memory, pointer, arguments);
    if (status == TURBOWASM_OK) *out_pointer = pointer;
    return status;
}

turbowasm_status turbowasm_component_core_call_adapter_init(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_component_canonical_memory *memory) {
    return turbowasm_component_core_call_adapter_init_with_resources(
        adapter, graph, function_type,
        instance, function_index, memory, NULL);
}

turbowasm_status turbowasm_component_core_call_adapter_init_with_resources(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_component_canonical_memory *memory,
    turbowasm_component_resource_table *resources) {
    const turbowasm_component_type *function;
    bool uses_memory = false;
    bool uses_resources = false;
    bool needs_realloc = false;
    uint32_t i;
    turbowasm_status status;

    if (adapter == NULL || graph == NULL ||
        instance == NULL || instance->impl == NULL ||
        adapter->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_type_graph_validate(graph))
        return TURBOWASM_MALFORMED_MODULE;

    function = turbowasm_component_type_graph_get(
        graph, function_type);
    if (function == NULL ||
        function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < function->as.function.param_count; ++i) {
        uint32_t features;
        if (!turbowasm_component_value_type_features(
                graph, function->as.function.params[i], &features))
            return TURBOWASM_UNSUPPORTED;
        if ((features & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u) {
            uses_memory = true;
            needs_realloc = true;
        }
        if ((features & TURBOWASM_COMPONENT_VALUE_RESOURCES) != 0u)
            uses_resources = true;
    }

    if (function->as.function.has_result) {
        turbowasm_component_type_kind result_kind = TURBOWASM_COMPONENT_TYPE_UNDEFINED;
        uint64_t result_identity = 0u;
        turbowasm_value_kind result_rep_kind = (turbowasm_value_kind)0;

        uint32_t features;
        if (!turbowasm_component_value_type_features(
                graph, function->as.function.result, &features))
            return TURBOWASM_UNSUPPORTED;
        if ((features & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u)
            uses_memory = true;
        if ((features & TURBOWASM_COMPONENT_VALUE_RESOURCES) != 0u) {
            uses_resources = true;
            if (type_ref_is_resource_handle(
                    graph, function->as.function.result)) {
                status = resource_handle_info(
                    graph, function->as.function.result,
                    &result_kind, &result_identity, &result_rep_kind);
                if (status != TURBOWASM_OK)
                    return status;
                if (result_kind == TURBOWASM_COMPONENT_TYPE_BORROW)
                    return TURBOWASM_MALFORMED_MODULE;
            }
        }
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->graph = graph;
    adapter->function_type = function_type;
    adapter->instance = instance;
    adapter->function_index = function_index;
    adapter->uses_memory = uses_memory;
    adapter->uses_resources = uses_resources;
    adapter->resources = resources;

    status = turbowasm_component_canonical_flatten_function(
        graph, function_type,
        memory != NULL
            ? memory->pointer_type
            : TURBOWASM_COMPONENT_POINTER_I32,
        TURBOWASM_COMPONENT_CANONICAL_LIFT,
        &adapter->flat_signature);
    if (status != TURBOWASM_OK)
        goto fail;

    if (adapter->flat_signature.params_indirect) {
        adapter->uses_memory = true;
        needs_realloc = true;
    }
    if (adapter->flat_signature.results_indirect)
        adapter->uses_memory = true;

    if (uses_resources && resources == NULL) {
        status = TURBOWASM_UNSUPPORTED;
        goto fail;
    }

    status = validate_memory_binding(
        adapter, memory, needs_realloc);
    if (status != TURBOWASM_OK)
        goto fail;

    status = validate_core_signature(adapter);
    if (status != TURBOWASM_OK)
        goto fail;

    adapter->initialized = true;
    return TURBOWASM_OK;

fail:
    memset(adapter, 0, sizeof(*adapter));
    return status;
}

void turbowasm_component_core_call_adapter_destroy(
    turbowasm_component_core_call_adapter *adapter) {
    if (adapter == NULL)
        return;
    memset(adapter, 0, sizeof(*adapter));
}

turbowasm_status turbowasm_component_core_call_set_post_return(
    turbowasm_component_core_call_adapter *adapter,
    turbowasm_instance *instance, uint32_t function_index) {
    const turbowasm_module *module;
    turbowasm_function_signature signature;
    uint32_t i;
    if (adapter == NULL || !adapter->initialized || instance == NULL ||
        adapter->post_return_kind != TURBOWASM_COMPONENT_POST_RETURN_NONE)
        return TURBOWASM_INVALID_ARGUMENT;
    module = turbowasm_instance_module(instance);
    if (module == NULL || !turbowasm_module_function_signature_get(
            module, function_index, &signature) || signature.result_count != 0u ||
        signature.param_count != adapter->flat_signature.result_count)
        return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < signature.param_count; ++i) {
        if (!core_type_matches(turbowasm_module_function_param_type(
                module, function_index, i), adapter->flat_signature.results[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }
    adapter->post_return_instance = instance;
    adapter->post_return_function_index = function_index;
    adapter->post_return_kind = TURBOWASM_COMPONENT_POST_RETURN_CORE;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_core_call_set_canonical_post_return(
    turbowasm_component_core_call_adapter *adapter,
    const turbowasm_host_function_type *signature) {
    uint32_t i;
    if (adapter == NULL || !adapter->initialized || signature == NULL ||
        adapter->post_return_kind != TURBOWASM_COMPONENT_POST_RETURN_NONE)
        return TURBOWASM_INVALID_ARGUMENT;
    if (signature->result_count != 0u ||
        signature->param_count != adapter->flat_signature.result_count ||
        (signature->param_count != 0u && signature->params == NULL))
        return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < signature->param_count; ++i) {
        if (signature->params[i] != flat_value_kind(adapter->flat_signature.results[i]))
            return TURBOWASM_TYPE_MISMATCH;
    }
    adapter->post_return_kind = TURBOWASM_COMPONENT_POST_RETURN_CANONICAL_LEAVE;
    return TURBOWASM_OK;
}

static turbowasm_status component_lift_results(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_type *function,
    turbowasm_component_resource_codec_context *codec,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *results, size_t count,
    turbowasm_component_value *out) {
    if (count != adapter->flat_signature.result_count)
        return TURBOWASM_MALFORMED_MODULE;
    if (!function->as.function.has_result)
        return TURBOWASM_OK;
    if (type_ref_is_resource_handle(adapter->graph, function->as.function.result)) {
        if (count != 1u || results[0].kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_MALFORMED_MODULE;
        return canonical_resource_lift(codec, adapter->graph,
            function->as.function.result, (uint32_t)results[0].as.i32, out);
    }
    if (adapter->flat_signature.results_indirect) {
        uint64_t pointer;
        turbowasm_status status = pointer_from_core(
            memory->pointer_type, &results[0], &pointer);
        if (status != TURBOWASM_OK)
            return status;
        return turbowasm_component_canonical_lift_value(adapter->graph,
            function->as.function.result, memory, pointer, out);
    }
    return turbowasm_component_canonical_lift_flat_value(adapter->graph,
        function->as.function.result,
        (adapter->uses_memory || adapter->uses_resources) ? memory : NULL,
        results, (uint32_t)count, out);
}

static turbowasm_status component_post_return(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_value *results, size_t count,
    turbowasm_jit_execution_control *control, turbowasm_trap *trap) {
    turbowasm_status status;
    size_t returned = 0u;
    if (adapter->post_return_kind == TURBOWASM_COMPONENT_POST_RETURN_NONE)
        return TURBOWASM_OK;
    /* A direct canonical target has the same may_leave failure as a Core
     * cleanup that calls it. There is no provider or handle side effect. */
    if (adapter->post_return_kind == TURBOWASM_COMPONENT_POST_RETURN_CANONICAL_LEAVE) {
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }
    if (adapter->may_leave != NULL && !*adapter->may_leave) {
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }
    if (adapter->may_leave != NULL)
        *adapter->may_leave = false;
    if (adapter->host_call != NULL)
        status = turbowasm_instance_invoke_from_host(adapter->host_call,
            adapter->post_return_instance, adapter->post_return_function_index,
            results, count, NULL, 0u, &returned, trap);
    else if (control != NULL)
        status = turbowasm_instance_invoke_internal(
            adapter->post_return_instance->impl, adapter->post_return_function_index,
            results, count, NULL, 0u, &returned, trap, control);
    else
        status = turbowasm_instance_invoke(adapter->post_return_instance,
            adapter->post_return_function_index, results, count,
            NULL, 0u, &returned, trap);
    if (adapter->may_leave != NULL)
        *adapter->may_leave = true;
    if (status == TURBOWASM_EXCEPTION) {
        ((turbowasm_instance_impl *)adapter->post_return_instance->impl)->pending_exception = NULL;
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }
    if (status == TURBOWASM_OK && returned != 0u)
        return TURBOWASM_TYPE_MISMATCH;
    return status;
}

turbowasm_status turbowasm_component_core_call_invoke(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    const turbowasm_component_type *function;
    turbowasm_value core_args[
        TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
    turbowasm_value core_results[
        TURBOWASM_COMPONENT_MAX_FLAT_RESULTS] = {{0}};
    turbowasm_component_call_resource_scope resource_scope = {0};
    turbowasm_component_resource_codec_context codec = {0};
    turbowasm_component_canonical_memory call_memory;
    size_t core_arg_count = 0u;
    size_t core_result_count = 0u;
    turbowasm_status status;
    uint32_t i;

    if (adapter == NULL || !adapter->initialized ||
        trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    function = turbowasm_component_type_graph_get(
        adapter->graph, adapter->function_type);
    if (function == NULL ||
        function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_MALFORMED_MODULE;
    if (argument_count != function->as.function.param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (function->as.function.has_result && out_result == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (out_result != NULL)
        memset(out_result, 0, sizeof(*out_result));
    *trap = TURBOWASM_TRAP_NONE;

    resource_scope.adapter = adapter;
    codec.adapter = adapter;
    codec.scope = &resource_scope;
    call_memory = adapter->memory;

    if (adapter->uses_resources) {
        call_memory.resource_lower = canonical_resource_lower;
        call_memory.resource_lift = canonical_resource_lift;
        call_memory.resource_context = &codec;
    }

    if (adapter->flat_signature.params_indirect) {
        uint64_t pointer;

        status = lower_indirect_parameters(
            adapter, &call_memory, arguments, &pointer);
        if (status != TURBOWASM_OK)
            goto lowering_failed;

        status = pointer_to_core(
            call_memory.pointer_type,
            pointer, &core_args[0]);
        if (status != TURBOWASM_OK)
            goto lowering_failed;

        core_arg_count = 1u;
    } else {
        for (i = 0u; i < function->as.function.param_count; ++i) {
            turbowasm_component_type_ref ref =
                function->as.function.params[i];
            uint32_t j;

            if (type_ref_is_resource_handle(adapter->graph, ref)) {
                uint32_t handle;

                status = canonical_resource_lower(
                    &codec, adapter->graph, ref, &arguments[i], &handle);
                if (status != TURBOWASM_OK)
                    goto lowering_failed;

                if (core_arg_count >=
                    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto lowering_failed;
                }

                core_args[core_arg_count].kind =
                    TURBOWASM_VALUE_I32;
                core_args[core_arg_count].as.i32 =
                    (int32_t)handle;
                ++core_arg_count;
                continue;
            }

            {
                turbowasm_value
                    flat[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
                uint32_t flat_count = 0u;

                status =
                    turbowasm_component_canonical_lower_flat_value(
                        adapter->graph,
                        ref,
                        (adapter->uses_memory || adapter->uses_resources)
                            ? &call_memory
                            : NULL,
                        &arguments[i],
                        flat,
                        TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS,
                        &flat_count);
                if (status != TURBOWASM_OK)
                    goto lowering_failed;

                if (flat_count >
                    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS -
                        core_arg_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto lowering_failed;
                }

                for (j = 0u; j < flat_count; ++j)
                    core_args[core_arg_count++] = flat[j];
            }
        }
    }

    if (core_arg_count != adapter->flat_signature.param_count) {
        status = TURBOWASM_MALFORMED_MODULE;
        goto lowering_failed;
    }

    if (adapter->admission_commit != NULL)
        adapter->admission_commit(adapter->admission_context);
    if (adapter->host_call != NULL)
        status = turbowasm_instance_invoke_from_host(adapter->host_call,
            adapter->instance, adapter->function_index, core_args, core_arg_count,
            core_results, adapter->flat_signature.result_count, &core_result_count, trap);
    else
        status = turbowasm_instance_invoke(adapter->instance,
            adapter->function_index, core_args, core_arg_count, core_results,
            adapter->flat_signature.result_count, &core_result_count, trap);
    if (status == TURBOWASM_EXCEPTION) {
        ((turbowasm_instance_impl *)adapter->instance->impl)->pending_exception = NULL;
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        status = TURBOWASM_TRAPPED;
    }

    if (status != TURBOWASM_OK) {
        /*
         * A Core trap aborts the synchronous borrow scope. Borrowed handles
         * are cleaned locally; transferred own handles remain in the callee's
         * Component resource table exactly as in the C5b1 direct path.
         */
        resource_scope_abort_borrows(&resource_scope);
        resource_scope_destroy(&resource_scope);
        return status;
    }

    if (!resource_scope_finalize_borrows(&resource_scope)) {
        resource_scope_destroy(&resource_scope);
        return TURBOWASM_TRAPPED;
    }

    status = component_lift_results(adapter, function, &codec, &call_memory,
        core_results, core_result_count, out_result);
    if (status == TURBOWASM_OK)
        status = component_post_return(adapter, core_results, core_result_count, NULL, trap);
    if (status != TURBOWASM_OK) {
        /* Preserve the primary failure if a resource destructor also fails. */
        turbowasm_status cleanup = turbowasm_component_value_destroy(out_result);
        (void)cleanup;
    }
    resource_scope_destroy(&resource_scope);
    return status;

lowering_failed:
    resource_scope_rollback(&resource_scope);
    resource_scope_destroy(&resource_scope);
    return status;
}


typedef struct turbowasm_component_core_execution_impl {
    const turbowasm_component_core_call_adapter *adapter;
    const turbowasm_component_type *function;

    turbowasm_component_call_resource_scope resource_scope;
    turbowasm_component_resource_codec_context codec;
    turbowasm_component_canonical_memory call_memory;

    turbowasm_execution runtime_execution;
    turbowasm_component_value lifted_result;
    turbowasm_trap trap;
    turbowasm_status terminal_status;

    bool runtime_created;
    bool started;
    bool finalized;
    bool result_taken;
} turbowasm_component_core_execution_impl;

static turbowasm_component_core_execution_impl *
component_core_execution_impl_mut(
    turbowasm_component_core_execution *execution) {
    return execution != NULL
        ? (turbowasm_component_core_execution_impl *)execution->impl
        : NULL;
}

static const turbowasm_component_core_execution_impl *
component_core_execution_impl_get(
    const turbowasm_component_core_execution *execution) {
    return execution != NULL
        ? (const turbowasm_component_core_execution_impl *)execution->impl
        : NULL;
}

static turbowasm_status component_core_execution_lower(
    turbowasm_component_core_execution_impl *impl,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_value *core_args,
    size_t *out_core_arg_count) {
    const turbowasm_component_core_call_adapter *adapter;
    size_t core_arg_count = 0u;
    uint32_t i;
    turbowasm_status status;

    if (impl == NULL || impl->adapter == NULL ||
        core_args == NULL || out_core_arg_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    adapter = impl->adapter;
    if (argument_count != impl->function->as.function.param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    if (adapter->flat_signature.params_indirect) {
        uint64_t pointer;

        status = lower_indirect_parameters(
            adapter, &impl->call_memory, arguments, &pointer);
        if (status != TURBOWASM_OK)
            return status;

        status = pointer_to_core(
            impl->call_memory.pointer_type,
            pointer, &core_args[0]);
        if (status != TURBOWASM_OK)
            return status;
        core_arg_count = 1u;
    } else {
        for (i = 0u; i < impl->function->as.function.param_count; ++i) {
            turbowasm_component_type_ref ref =
                impl->function->as.function.params[i];
            uint32_t j;

            if (type_ref_is_resource_handle(adapter->graph, ref)) {
                uint32_t handle;

                status = canonical_resource_lower(
                    &impl->codec, adapter->graph, ref, &arguments[i], &handle);
                if (status != TURBOWASM_OK)
                    return status;

                if (core_arg_count >=
                    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS)
                    return TURBOWASM_MALFORMED_MODULE;

                core_args[core_arg_count].kind =
                    TURBOWASM_VALUE_I32;
                core_args[core_arg_count].as.i32 =
                    (int32_t)handle;
                ++core_arg_count;
                continue;
            }

            {
                turbowasm_value
                    flat[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
                uint32_t flat_count = 0u;

                status =
                    turbowasm_component_canonical_lower_flat_value(
                        adapter->graph,
                        ref,
                        (adapter->uses_memory || adapter->uses_resources)
                            ? &impl->call_memory
                            : NULL,
                        &arguments[i],
                        flat,
                        TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS,
                        &flat_count);
                if (status != TURBOWASM_OK)
                    return status;

                if (flat_count >
                    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS -
                        core_arg_count)
                    return TURBOWASM_MALFORMED_MODULE;

                for (j = 0u; j < flat_count; ++j)
                    core_args[core_arg_count++] = flat[j];
            }
        }
    }

    if (core_arg_count != adapter->flat_signature.param_count)
        return TURBOWASM_MALFORMED_MODULE;

    *out_core_arg_count = core_arg_count;
    return TURBOWASM_OK;
}

static turbowasm_status component_core_execution_lift(
    turbowasm_component_core_execution_impl *impl) {
    const turbowasm_component_core_call_adapter *adapter;
    turbowasm_value
        core_results[TURBOWASM_COMPONENT_MAX_FLAT_RESULTS] = {{0}};
    size_t core_result_count;
    size_t i;

    if (impl == NULL || impl->adapter == NULL ||
        impl->function == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    adapter = impl->adapter;

    if (!resource_scope_finalize_borrows(&impl->resource_scope))
        return TURBOWASM_TRAPPED;

    core_result_count =
        turbowasm_execution_result_count(&impl->runtime_execution);
    if (core_result_count != adapter->flat_signature.result_count ||
        core_result_count > TURBOWASM_COMPONENT_MAX_FLAT_RESULTS)
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < core_result_count; ++i) {
        const turbowasm_value *value =
            turbowasm_execution_result_at(
                &impl->runtime_execution, i);
        if (value == NULL)
            return TURBOWASM_MALFORMED_MODULE;
        core_results[i] = *value;
    }

    return component_lift_results(adapter, impl->function, &impl->codec,
        &impl->call_memory, core_results, core_result_count, &impl->lifted_result);
}

static turbowasm_status component_core_execution_complete(
    void *context, const turbowasm_value *results, size_t count,
    turbowasm_jit_execution_control *control, turbowasm_trap *trap) {
    turbowasm_component_core_execution_impl *impl = context;
    turbowasm_status status;
    if (!resource_scope_finalize_borrows(&impl->resource_scope))
        return TURBOWASM_TRAPPED;
    status = component_lift_results(impl->adapter, impl->function, &impl->codec,
        &impl->call_memory, results, count, &impl->lifted_result);
    if (status == TURBOWASM_OK)
        status = component_post_return(impl->adapter, results, count, control, trap);
    return status;
}

static turbowasm_status component_core_execution_finalize(
    turbowasm_component_core_execution_impl *impl,
    turbowasm_status runtime_status) {
    turbowasm_status status = runtime_status;

    if (impl == NULL || impl->finalized)
        return TURBOWASM_INVALID_ARGUMENT;

    impl->trap = turbowasm_execution_trap(
        &impl->runtime_execution);

    if (runtime_status != TURBOWASM_OK) {
        resource_scope_abort_borrows(&impl->resource_scope);
    } else if (impl->adapter->post_return_kind == TURBOWASM_COMPONENT_POST_RETURN_NONE) {
        status = component_core_execution_lift(impl);
    }
    if (status != TURBOWASM_OK) {
        turbowasm_status cleanup = turbowasm_component_value_destroy(&impl->lifted_result);
        (void)cleanup;
    }

    resource_scope_destroy(&impl->resource_scope);
    impl->terminal_status = status;
    impl->finalized = true;
    return status;
}

turbowasm_status turbowasm_component_core_execution_create(
    turbowasm_component_core_execution *execution,
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count) {
    const turbowasm_component_type *function;
    const turbowasm_runtime_config *config;
    turbowasm_runtime_scope alloc_scope;
    turbowasm_component_core_execution_impl *impl = NULL;
    turbowasm_value
        core_args[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
    size_t core_arg_count = 0u;
    turbowasm_status status;

    if (execution == NULL || execution->impl != NULL ||
        adapter == NULL || !adapter->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    function = turbowasm_component_type_graph_get(
        adapter->graph, adapter->function_type);
    if (function == NULL ||
        function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_MALFORMED_MODULE;
    if (argument_count != function->as.function.param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    config = adapter_runtime_config(adapter);
    if (config == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    alloc_scope = turbowasm_runtime_scope_enter(config);
    impl = (turbowasm_component_core_execution_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(alloc_scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->adapter = adapter;
    impl->function = function;
    impl->trap = TURBOWASM_TRAP_NONE;
    impl->terminal_status = TURBOWASM_OK;
    impl->resource_scope.adapter = adapter;
    impl->codec.adapter = adapter;
    impl->codec.scope = &impl->resource_scope;
    impl->call_memory = adapter->memory;

    if (adapter->uses_resources) {
        impl->call_memory.resource_lower =
            canonical_resource_lower;
        impl->call_memory.resource_lift =
            canonical_resource_lift;
        impl->call_memory.resource_context = &impl->codec;
    }

    status = component_core_execution_lower(
        impl, arguments, argument_count,
        core_args, &core_arg_count);
    if (status != TURBOWASM_OK)
        goto fail_before_start;

    status = turbowasm_execution_create(
        &impl->runtime_execution,
        adapter->instance,
        adapter->function_index,
        core_args,
        core_arg_count);
    if (status != TURBOWASM_OK)
        goto fail_before_start;

    impl->runtime_created = true;
    if (adapter->post_return_kind != TURBOWASM_COMPONENT_POST_RETURN_NONE) {
        status = turbowasm_execution_set_completion(&impl->runtime_execution,
            component_core_execution_complete, impl);
        if (status != TURBOWASM_OK) {
            turbowasm_execution_destroy(&impl->runtime_execution);
            goto fail_before_start;
        }
    }
    execution->impl = impl;
    return TURBOWASM_OK;

fail_before_start:
    resource_scope_rollback(&impl->resource_scope);
    resource_scope_destroy(&impl->resource_scope);
    alloc_scope = turbowasm_runtime_scope_enter(config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(alloc_scope);
    return status;
}

void turbowasm_component_core_execution_destroy(
    turbowasm_component_core_execution *execution) {
    turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_mut(execution);
    const turbowasm_runtime_config *config;
    turbowasm_runtime_scope alloc_scope;

    if (impl == NULL)
        return;

    config = adapter_runtime_config(impl->adapter);

    /* Unwind host-wait callbacks while their canonical borrows remain live. */
    if (impl->runtime_created)
        turbowasm_execution_destroy(
            &impl->runtime_execution);

    if (!impl->finalized) {
        if (impl->started)
            resource_scope_abort_borrows(&impl->resource_scope);
        else
            resource_scope_rollback(&impl->resource_scope);
        resource_scope_destroy(&impl->resource_scope);
    }

    if (!impl->result_taken &&
        impl->lifted_result.kind !=
            TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(
            &impl->lifted_result);

    if (config != NULL) {
        alloc_scope = turbowasm_runtime_scope_enter(config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(alloc_scope);
    }
    execution->impl = NULL;
}

turbowasm_status turbowasm_component_core_execution_resume(
    turbowasm_component_core_execution *execution,
    const turbowasm_execution_options *options) {
    turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_mut(execution);
    turbowasm_status status;

    if (impl == NULL || impl->finalized)
        return TURBOWASM_INVALID_ARGUMENT;

    impl->started = true;
    status = turbowasm_execution_resume(
        &impl->runtime_execution, options);
    if (status == TURBOWASM_YIELDED)
        return status;

    return component_core_execution_finalize(
        impl, status);
}

turbowasm_execution_state
turbowasm_component_core_execution_state_get(
    const turbowasm_component_core_execution *execution) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    if (impl == NULL)
        return TURBOWASM_EXECUTION_FAILED;
    if (!impl->finalized)
        return turbowasm_execution_state_get(
            &impl->runtime_execution);

    if (impl->terminal_status == TURBOWASM_OK)
        return TURBOWASM_EXECUTION_COMPLETED;
    if (impl->terminal_status == TURBOWASM_TRAPPED)
        return TURBOWASM_EXECUTION_TRAPPED;
    if (impl->terminal_status == TURBOWASM_EXCEPTION)
        return TURBOWASM_EXECUTION_EXCEPTION;
    return TURBOWASM_EXECUTION_FAILED;
}

turbowasm_yield_reason
turbowasm_component_core_execution_yield_reason_get(
    const turbowasm_component_core_execution *execution) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    if (impl == NULL || impl->finalized)
        return TURBOWASM_YIELD_NONE;
    return turbowasm_execution_yield_reason_get(
        &impl->runtime_execution);
}

bool turbowasm_component_core_execution_pending_host_wait(
    const turbowasm_component_core_execution *execution,
    turbowasm_host_wait *out_wait) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    return impl != NULL && !impl->finalized &&
        turbowasm_execution_pending_host_wait(
            &impl->runtime_execution, out_wait);
}

turbowasm_status
turbowasm_component_core_execution_complete_host_wait(
    turbowasm_component_core_execution *execution,
    turbowasm_host_wait wait,
    int status) {
    turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_mut(execution);

    if (impl == NULL || impl->finalized)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_execution_complete_host_wait(
        &impl->runtime_execution, wait, status);
}

turbowasm_status turbowasm_component_core_execution_terminal_status(
    const turbowasm_component_core_execution *execution) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    return impl != NULL && impl->finalized
        ? impl->terminal_status
        : TURBOWASM_INVALID_ARGUMENT;
}

turbowasm_trap turbowasm_component_core_execution_trap(
    const turbowasm_component_core_execution *execution) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    return impl != NULL && impl->finalized
        ? impl->trap
        : TURBOWASM_TRAP_NONE;
}

size_t turbowasm_component_core_execution_result_count(
    const turbowasm_component_core_execution *execution) {
    const turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_get(execution);

    return impl != NULL && impl->finalized &&
        impl->terminal_status == TURBOWASM_OK &&
        impl->function != NULL &&
        impl->function->as.function.has_result
        ? 1u
        : 0u;
}

turbowasm_status turbowasm_component_core_execution_take_result(
    turbowasm_component_core_execution *execution,
    turbowasm_component_value *out_result) {
    turbowasm_component_core_execution_impl *impl =
        component_core_execution_impl_mut(execution);

    if (impl == NULL || out_result == NULL ||
        !impl->finalized ||
        impl->terminal_status != TURBOWASM_OK ||
        impl->function == NULL ||
        !impl->function->as.function.has_result ||
        impl->result_taken)
        return TURBOWASM_INVALID_ARGUMENT;

    *out_result = impl->lifted_result;
    memset(&impl->lifted_result, 0, sizeof(impl->lifted_result));
    impl->result_taken = true;
    return TURBOWASM_OK;
}
