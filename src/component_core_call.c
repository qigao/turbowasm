#include "component_core_call.h"

#include "instance_internal.h"

#include <cmeta/cmeta.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool align_up_u64(
    uint64_t value,
    uint64_t alignment,
    uint64_t *out) {
    uint64_t mask;

    if (out == NULL || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u)
        return false;
    mask = alignment - 1u;
    if (value > UINT64_MAX - mask)
        return false;
    *out = (value + mask) & ~mask;
    return true;
}

static bool type_ref_supported(
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

    kind = type->kind;
    if (kind >= TURBOWASM_COMPONENT_TYPE_BOOL &&
        kind <= TURBOWASM_COMPONENT_TYPE_STRING)
        return true;
    if (kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return type_ref_supported(
            graph, type->as.list.element_type, depth + 1u);
    if (kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        kind == TURBOWASM_COMPONENT_TYPE_BORROW)
        return depth == 0u;

    return false;
}

static bool type_ref_uses_memory(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t depth) {
    const turbowasm_component_type *type;

    if (graph == NULL || depth >= 64u)
        return true;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        return ref.as.inline_type ==
            TURBOWASM_COMPONENT_TYPE_STRING;
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return true;

    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (type == NULL)
        return true;
    if (type->kind == TURBOWASM_COMPONENT_TYPE_STRING ||
        type->kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return true;
    return false;
}

static bool type_ref_uses_resources(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t depth) {
    const turbowasm_component_type *type;

    if (graph == NULL || depth >= 64u ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;
    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (type == NULL)
        return false;
    if (type->kind == TURBOWASM_COMPONENT_TYPE_OWN ||
        type->kind == TURBOWASM_COMPONENT_TYPE_BORROW)
        return true;
    if (type->kind == TURBOWASM_COMPONENT_TYPE_LIST)
        return type_ref_uses_resources(
            graph, type->as.list.element_type, depth + 1u);
    return false;
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

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity == 0u)
        return TURBOWASM_MALFORMED_MODULE;

    if (resource_type->as.resource.rep_type == 0x7fu) {
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
    return TURBOWASM_OK;
}

static void rollback_created_handles(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_resource_handle *handles,
    const uint64_t *identities,
    const bool *borrowed,
    uint32_t count) {
    uint32_t i;

    if (adapter == NULL || adapter->resources == NULL)
        return;

    for (i = 0u; i < count; ++i) {
        if (borrowed[i]) {
            (void)turbowasm_component_resource_drop(
                adapter->resources, handles[i], identities[i],
                NULL, NULL);
        } else {
            turbowasm_value rep = {0};
            (void)turbowasm_component_resource_take_owned(
                adapter->resources, handles[i], identities[i], &rep);
        }
    }
}

static bool finalize_borrow_scope(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_resource_handle *handles,
    const uint64_t *identities,
    const bool *borrowed,
    uint32_t count) {
    uint32_t i;
    bool clean = true;

    if (adapter == NULL || adapter->resources == NULL)
        return count == 0u;

    for (i = 0u; i < count; ++i) {
        turbowasm_value rep = {0};
        turbowasm_status status;

        if (!borrowed[i])
            continue;

        status = turbowasm_component_resource_rep(
            adapter->resources, handles[i], identities[i], &rep);
        if (status == TURBOWASM_OK) {
            clean = false;
            (void)turbowasm_component_resource_drop(
                adapter->resources, handles[i], identities[i],
                NULL, NULL);
        } else if (status != TURBOWASM_TRAPPED) {
            clean = false;
        }
    }
    return clean;
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
        memory->instance != adapter->instance ||
        memory->string_encoding != TURBOWASM_COMPONENT_STRING_UTF8)
        return TURBOWASM_INVALID_ARGUMENT;

    module = turbowasm_instance_module(adapter->instance);
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

static turbowasm_status parameter_tuple_layout(
    const turbowasm_component_core_call_adapter *adapter,
    uint64_t *out_alignment,
    uint64_t *out_size) {
    const turbowasm_component_type *function;
    uint64_t offset = 0u;
    uint64_t maximum_alignment = 1u;
    uint32_t i;

    if (adapter == NULL || out_alignment == NULL || out_size == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    function = turbowasm_component_type_graph_get(
        adapter->graph, adapter->function_type);
    if (function == NULL ||
        function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return TURBOWASM_MALFORMED_MODULE;

    for (i = 0u; i < function->as.function.param_count; ++i) {
        turbowasm_component_layout layout;
        turbowasm_status status =
            turbowasm_component_canonical_layout(
                adapter->graph,
                function->as.function.params[i],
                adapter->memory.pointer_type,
                &layout);
        if (status != TURBOWASM_OK)
            return status;
        if (!align_up_u64(offset, layout.alignment, &offset))
            return TURBOWASM_OUT_OF_MEMORY;
        if (layout.size > UINT64_MAX - offset)
            return TURBOWASM_OUT_OF_MEMORY;
        offset += layout.size;
        if (layout.alignment > maximum_alignment)
            maximum_alignment = layout.alignment;
    }

    if (!align_up_u64(offset, maximum_alignment, &offset))
        return TURBOWASM_OUT_OF_MEMORY;
    *out_alignment = maximum_alignment;
    *out_size = offset;
    return TURBOWASM_OK;
}

static turbowasm_status lower_indirect_parameters(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    uint64_t *out_pointer) {
    const turbowasm_component_type *function;
    turbowasm_instance_impl *instance;
    uint64_t alignment;
    uint64_t size;
    uint64_t pointer;
    uint64_t offset = 0u;
    uint32_t i;
    turbowasm_status status;

    if (adapter == NULL || arguments == NULL || out_pointer == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (adapter->memory.guest_realloc == NULL)
        return TURBOWASM_UNSUPPORTED;

    function = turbowasm_component_type_graph_get(
        adapter->graph, adapter->function_type);
    if (function == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    status = parameter_tuple_layout(adapter, &alignment, &size);
    if (status != TURBOWASM_OK)
        return status;

    status = adapter->memory.guest_realloc(
        adapter->memory.realloc_context,
        0u, 0u, alignment, size, &pointer);
    if (status != TURBOWASM_OK)
        return status;
    if ((alignment != 0u && pointer % alignment != 0u) ||
        size > (uint64_t)SIZE_MAX)
        return TURBOWASM_TRAPPED;

    instance = (turbowasm_instance_impl *)adapter->instance->impl;
    if (instance == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    {
        uint8_t *range = NULL;
        status = turbowasm_instance_memory_bounds(
            instance, adapter->memory.memory_index,
            pointer, 0u, (size_t)size, &range);
        if (status != TURBOWASM_OK)
            return status;
    }

    for (i = 0u; i < function->as.function.param_count; ++i) {
        turbowasm_component_layout layout;
        status = turbowasm_component_canonical_layout(
            adapter->graph,
            function->as.function.params[i],
            adapter->memory.pointer_type,
            &layout);
        if (status != TURBOWASM_OK)
            return status;
        if (!align_up_u64(offset, layout.alignment, &offset))
            return TURBOWASM_OUT_OF_MEMORY;

        status = turbowasm_component_canonical_lower_value(
            adapter->graph,
            function->as.function.params[i],
            &adapter->memory,
            pointer + offset,
            &arguments[i]);
        if (status != TURBOWASM_OK)
            return status;
        offset += layout.size;
    }

    *out_pointer = pointer;
    return TURBOWASM_OK;
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
        if (!type_ref_supported(
                graph, function->as.function.params[i], 0u))
            return TURBOWASM_UNSUPPORTED;
        if (type_ref_uses_memory(
                graph, function->as.function.params[i], 0u)) {
            uses_memory = true;
            needs_realloc = true;
        }
        if (type_ref_uses_resources(
                graph, function->as.function.params[i], 0u))
            uses_resources = true;
    }

    if (function->as.function.has_result) {
        turbowasm_component_type_kind result_kind = TURBOWASM_COMPONENT_TYPE_UNDEFINED;
        uint64_t result_identity = 0u;
        turbowasm_value_kind result_rep_kind = (turbowasm_value_kind)0;

        if (!type_ref_supported(
                graph, function->as.function.result, 0u))
            return TURBOWASM_UNSUPPORTED;
        if (type_ref_uses_memory(
                graph, function->as.function.result, 0u))
            uses_memory = true;
        if (type_ref_uses_resources(
                graph, function->as.function.result, 0u)) {
            uses_resources = true;
            status = resource_handle_info(
                graph, function->as.function.result,
                &result_kind, &result_identity, &result_rep_kind);
            if (status != TURBOWASM_OK)
                return status;
            if (result_kind == TURBOWASM_COMPONENT_TYPE_BORROW)
                return TURBOWASM_MALFORMED_MODULE;
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
        if (uses_resources) {
            status = TURBOWASM_UNSUPPORTED;
            goto fail;
        }
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

turbowasm_status turbowasm_component_core_call_invoke(
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    const turbowasm_component_type *function;
    turbowasm_value core_args[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {{0}};
    turbowasm_value core_results[TURBOWASM_COMPONENT_MAX_FLAT_RESULTS] = {{0}};
    turbowasm_component_resource_handle
        created_handles[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {0};
    uint64_t created_identities[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {0};
    bool created_borrowed[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {0};
    uint32_t created_count = 0u;
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

    if (adapter->flat_signature.params_indirect) {
        uint64_t pointer;
        status = lower_indirect_parameters(
            adapter, arguments, &pointer);
        if (status != TURBOWASM_OK)
            return status;
        status = pointer_to_core(
            adapter->memory.pointer_type,
            pointer, &core_args[0]);
        if (status != TURBOWASM_OK)
            return status;
        core_arg_count = 1u;
    } else {
        for (i = 0u; i < function->as.function.param_count; ++i) {
            turbowasm_component_type_ref ref =
                function->as.function.params[i];
            uint32_t j;

            if (type_ref_uses_resources(adapter->graph, ref, 0u)) {
                bool borrowed = false;
                if (created_count >=
                    TURBOWASM_COMPONENT_MAX_FLAT_PARAMS) {
                    rollback_created_handles(
                        adapter, created_handles,
                        created_identities, created_borrowed,
                        created_count);
                    return TURBOWASM_UNSUPPORTED;
                }

                status = lower_resource_argument(
                    adapter, ref, &arguments[i],
                    &created_handles[created_count],
                    &created_identities[created_count],
                    &borrowed,
                    &core_args[core_arg_count]);
                if (status != TURBOWASM_OK) {
                    rollback_created_handles(
                        adapter, created_handles,
                        created_identities, created_borrowed,
                        created_count);
                    return status;
                }
                created_borrowed[created_count] = borrowed;
                ++created_count;
                ++core_arg_count;
                continue;
            }

            {
                turbowasm_value flat[2] = {{0}};
                uint32_t flat_count = 0u;

                status = turbowasm_component_canonical_lower_flat_value(
                    adapter->graph,
                    ref,
                    adapter->uses_memory ? &adapter->memory : NULL,
                    &arguments[i],
                    flat, &flat_count);
                if (status != TURBOWASM_OK) {
                    rollback_created_handles(
                        adapter, created_handles,
                        created_identities, created_borrowed,
                        created_count);
                    return status;
                }
                if (flat_count >
                    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS -
                    core_arg_count) {
                    rollback_created_handles(
                        adapter, created_handles,
                        created_identities, created_borrowed,
                        created_count);
                    return TURBOWASM_MALFORMED_MODULE;
                }

                for (j = 0u; j < flat_count; ++j)
                    core_args[core_arg_count++] = flat[j];
            }
        }
    }

    if (core_arg_count != adapter->flat_signature.param_count) {
        rollback_created_handles(
            adapter, created_handles,
            created_identities, created_borrowed,
            created_count);
        return TURBOWASM_MALFORMED_MODULE;
    }

    status = turbowasm_instance_invoke(
        adapter->instance,
        adapter->function_index,
        core_args,
        core_arg_count,
        core_results,
        adapter->flat_signature.result_count,
        &core_result_count,
        trap);

    if (status != TURBOWASM_OK) {
        /*
         * A Core trap aborts the synchronous borrow scope. Transient borrowed
         * handles are cleaned locally; transferred own handles remain owned by
         * the callee resource table.
         */
        for (i = 0u; i < created_count; ++i) {
            if (created_borrowed[i]) {
                (void)turbowasm_component_resource_drop(
                    adapter->resources,
                    created_handles[i],
                    created_identities[i],
                    NULL, NULL);
            }
        }
        return status;
    }

    if (!finalize_borrow_scope(
            adapter, created_handles,
            created_identities, created_borrowed,
            created_count))
        return TURBOWASM_TRAPPED;

    if (core_result_count != adapter->flat_signature.result_count)
        return TURBOWASM_MALFORMED_MODULE;

    if (!function->as.function.has_result)
        return TURBOWASM_OK;

    if (type_ref_uses_resources(
            adapter->graph, function->as.function.result, 0u)) {
        if (core_result_count != 1u)
            return TURBOWASM_MALFORMED_MODULE;
        return lift_owned_result(
            adapter, function->as.function.result,
            &core_results[0], out_result);
    }

    if (adapter->flat_signature.results_indirect) {
        uint64_t pointer;
        status = pointer_from_core(
            adapter->memory.pointer_type,
            &core_results[0], &pointer);
        if (status != TURBOWASM_OK)
            return status;
        return turbowasm_component_canonical_lift_value(
            adapter->graph,
            function->as.function.result,
            &adapter->memory,
            pointer,
            out_result);
    }

    return turbowasm_component_canonical_lift_flat_value(
        adapter->graph,
        function->as.function.result,
        adapter->uses_memory ? &adapter->memory : NULL,
        core_results,
        (uint32_t)core_result_count,
        out_result);
}
