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

    /* Resource/own/borrow are C5b. */
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
    const turbowasm_component_type *function;
    bool uses_memory = false;
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
    }
    if (function->as.function.has_result) {
        if (!type_ref_supported(
                graph, function->as.function.result, 0u))
            return TURBOWASM_UNSUPPORTED;
        if (type_ref_uses_memory(
                graph, function->as.function.result, 0u))
            uses_memory = true;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->graph = graph;
    adapter->function_type = function_type;
    adapter->instance = instance;
    adapter->function_index = function_index;
    adapter->uses_memory = uses_memory;

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
            turbowasm_value flat[2] = {{0}};
            uint32_t flat_count = 0u;
            uint32_t j;

            status = turbowasm_component_canonical_lower_flat_value(
                adapter->graph,
                function->as.function.params[i],
                adapter->uses_memory ? &adapter->memory : NULL,
                &arguments[i],
                flat, &flat_count);
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

    if (core_arg_count != adapter->flat_signature.param_count)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_instance_invoke(
        adapter->instance,
        adapter->function_index,
        core_args,
        core_arg_count,
        core_results,
        adapter->flat_signature.result_count,
        &core_result_count,
        trap);
    if (status != TURBOWASM_OK)
        return status;
    if (core_result_count != adapter->flat_signature.result_count)
        return TURBOWASM_MALFORMED_MODULE;

    if (!function->as.function.has_result)
        return TURBOWASM_OK;

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
