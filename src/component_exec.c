#include "component_exec.h"

#include "instance_internal.h"
#include "runtime_alloc.h"

#include <turbowasm/link.h>
#include <turbowasm/value.h>

#include <cmeta/cmeta.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static turbowasm_status initialize_lift_adapter(
    turbowasm_component_exec *exec, const turbowasm_component_binary *binary, uint32_t index);

static bool component_name_equal(
    turbowasm_component_name left,
    const uint8_t *right,
    uint32_t right_size) {
    return left.size == right_size &&
           (right_size == 0u ||
            (left.bytes != NULL && right != NULL &&
             memcmp(left.bytes, right, right_size) == 0));
}

static bool component_name_equal_core(
    turbowasm_component_name left,
    turbowasm_name right) {
    return left.size == right.size &&
           (left.size == 0u ||
            (left.bytes != NULL && right.bytes != NULL &&
             memcmp(left.bytes, right.bytes, left.size) == 0));
}

static const turbowasm_export_desc *find_core_function_export(
    const turbowasm_module *module,
    turbowasm_component_name name) {
    size_t i;
    size_t count;

    if (module == NULL)
        return NULL;

    count = turbowasm_module_export_count(module);
    for (i = 0u; i < count; ++i) {
        const turbowasm_export_desc *export_desc =
            turbowasm_module_export_at(module, i);
        if (export_desc != NULL &&
            export_desc->kind == TURBOWASM_EXTERN_FUNCTION &&
            component_name_equal_core(name, export_desc->name))
            return export_desc;
    }
    return NULL;
}

static const turbowasm_export_desc *find_core_memory_export(
    const turbowasm_module *module,
    turbowasm_component_name name) {
    size_t i;
    size_t count;

    if (module == NULL)
        return NULL;

    count = turbowasm_module_export_count(module);
    for (i = 0u; i < count; ++i) {
        const turbowasm_export_desc *export_desc =
            turbowasm_module_export_at(module, i);
        if (export_desc != NULL &&
            export_desc->kind == TURBOWASM_EXTERN_MEMORY &&
            component_name_equal_core(name, export_desc->name))
            return export_desc;
    }
    return NULL;
}

static turbowasm_value_kind pointer_value_kind(
    turbowasm_component_pointer_type pointer_type) {
    return pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? TURBOWASM_VALUE_I64
        : TURBOWASM_VALUE_I32;
}

static bool core_module_function_has_pointer_signature(
    const turbowasm_module *module,
    uint32_t function_index,
    turbowasm_component_pointer_type pointer_type) {
    turbowasm_function_signature signature;
    const cmeta_type_desc *expected;
    uint32_t i;

    if (module == NULL ||
        !turbowasm_module_function_signature_get(
            module, function_index, &signature) ||
        signature.param_count != 4u ||
        signature.result_count != 1u)
        return false;

    expected = turbowasm_value_type_descriptor(
        pointer_value_kind(pointer_type));
    if (expected == NULL)
        return false;

    for (i = 0u; i < 4u; ++i) {
        const cmeta_type_desc *actual =
            turbowasm_module_function_param_type(
                module, function_index, i);
        if (actual == NULL || !cmeta_type_equal(actual, expected))
            return false;
    }

    {
        const cmeta_type_desc *actual =
            turbowasm_module_function_result_type(
                module, function_index, 0u);
        return actual != NULL &&
               cmeta_type_equal(actual, expected);
    }
}

static bool core_function_has_pointer_signature(
    const turbowasm_instance *instance,
    uint32_t function_index,
    turbowasm_component_pointer_type pointer_type) {
    if (instance == NULL)
        return false;
    return core_module_function_has_pointer_signature(
        turbowasm_instance_module(instance),
        function_index,
        pointer_type);
}

static turbowasm_status pointer_argument(
    turbowasm_component_pointer_type pointer_type,
    uint64_t value,
    turbowasm_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value > UINT32_MAX)
            return TURBOWASM_TRAPPED;
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)value;
        return TURBOWASM_OK;
    }
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I64) {
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)value;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status component_guest_realloc(
    void *context,
    uint64_t old_pointer,
    uint64_t old_size,
    uint64_t alignment,
    uint64_t new_size,
    uint64_t *out_pointer) {
    turbowasm_component_exec_realloc_context *realloc_context =
        (turbowasm_component_exec_realloc_context *)context;
    turbowasm_value arguments[4] = {{0}};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    if (realloc_context == NULL ||
        realloc_context->instance == NULL ||
        out_pointer == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pointer_argument(
        realloc_context->pointer_type,
        old_pointer, &arguments[0]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        old_size, &arguments[1]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        alignment, &arguments[2]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        new_size, &arguments[3]);
    if (status != TURBOWASM_OK)
        return status;

    if (realloc_context->may_leave != NULL) {
        if (!*realloc_context->may_leave) {
            if (realloc_context->trap != NULL)
                *realloc_context->trap = TURBOWASM_TRAP_UNREACHABLE;
            return TURBOWASM_TRAPPED;
        }
        *realloc_context->may_leave = false;
    }
    if (realloc_context->call != NULL)
        status = turbowasm_instance_invoke_from_host(
            realloc_context->call, realloc_context->instance,
            realloc_context->function_index, arguments, 4u,
            &result, 1u, &result_count, &trap);
    else
        status = turbowasm_instance_invoke(
            realloc_context->instance, realloc_context->function_index,
            arguments, 4u, &result, 1u, &result_count, &trap);
    if (realloc_context->may_leave != NULL)
        *realloc_context->may_leave = true;
    if (status == TURBOWASM_EXCEPTION) {
        ((turbowasm_instance_impl *)realloc_context->instance->impl)->pending_exception = NULL;
        trap = TURBOWASM_TRAP_UNREACHABLE;
        status = TURBOWASM_TRAPPED;
    }
    if (realloc_context->trap != NULL)
        *realloc_context->trap = trap;
    if (status != TURBOWASM_OK)
        return status;
    if (trap != TURBOWASM_TRAP_NONE)
        return TURBOWASM_TRAPPED;
    if (result_count != 1u ||
        result.kind !=
            pointer_value_kind(realloc_context->pointer_type))
        return TURBOWASM_TYPE_MISMATCH;

    *out_pointer =
        result.kind == TURBOWASM_VALUE_I64
            ? (uint64_t)result.as.i64
            : (uint64_t)(uint32_t)result.as.i32;
    return TURBOWASM_OK;
}

static turbowasm_status invoke_mapped_core_function(
    turbowasm_component_exec *exec,
    turbowasm_host_call *call,
    uint32_t core_function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    const turbowasm_component_exec_core_function *function;
    turbowasm_instance *instance;
    turbowasm_status status;

    if (exec == NULL || result_count == NULL || trap == NULL ||
        core_function_index >= exec->core_function_count)
        return TURBOWASM_INVALID_ARGUMENT;

    function = &exec->core_functions[core_function_index];
    if (function->kind !=
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE ||
        function->instance_index >= exec->core_instance_count)
        return TURBOWASM_UNSUPPORTED;

    instance = &exec->core_instances[function->instance_index];
    if (call != NULL)
        status = turbowasm_instance_invoke_from_host(call, instance,
            function->function_index, arguments, argument_count,
            results, result_capacity, result_count, trap);
    else
        status = turbowasm_instance_invoke(instance,
            function->function_index, arguments, argument_count,
            results, result_capacity, result_count, trap);
    if (status == TURBOWASM_EXCEPTION) {
        ((turbowasm_instance_impl *)instance->impl)->pending_exception = NULL;
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }
    return status;
}

static turbowasm_status component_resource_destructor_bridge(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    turbowasm_component_exec_resource_context *resource_context =
        (turbowasm_component_exec_resource_context *)context;
    const turbowasm_component_type *resource_type;
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    if (resource_context == NULL ||
        resource_context->exec == NULL ||
        resource_context->exec->binary == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    resource_type = turbowasm_component_type_graph_get(
        &resource_context->exec->binary->type_graph,
        resource_context->resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity != resource_identity ||
        !resource_type->as.resource.has_destructor)
        return TURBOWASM_INVALID_ARGUMENT;

    status = invoke_mapped_core_function(
        resource_context->exec,
        resource_context->call,
        resource_type->as.resource.destructor_index,
        &rep,
        1u,
        NULL,
        0u,
        &result_count,
        &trap);
    if (resource_context->trap != NULL)
        *resource_context->trap = trap;
    if (status != TURBOWASM_OK)
        return status;
    if (trap != TURBOWASM_TRAP_NONE || result_count != 0u)
        return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

static turbowasm_status component_resource_release_from_host(
    turbowasm_component_exec *exec, uint64_t identity, turbowasm_value rep,
    turbowasm_host_call *call) {
    uint32_t i;
    if (exec == NULL || exec->binary == NULL || identity == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < exec->binary->type_graph.count; ++i) {
        const turbowasm_component_type *type = &exec->binary->type_graph.types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE &&
            type->as.resource.identity == identity && !type->as.resource.identity_alias) {
            turbowasm_component_exec_resource_context context = {exec, i};
            context.call = call;
            if (!type->as.resource.has_destructor)
                return TURBOWASM_OK;
            return component_resource_destructor_bridge(&context, identity, rep);
        }
    }
    if (exec->imports.resource_drop == NULL || rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;
    return exec->imports.resource_drop(exec->imports.context, identity, (uint32_t)rep.as.i32);
}

turbowasm_status turbowasm_component_exec_resource_release(
    turbowasm_component_exec *exec, uint64_t identity, turbowasm_value rep) {
    return component_resource_release_from_host(exec, identity, rep, NULL);
}

static turbowasm_status component_external_destructor(
    void *context, uint64_t identity, turbowasm_value rep) {
    return turbowasm_component_exec_resource_release(context, identity, rep);
}

typedef struct component_import_resource_loan {
    turbowasm_component_exec *exec;
    uint64_t identity;
    uint32_t handle;
    uint32_t staged_handle;
    turbowasm_host_call *call;
    turbowasm_value rep;
    bool borrowed;
    bool committed;
} component_import_resource_loan;

static turbowasm_status component_import_resource_release(void *context) {
    component_import_resource_loan *loan = context;
    turbowasm_status status = TURBOWASM_OK;
    if (loan->borrowed)
        status = turbowasm_component_resource_lend_release(
            &loan->exec->resource_table, loan->handle, loan->identity);
    else if (!loan->committed) {
        if (loan->staged_handle != 0u)
            status = turbowasm_component_resource_take_owned(&loan->exec->resource_table,
                loan->staged_handle, loan->identity, &loan->rep);
        if (status == TURBOWASM_OK)
            status = component_resource_release_from_host(loan->exec, loan->identity, loan->rep, loan->call);
    }
    turbowasm_rt_free(loan);
    return status;
}

/* Guest handles belong to this instance. Provider handles remain opaque reps;
 * the canonical table owns movement, generation checks and active borrow loans. */
static turbowasm_status component_import_resource_lift(
    void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, uint32_t handle, turbowasm_component_value *out) {
    turbowasm_component_exec *exec = context;
    const turbowasm_component_type *type = ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED
        ? turbowasm_component_type_graph_get(graph, ref.as.indexed) : NULL;
    const turbowasm_component_type *resource;
    component_import_resource_loan *loan;
    turbowasm_value rep = {0};
    turbowasm_status status;
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
            type->kind != TURBOWASM_COMPONENT_TYPE_BORROW) || exec->imports.resource_lift == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    if (resource == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_rep(&exec->resource_table, handle,
        resource->as.resource.identity, &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;
    status = exec->imports.resource_lift(exec->imports.context, graph, ref, (uint32_t)rep.as.i32, out);
    if (status != TURBOWASM_OK)
        return status;
    loan = turbowasm_rt_calloc(1u, sizeof(*loan));
    if (loan == NULL) {
        memset(out, 0, sizeof(*out));
        return TURBOWASM_OUT_OF_MEMORY;
    }
    loan->exec = exec;
    loan->identity = resource->as.resource.identity;
    loan->handle = handle;
    loan->rep = rep;
    loan->borrowed = type->kind == TURBOWASM_COMPONENT_TYPE_BORROW;
    status = loan->borrowed
        ? turbowasm_component_resource_lend_acquire(&exec->resource_table, handle, loan->identity)
        : turbowasm_component_resource_take_owned(&exec->resource_table, handle, loan->identity, &rep);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(loan);
        memset(out, 0, sizeof(*out));
        return status;
    }
    out->release = component_import_resource_release;
    out->release_context = loan;
    out->resource_identity = loan->identity;
    return TURBOWASM_OK;
}

static turbowasm_status component_import_resource_lower(
    void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, const turbowasm_component_value *value, uint32_t *out) {
    turbowasm_component_exec *exec = context;
    const turbowasm_component_type *type = ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED
        ? turbowasm_component_type_graph_get(graph, ref.as.indexed) : NULL;
    const turbowasm_component_type *resource;
    turbowasm_value rep = {.kind = TURBOWASM_VALUE_I32};
    uint32_t provider_handle;
    turbowasm_status status;
    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_OWN ||
            exec->imports.resource_lower == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    if (resource == NULL)
        return TURBOWASM_TYPE_MISMATCH;
    status = exec->imports.resource_lower(exec->imports.context, graph, ref, value, &provider_handle);
    if (status != TURBOWASM_OK)
        return status;
    rep.as.i32 = (int32_t)provider_handle;
    status = turbowasm_component_resource_new_owned(&exec->resource_table,
        resource->as.resource.identity, rep, out);
    if (status != TURBOWASM_OK)
        (void)turbowasm_component_exec_resource_release(exec, resource->as.resource.identity, rep);
    return status;
}

/* Canonical lifting has already checked shape and bounded nesting. */
static void component_import_commit_resources(turbowasm_component_value *value) {
    turbowasm_component_value_list *sequence = NULL;
    turbowasm_component_value *payload = NULL;
    size_t i;
    switch (value->kind) {
        case TURBOWASM_COMPONENT_TYPE_OWN:
            if (value->release == component_import_resource_release)
                ((component_import_resource_loan *)value->release_context)->committed = true;
            break;
        case TURBOWASM_COMPONENT_TYPE_LIST: sequence = &value->as.list; break;
        case TURBOWASM_COMPONENT_TYPE_RECORD: sequence = &value->as.record; break;
        case TURBOWASM_COMPONENT_TYPE_TUPLE: sequence = &value->as.tuple; break;
        case TURBOWASM_COMPONENT_TYPE_VARIANT: payload = value->as.variant.payload; break;
        case TURBOWASM_COMPONENT_TYPE_OPTION: payload = value->as.option.payload; break;
        case TURBOWASM_COMPONENT_TYPE_RESULT: payload = value->as.result.payload; break;
        default: break;
    }
    if (sequence != NULL)
        for (i = 0u; i < sequence->count; ++i)
            component_import_commit_resources(&sequence->items[i]);
    if (payload != NULL)
        component_import_commit_resources(payload);
}

typedef struct component_local_call {
    turbowasm_component_exec *exec;
    turbowasm_host_call *call;
    turbowasm_component_value *arguments;
    size_t argument_count;
} component_local_call;

static void component_local_commit_arguments(void *context) {
    component_local_call *local = context;
    size_t i;
    for (i = 0u; i < local->argument_count; ++i)
        component_import_commit_resources(&local->arguments[i]);
}

static turbowasm_status component_local_resource_lift(
    void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, uint32_t handle, turbowasm_component_value *out) {
    component_local_call *local = context;
    const turbowasm_component_type *type = ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED
        ? turbowasm_component_type_graph_get(graph, ref.as.indexed) : NULL;
    const turbowasm_component_type *resource;
    component_import_resource_loan *loan;
    turbowasm_status status;
    turbowasm_value rep = {0};
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
                        type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return TURBOWASM_TYPE_MISMATCH;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    if (resource == NULL) return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_rep(&local->exec->resource_table, handle,
        resource->as.resource.identity, &rep);
    if (status != TURBOWASM_OK) return status;
    loan = turbowasm_rt_calloc(1u, sizeof(*loan));
    if (loan == NULL) return TURBOWASM_OUT_OF_MEMORY;
    loan->exec = local->exec; loan->call = local->call;
    loan->identity = resource->as.resource.identity;
    loan->handle = handle; loan->rep = rep;
    loan->borrowed = type->kind == TURBOWASM_COMPONENT_TYPE_BORROW;
    status = loan->borrowed
        ? turbowasm_component_resource_lend_acquire(&local->exec->resource_table, handle, loan->identity)
        : turbowasm_component_resource_take_owned(&local->exec->resource_table, handle, loan->identity, &rep);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(loan); return status; }
    out->kind = type->kind; out->as.resource_rep = rep;
    out->resource_identity = loan->identity;
    out->release = component_import_resource_release; out->release_context = loan;
    return TURBOWASM_OK;
}

static turbowasm_status component_local_result_owner(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    turbowasm_component_value *value) {
    component_local_call *local = context;
    component_import_resource_loan *loan;
    (void)graph; (void)ref;
    if (value->kind != TURBOWASM_COMPONENT_TYPE_OWN) return TURBOWASM_TYPE_MISMATCH;
    loan = turbowasm_rt_calloc(1u, sizeof(*loan));
    if (loan == NULL) {
        (void)component_resource_release_from_host(local->exec, value->resource_identity,
            value->as.resource_rep, local->call);
        memset(value, 0, sizeof(*value));
        return TURBOWASM_OUT_OF_MEMORY;
    }
    loan->exec = local->exec; loan->call = local->call;
    loan->identity = value->resource_identity; loan->rep = value->as.resource_rep;
    value->release = component_import_resource_release; value->release_context = loan;
    return TURBOWASM_OK;
}

static turbowasm_status component_local_resource_lower(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    const turbowasm_component_value *value, uint32_t *out) {
    component_local_call *local = context;
    const turbowasm_component_type *type = ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INDEXED
        ? turbowasm_component_type_graph_get(graph, ref.as.indexed) : NULL;
    const turbowasm_component_type *resource;
    component_import_resource_loan *loan;
    turbowasm_status status;
    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_OWN ||
        value->kind != TURBOWASM_COMPONENT_TYPE_OWN || value->release != component_import_resource_release)
        return TURBOWASM_TYPE_MISMATCH;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    loan = value->release_context;
    if (resource == NULL || loan == NULL || loan->committed || loan->staged_handle != 0u ||
        loan->exec != local->exec || loan->identity != resource->as.resource.identity)
        return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_new_owned(&local->exec->resource_table,
        loan->identity, loan->rep, out);
    if (status == TURBOWASM_OK) loan->staged_handle = *out;
    return status;
}

static turbowasm_status component_local_invoke(
    const turbowasm_component_exec_canon_lower_context *context,
    component_local_call *local, turbowasm_component_value *out, turbowasm_trap *trap) {
    turbowasm_component_core_call_adapter adapter;
    turbowasm_component_exec_realloc_context realloc_context;
    turbowasm_status status = initialize_lift_adapter(
        local->exec, local->exec->binary, context->local_adapter_index);
    if (status != TURBOWASM_OK) return status;
    adapter = local->exec->functions[context->local_adapter_index];
    adapter.host_call = local->call;
    adapter.admission_commit = component_local_commit_arguments;
    adapter.admission_context = local;
    adapter.result_owner = component_local_result_owner;
    adapter.result_owner_context = local;
    if (adapter.memory.guest_realloc != NULL) {
        realloc_context = local->exec->realloc_contexts[context->local_adapter_index];
        realloc_context.call = local->call; realloc_context.trap = trap;
        adapter.memory.realloc_context = &realloc_context;
    }
    return turbowasm_component_core_call_invoke(&adapter,
        local->arguments, local->argument_count, out, trap);
}

static turbowasm_status component_resource_builtin_host(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_component_exec_resource_builtin_context *builtin_context =
        (turbowasm_component_exec_resource_builtin_context *)context;
    turbowasm_component_resource_handle handle;
    turbowasm_status status;

    if (builtin_context == NULL ||
        result_count == NULL ||
        trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    *result_count = 0u;

    if (builtin_context->exec != NULL && !builtin_context->exec->may_leave &&
        builtin_context->kind != TURBOWASM_COMPONENT_RESOURCE_BUILTIN_REP) {
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }

    if (builtin_context->external) {
        const turbowasm_component_type *resource_type;

        if (builtin_context->exec == NULL ||
            builtin_context->exec->binary == NULL ||
            builtin_context->exec->imports.resource_drop == NULL ||
            builtin_context->resource_type >=
                builtin_context->exec->binary->type_graph.count ||
            builtin_context->kind !=
                TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP ||
            arguments == NULL ||
            argument_count != 1u ||
            arguments[0].kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_UNSUPPORTED;

        resource_type = turbowasm_component_type_graph_get(
            &builtin_context->exec->binary->type_graph,
            builtin_context->resource_type);
        if (resource_type == NULL ||
            resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
            return TURBOWASM_MALFORMED_MODULE;

        return turbowasm_component_resource_drop(
            &builtin_context->exec->resource_table,
            (uint32_t)arguments[0].as.i32,
            resource_type->as.resource.identity,
            component_external_destructor, builtin_context->exec);
    }

    if (builtin_context->binding == NULL ||
        !builtin_context->binding->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    switch (builtin_context->kind) {
        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_NEW:
            if (arguments == NULL ||
                argument_count != 1u ||
                arguments[0].kind !=
                    builtin_context->binding->rep_kind ||
                results == NULL ||
                result_capacity < 1u)
                return TURBOWASM_INVALID_ARGUMENT;

            status = turbowasm_component_resource_binding_new(
                builtin_context->binding,
                arguments[0],
                &handle);
            if (status != TURBOWASM_OK)
                return status;

            results[0].kind = TURBOWASM_VALUE_I32;
            results[0].as.i32 = (int32_t)handle;
            *result_count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_REP:
            if (arguments == NULL ||
                argument_count != 1u ||
                arguments[0].kind != TURBOWASM_VALUE_I32 ||
                results == NULL ||
                result_capacity < 1u)
                return TURBOWASM_INVALID_ARGUMENT;

            status = turbowasm_component_resource_binding_rep(
                builtin_context->binding,
                (uint32_t)arguments[0].as.i32,
                &results[0]);
            if (status != TURBOWASM_OK)
                return status;

            *result_count = 1u;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP: {
            turbowasm_component_resource_binding binding = *builtin_context->binding;
            turbowasm_component_exec_resource_context destructor_context;
            if (arguments == NULL ||
                argument_count != 1u ||
                arguments[0].kind != TURBOWASM_VALUE_I32)
                return TURBOWASM_INVALID_ARGUMENT;

            if (binding.destructor != NULL) {
                destructor_context = *(const turbowasm_component_exec_resource_context *)
                    binding.destructor_context;
                destructor_context.call = call;
                destructor_context.trap = trap;
                binding.destructor_context = &destructor_context;
            }
            return turbowasm_component_resource_binding_drop(
                &binding,
                (uint32_t)arguments[0].as.i32);
        }

        default:
            return TURBOWASM_INVALID_ARGUMENT;
    }
}

static bool resource_builtin_host_type(
    const turbowasm_component_exec_resource_builtin_context *context,
    turbowasm_host_function_type *out) {
    static const turbowasm_value_kind i32_param[] = {
        TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind i32_result[] = {
        TURBOWASM_VALUE_I32
    };

    if (context == NULL || out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    if (context->external) {
        if (context->kind !=
                TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP ||
            context->exec == NULL ||
            context->exec->imports.resource_drop == NULL)
            return false;
        out->params = i32_param;
        out->param_count = 1u;
        return true;
    }

    if (context->binding == NULL ||
        !context->binding->initialized)
        return false;

    switch (context->kind) {
        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_NEW:
            out->params = &context->binding->rep_kind;
            out->param_count = 1u;
            out->results = i32_result;
            out->result_count = 1u;
            return true;

        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_REP:
            out->params = i32_param;
            out->param_count = 1u;
            out->results = &context->binding->rep_kind;
            out->result_count = 1u;
            return true;

        case TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP:
            out->params = i32_param;
            out->param_count = 1u;
            return true;

        default:
            return false;
    }
}

static turbowasm_status core_pointer_read(
    turbowasm_component_pointer_type pointer_type,
    const turbowasm_value *value,
    uint64_t *out) {
    if (value == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value->kind != TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint32_t)value->as.i32;
        return TURBOWASM_OK;
    }
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I64) {
        if (value->kind != TURBOWASM_VALUE_I64)
            return TURBOWASM_TYPE_MISMATCH;
        *out = (uint64_t)value->as.i64;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status component_canon_lower_host(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_component_exec_canon_lower_context *lower_context =
        (turbowasm_component_exec_canon_lower_context *)context;
    const turbowasm_component_type *function_type;
    const turbowasm_component_canonical_memory *memory = NULL;
    turbowasm_component_canonical_memory call_memory;
    turbowasm_component_exec_realloc_context call_realloc;
    turbowasm_component_value
        component_arguments[TURBOWASM_COMPONENT_MAX_FLAT_PARAMS] = {{0}};
    turbowasm_component_value component_result = {0};
    component_local_call local = {0};
    uint32_t component_param_count;
    uint32_t core_cursor = 0u;
    uint64_t result_pointer = 0u;
    uint32_t i;
    turbowasm_status status = TURBOWASM_OK;

    if (lower_context == NULL ||
        lower_context->exec == NULL ||
        (lower_context->local_adapter_index == UINT32_MAX &&
         lower_context->exec->imports.invoke == NULL) ||
        lower_context->graph == NULL ||
        result_count == NULL ||
        trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    function_type = turbowasm_component_type_graph_get(
        lower_context->graph, lower_context->function_type);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count >
            TURBOWASM_COMPONENT_MAX_FLAT_PARAMS ||
        argument_count !=
            lower_context->flat_signature.param_count ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;

    component_param_count =
        function_type->as.function.param_count;
    local.exec = lower_context->exec; local.call = call;
    local.arguments = component_arguments; local.argument_count = component_param_count;
    if (lower_context->local_adapter_index != UINT32_MAX || lower_context->uses_memory ||
        lower_context->memory.resource_lower != NULL ||
        lower_context->memory.resource_lift != NULL) {
        call_memory = lower_context->memory;
        if (lower_context->local_adapter_index != UINT32_MAX) {
            call_memory.resource_lift = component_local_resource_lift;
            call_memory.resource_lower = component_local_resource_lower;
            call_memory.resource_context = &local;
        }
        if (call_memory.guest_realloc != NULL) {
            call_realloc = lower_context->realloc_context;
            call_realloc.call = call;
            call_realloc.trap = trap;
            call_memory.realloc_context = &call_realloc;
        }
        memory = &call_memory;
    }

    *trap = TURBOWASM_TRAP_NONE;
    *result_count = 0u;

    if (!lower_context->exec->may_leave) {
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }

    for (i = 0u; i < component_param_count; ++i) {
        turbowasm_component_flat_type_list flat;
        turbowasm_component_pointer_type pointer_type =
            memory != NULL
                ? memory->pointer_type
                : TURBOWASM_COMPONENT_POINTER_I32;

        status = turbowasm_component_canonical_flatten_type(
            lower_context->graph,
            function_type->as.function.params[i],
            pointer_type,
            &flat);
        if (status != TURBOWASM_OK)
            goto done;
        if (core_cursor > argument_count ||
            flat.count >
                (uint32_t)argument_count - core_cursor) {
            status = TURBOWASM_TYPE_MISMATCH;
            goto done;
        }

        status = turbowasm_component_canonical_lift_flat_value(
            lower_context->graph,
            function_type->as.function.params[i],
            memory,
            &arguments[core_cursor],
            flat.count,
            &component_arguments[i]);
        if (status != TURBOWASM_OK)
            goto done;
        core_cursor += flat.count;
    }

    if (lower_context->flat_signature.results_indirect) {
        if (memory == NULL ||
            core_cursor >= argument_count) {
            status = TURBOWASM_TYPE_MISMATCH;
            goto done;
        }
        status = core_pointer_read(
            memory->pointer_type,
            &arguments[core_cursor],
            &result_pointer);
        if (status != TURBOWASM_OK)
            goto done;
        ++core_cursor;
    }

    if (core_cursor != argument_count) {
        status = TURBOWASM_TYPE_MISMATCH;
        goto done;
    }

    if (lower_context->local_adapter_index != UINT32_MAX) {
        status = component_local_invoke(lower_context, &local,
            function_type->as.function.has_result ? &component_result : NULL, trap);
    } else {
        for (i = 0u; i < component_param_count; ++i)
            component_import_commit_resources(&component_arguments[i]);
        status = lower_context->exec->imports.invoke(
        lower_context->exec->imports.context,
        call,
        lower_context->instance_name,
        lower_context->function_name,
        lower_context->graph,
        lower_context->function_type,
        component_arguments,
        component_param_count,
        function_type->as.function.has_result
            ? &component_result
            : NULL,
        trap);
    }
    if (status != TURBOWASM_OK ||
        *trap != TURBOWASM_TRAP_NONE)
        goto done;

    if (function_type->as.function.has_result) {
        if (lower_context->flat_signature.results_indirect) {
            status = turbowasm_component_canonical_lower_value(
                lower_context->graph,
                function_type->as.function.result,
                memory,
                result_pointer,
                &component_result);
            if (status != TURBOWASM_OK)
                goto done;
            *result_count = 0u;
        } else {
            uint32_t flat_count = 0u;

            if (lower_context->flat_signature.result_count >
                    result_capacity ||
                (lower_context->flat_signature.result_count != 0u &&
                 results == NULL)) {
                status = TURBOWASM_INVALID_ARGUMENT;
                goto done;
            }

            status =
                turbowasm_component_canonical_lower_flat_value(
                    lower_context->graph,
                    function_type->as.function.result,
                    memory,
                    &component_result,
                    results,
                    (uint32_t)result_capacity,
                    &flat_count);
            if (status != TURBOWASM_OK)
                goto done;
            if (flat_count !=
                lower_context->flat_signature.result_count) {
                status = TURBOWASM_TYPE_MISMATCH;
                goto done;
            }
            *result_count = flat_count;
        }
    }

    if (lower_context->local_adapter_index != UINT32_MAX)
        component_import_commit_resources(&component_result);

done:
    for (i = 0u; i < component_param_count; ++i)
        turbowasm_component_value_destroy(
            &component_arguments[i]);
    turbowasm_component_value_destroy(&component_result);
    return status;
}

static const turbowasm_component_core_inline_export *
find_inline_core_export(
    const turbowasm_component_core_instance_def *definition,
    turbowasm_component_name name) {
    uint32_t i;

    if (definition == NULL ||
        definition->kind !=
            TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE)
        return NULL;

    for (i = 0u; i < definition->export_count; ++i) {
        const turbowasm_component_core_inline_export *export_desc =
            &definition->exports[i];
        if (component_name_equal(
                export_desc->name,
                name.bytes,
                name.size))
            return export_desc;
    }
    return NULL;
}

static turbowasm_status define_inline_provider(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    uint32_t provider_index,
    turbowasm_name module_name,
    turbowasm_linker *linker) {
    const turbowasm_component_core_instance_def *definition;
    uint32_t i;

    if (exec == NULL || binary == NULL || linker == NULL ||
        provider_index >= binary->core_instance_count)
        return TURBOWASM_INVALID_ARGUMENT;

    definition = &binary->core_instances[provider_index];
    if (definition->kind !=
        TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < definition->export_count; ++i) {
        const turbowasm_component_core_inline_export *export_desc =
            &definition->exports[i];
        const turbowasm_component_exec_core_function *function;
        turbowasm_host_function_type type;
        turbowasm_host_function_fn host_function;
        void *host_context;
        turbowasm_name name;

        if (export_desc->sort != 0x00u ||
            export_desc->item_index >= exec->core_function_count)
            return TURBOWASM_UNSUPPORTED;

        function = &exec->core_functions[export_desc->item_index];
        memset(&type, 0, sizeof(type));
        host_function = NULL;
        host_context = NULL;

        if (function->kind ==
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN) {
            const turbowasm_component_exec_resource_builtin_context *context;

            if (function->resource_builtin_index >=
                    binary->resource_builtin_count)
                return TURBOWASM_MALFORMED_MODULE;
            context =
                &exec->resource_builtin_contexts[
                    function->resource_builtin_index];
            if (!resource_builtin_host_type(context, &type))
                return TURBOWASM_MALFORMED_MODULE;
            host_function = component_resource_builtin_host;
            host_context = (void *)context;
        } else if (function->kind ==
                       TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_CANON_LOWER) {
            turbowasm_component_exec_canon_lower_context *context;

            if (function->canon_lower_index >=
                    binary->canon_lower_count ||
                exec->canon_lower_contexts == NULL)
                return TURBOWASM_MALFORMED_MODULE;
            context =
                &exec->canon_lower_contexts[
                    function->canon_lower_index];
            type = context->host_type;
            host_function = component_canon_lower_host;
            host_context = context;
        } else {
            return TURBOWASM_UNSUPPORTED;
        }

        name.bytes = export_desc->name.bytes;
        name.size = export_desc->name.size;

        {
            turbowasm_status status =
                turbowasm_linker_define_host_function(
                    linker,
                    module_name,
                    name,
                    &type,
                    host_function,
                    host_context);
            if (status != TURBOWASM_OK)
                return status;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status resolve_core_aliases_for_provider(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    uint32_t provider_index) {
    const turbowasm_component_core_instance_def *provider;
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        provider_index >= binary->core_instance_count)
        return TURBOWASM_INVALID_ARGUMENT;

    provider = &binary->core_instances[provider_index];

    for (i = 0u; i < binary->core_function_alias_count; ++i) {
        const turbowasm_component_core_function_alias *alias =
            &binary->core_function_aliases[i];
        turbowasm_component_exec_core_function *target;

        if (alias->instance_index != provider_index)
            continue;
        if (alias->core_function_index >=
            exec->core_function_count)
            return TURBOWASM_MALFORMED_MODULE;

        target =
            &exec->core_functions[alias->core_function_index];
        if (target->kind !=
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID)
            return TURBOWASM_MALFORMED_MODULE;

        if (provider->kind ==
            TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
            const turbowasm_module *module;
            const turbowasm_export_desc *export_desc;

            if (provider_index >= exec->core_instance_count ||
                exec->core_instances[provider_index].impl == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            module = turbowasm_instance_module(
                &exec->core_instances[provider_index]);
            export_desc = find_core_function_export(
                module, alias->name);
            if (export_desc == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            target->kind =
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE;
            target->instance_index = provider_index;
            target->function_index = export_desc->item_index;
        } else if (provider->kind ==
                   TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE) {
            const turbowasm_component_core_inline_export *export_desc;
            const turbowasm_component_exec_core_function *source;

            export_desc = find_inline_core_export(
                provider, alias->name);
            if (export_desc == NULL ||
                export_desc->sort != 0x00u ||
                export_desc->item_index >=
                    exec->core_function_count)
                return TURBOWASM_MALFORMED_MODULE;

            source =
                &exec->core_functions[export_desc->item_index];
            if (source->kind ==
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID)
                return TURBOWASM_MALFORMED_MODULE;

            *target = *source;
        } else {
            return TURBOWASM_MALFORMED_MODULE;
        }
    }

    return TURBOWASM_OK;
}

static turbowasm_status instantiate_core_instance(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    uint32_t index) {
    const turbowasm_component_core_instance_def *definition;
    turbowasm_linker linker = {0};
    bool has_linker = false;
    turbowasm_status status;
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        index >= binary->core_instance_count)
        return TURBOWASM_INVALID_ARGUMENT;

    definition = &binary->core_instances[index];

    if (definition->kind ==
        TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE)
        return TURBOWASM_OK;

    if (definition->kind !=
            TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE ||
        definition->module_index >= exec->core_module_count)
        return TURBOWASM_MALFORMED_MODULE;

    if (definition->argument_count == 0u)
        return turbowasm_instance_create(
            &exec->core_instances[index],
            &exec->core_modules[definition->module_index]);

    status = turbowasm_linker_init_with_config(
        &linker, &binary->config);
    if (status != TURBOWASM_OK)
        return status;
    has_linker = true;

    for (i = 0u; i < definition->argument_count; ++i) {
        const turbowasm_component_core_instantiate_arg *argument =
            &definition->arguments[i];
        const turbowasm_component_core_instance_def *provider;
        turbowasm_name name;

        if (argument->instance_index >= index) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }

        provider =
            &binary->core_instances[argument->instance_index];

        name.bytes = argument->name.bytes;
        name.size = argument->name.size;

        if (provider->kind ==
            TURBOWASM_COMPONENT_CORE_INSTANCE_INLINE) {
            status = define_inline_provider(
                exec,
                binary,
                argument->instance_index,
                name,
                &linker);
        } else if (provider->kind ==
                   TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
            if (exec->core_instances[
                    argument->instance_index].impl == NULL) {
                status = TURBOWASM_MALFORMED_MODULE;
            } else {
                status = turbowasm_linker_define_instance(
                    &linker,
                    name,
                    &exec->core_instances[
                        argument->instance_index]);
            }
        } else {
            status = TURBOWASM_MALFORMED_MODULE;
        }

        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_instance_create_linked(
        &exec->core_instances[index],
        &exec->core_modules[definition->module_index],
        &linker);

done:
    if (has_linker)
        turbowasm_linker_destroy(&linker);
    return status;
}

static turbowasm_status initialize_resource_state(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary) {
    uint32_t i;

    if (exec == NULL || binary == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!turbowasm_component_resource_table_init(
            &exec->resource_table, 0u))
        return TURBOWASM_INVALID_ARGUMENT;

    exec->resource_binding_count = binary->type_graph.count;
    if (exec->resource_binding_count != 0u) {
        if ((size_t)exec->resource_binding_count >
                SIZE_MAX / sizeof(*exec->resource_bindings) ||
            (size_t)exec->resource_binding_count >
                SIZE_MAX / sizeof(*exec->resource_contexts))
            return TURBOWASM_OUT_OF_MEMORY;

        exec->resource_bindings =
            (turbowasm_component_resource_binding *)
                turbowasm_rt_calloc(
                    exec->resource_binding_count,
                    sizeof(*exec->resource_bindings));
        exec->resource_contexts =
            (turbowasm_component_exec_resource_context *)
                turbowasm_rt_calloc(
                    exec->resource_binding_count,
                    sizeof(*exec->resource_contexts));
        if (exec->resource_bindings == NULL ||
            exec->resource_contexts == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (binary->resource_builtin_count != 0u) {
        if ((size_t)binary->resource_builtin_count >
            SIZE_MAX / sizeof(*exec->resource_builtin_contexts))
            return TURBOWASM_OUT_OF_MEMORY;

        exec->resource_builtin_contexts =
            (turbowasm_component_exec_resource_builtin_context *)
                turbowasm_rt_calloc(
                    binary->resource_builtin_count,
                    sizeof(*exec->resource_builtin_contexts));
        if (exec->resource_builtin_contexts == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    for (i = 0u; i < binary->resource_builtin_count; ++i) {
        const turbowasm_component_resource_builtin *builtin =
            &binary->resource_builtins[i];
        const turbowasm_component_type *resource_type;
        turbowasm_component_resource_binding *binding;
        turbowasm_component_exec_resource_context *resource_context;
        turbowasm_component_exec_core_function *core_function;
        turbowasm_status status;

        if (builtin->resource_type >=
                exec->resource_binding_count ||
            builtin->core_function_index >=
                exec->core_function_count)
            return TURBOWASM_MALFORMED_MODULE;

        resource_type = turbowasm_component_resource_definition(
            &binary->type_graph, builtin->resource_type);
        if (resource_type == NULL ||
            resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
            return TURBOWASM_MALFORMED_MODULE;

        if (resource_type->as.resource.has_destructor &&
            resource_type->as.resource.destructor_index >=
                exec->core_function_count)
            return TURBOWASM_MALFORMED_MODULE;

        binding =
            &exec->resource_bindings[builtin->resource_type];
        resource_context =
            &exec->resource_contexts[builtin->resource_type];

        if (resource_type->as.resource.identity_alias) {
            turbowasm_component_exec_resource_builtin_context *builtin_context =
                &exec->resource_builtin_contexts[i];

            if (builtin->kind !=
                    TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP ||
                exec->imports.resource_drop == NULL)
                return TURBOWASM_UNSUPPORTED;

            builtin_context->exec = exec;
            builtin_context->binding = NULL;
            builtin_context->kind = builtin->kind;
            builtin_context->resource_type = builtin->resource_type;
            builtin_context->external = true;

            core_function =
                &exec->core_functions[builtin->core_function_index];
            if (core_function->kind !=
                    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID)
                return TURBOWASM_MALFORMED_MODULE;
            core_function->kind =
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN;
            core_function->resource_builtin_index = i;
            continue;
        }

        if (!binding->initialized) {
            resource_context->exec = exec;
            resource_context->resource_type = (uint32_t)(
                resource_type - binary->type_graph.types);

            status = turbowasm_component_resource_binding_init(
                binding,
                &binary->type_graph,
                builtin->resource_type,
                &exec->resource_table,
                resource_type->as.resource.has_destructor
                    ? component_resource_destructor_bridge
                    : NULL,
                resource_type->as.resource.has_destructor
                    ? resource_context
                    : NULL);
            if (status != TURBOWASM_OK)
                return status;
        }

        exec->resource_builtin_contexts[i].exec = exec;
        exec->resource_builtin_contexts[i].binding = binding;
        exec->resource_builtin_contexts[i].kind = builtin->kind;
        exec->resource_builtin_contexts[i].resource_type =
            builtin->resource_type;
        exec->resource_builtin_contexts[i].external = false;

        core_function =
            &exec->core_functions[builtin->core_function_index];
        if (core_function->kind !=
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID)
            return TURBOWASM_MALFORMED_MODULE;

        core_function->kind =
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN;
        core_function->resource_builtin_index = i;
    }

    return TURBOWASM_OK;
}

static const turbowasm_component_inline_export *
find_component_instance_export(
    const turbowasm_component_instance_def *instance,
    turbowasm_component_name name) {
    uint32_t i;

    if (instance == NULL)
        return NULL;

    for (i = 0u; i < instance->export_count; ++i) {
        const turbowasm_component_inline_export *export_desc =
            &instance->exports[i];

        if (export_desc->kind ==
                TURBOWASM_COMPONENT_EXTERN_FUNCTION &&
            component_name_equal(
                export_desc->name,
                name.bytes,
                name.size))
            return export_desc;
    }

    return NULL;
}

static const turbowasm_component_import *
find_component_instance_import(
    const turbowasm_component_binary *binary,
    uint32_t instance_index) {
    uint32_t i;

    if (binary == NULL)
        return NULL;
    for (i = 0u; i < binary->import_count; ++i) {
        const turbowasm_component_import *import_desc =
            &binary->imports[i];
        if (import_desc->kind == TURBOWASM_COMPONENT_EXTERN_INSTANCE &&
            import_desc->item_index == instance_index)
            return import_desc;
    }
    return NULL;
}

static const turbowasm_component_instance_def *
find_component_instance_definition(
    const turbowasm_component_binary *binary,
    uint32_t instance_index) {
    uint32_t i;

    if (binary == NULL)
        return NULL;
    for (i = 0u; i < binary->component_instance_count; ++i) {
        if (binary->component_instances[i].component_instance_index ==
            instance_index)
            return &binary->component_instances[i];
    }
    return NULL;
}

static const turbowasm_component_instance_type_export *
find_instance_type_function_export(
    const turbowasm_component_instance_type *instance_type,
    turbowasm_component_name name) {
    uint32_t i;

    if (instance_type == NULL)
        return NULL;
    for (i = 0u; i < instance_type->export_count; ++i) {
        const turbowasm_component_instance_type_export *export_desc =
            &instance_type->exports[i];
        if (export_desc->kind ==
                TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION &&
            export_desc->name_size == name.size &&
            (name.size == 0u ||
             (export_desc->name != NULL &&
              name.bytes != NULL &&
              memcmp(export_desc->name, name.bytes, name.size) == 0)))
            return export_desc;
    }
    return NULL;
}

static const turbowasm_component_function_alias *
find_component_function_alias(
    const turbowasm_component_binary *binary,
    uint32_t function_index) {
    uint32_t i;

    if (binary == NULL)
        return NULL;
    /* Sources precede exports, so a reverse scan follows an arbitrary chain
     * without recursion or additional allocation. */
    for (i = binary->component_function_alias_count; i != 0u; --i) {
        const turbowasm_component_function_alias *alias =
            &binary->component_function_aliases[i - 1u];
        if (alias->component_function_index != function_index)
            continue;
        if (!alias->local_source)
            return alias;
        function_index = alias->source_function_index;
    }
    return NULL;
}

static const turbowasm_component_core_memory_alias *
find_core_memory_alias_by_index(
    const turbowasm_component_binary *binary,
    uint32_t memory_index) {
    uint32_t i;

    if (binary == NULL)
        return NULL;
    for (i = 0u; i < binary->core_memory_alias_count; ++i) {
        if (binary->core_memory_aliases[i].core_memory_index ==
            memory_index)
            return &binary->core_memory_aliases[i];
    }
    return NULL;
}

static const turbowasm_component_core_function_alias *
find_core_function_alias_by_index(
    const turbowasm_component_binary *binary,
    uint32_t function_index) {
    uint32_t i;

    if (binary == NULL)
        return NULL;
    for (i = 0u; i < binary->core_function_alias_count; ++i) {
        if (binary->core_function_aliases[i].core_function_index ==
            function_index)
            return &binary->core_function_aliases[i];
    }
    return NULL;
}

static bool type_ref_contains_dynamic_memory(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    uint32_t features;
    return !turbowasm_component_value_type_features(graph, ref, &features) ||
        (features & TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY) != 0u;
}

static turbowasm_status configure_canon_lower_memory(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_component_canon_lower *lower,
    turbowasm_component_exec_canon_lower_context *context) {
    const turbowasm_component_core_memory_alias *memory_alias;
    const turbowasm_component_core_instance_def *provider;
    const turbowasm_module *module;
    const turbowasm_export_desc *export_desc;
    turbowasm_memory_desc memory_desc;

    if (exec == NULL || binary == NULL ||
        lower == NULL || context == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Canonical resource handles are always i32 in the flat ABI and do not
     * require a guest linear-memory option. Keep the external resource codec
     * available even for resource-only lowerings such as:
     *
     *   get-stdin() -> own<input-stream>
     *   subscribe(borrow<input-stream>) -> own<pollable>
     */
    context->memory.pointer_type = TURBOWASM_COMPONENT_POINTER_I32;
    context->memory.string_encoding = lower->string_encoding;
    if (exec->imports.resource_lower != NULL ||
        exec->imports.resource_lift != NULL) {
        context->memory.resource_lower =
            component_import_resource_lower;
        context->memory.resource_lift =
            component_import_resource_lift;
        context->memory.resource_context =
            exec;
    }

    if (!lower->has_memory)
        return TURBOWASM_OK;

    memory_alias = find_core_memory_alias_by_index(
        binary, lower->memory_index);
    if (memory_alias == NULL ||
        memory_alias->instance_index >=
            binary->core_instance_count)
        return TURBOWASM_MALFORMED_MODULE;

    provider =
        &binary->core_instances[memory_alias->instance_index];
    if (provider->kind !=
            TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE ||
        provider->module_index >= exec->core_module_count)
        return TURBOWASM_UNSUPPORTED;

    module = &exec->core_modules[provider->module_index];
    export_desc = find_core_memory_export(
        module, memory_alias->name);
    if (export_desc == NULL ||
        !turbowasm_module_memory_at(
            module, export_desc->item_index,
            &memory_desc))
        return TURBOWASM_MALFORMED_MODULE;

    context->uses_memory = true;
    context->memory.instance =
        &exec->core_instances[memory_alias->instance_index];
    context->memory.memory_index = export_desc->item_index;
    context->memory.pointer_type = memory_desc.memory64
        ? TURBOWASM_COMPONENT_POINTER_I64
        : TURBOWASM_COMPONENT_POINTER_I32;
    context->memory.string_encoding = lower->string_encoding;

    if (lower->has_realloc) {
        const turbowasm_component_core_function_alias *function_alias;
        const turbowasm_component_core_instance_def *function_provider;
        const turbowasm_module *function_module;
        const turbowasm_export_desc *function_export;

        function_alias = find_core_function_alias_by_index(
            binary, lower->realloc_function_index);
        if (function_alias == NULL ||
            function_alias->instance_index >=
                binary->core_instance_count)
            return TURBOWASM_UNSUPPORTED;

        function_provider =
            &binary->core_instances[
                function_alias->instance_index];
        if (function_provider->kind !=
                TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE ||
            function_provider->module_index >=
                exec->core_module_count)
            return TURBOWASM_UNSUPPORTED;

        function_module =
            &exec->core_modules[
                function_provider->module_index];
        function_export = find_core_function_export(
            function_module, function_alias->name);
        if (function_export == NULL ||
            !core_module_function_has_pointer_signature(
                function_module,
                function_export->item_index,
                context->memory.pointer_type))
            return TURBOWASM_TYPE_MISMATCH;

        context->realloc_context.instance =
            &exec->core_instances[
                function_alias->instance_index];
        context->realloc_context.function_index =
            function_export->item_index;
        context->realloc_context.pointer_type =
            context->memory.pointer_type;
        context->realloc_context.may_leave = &exec->may_leave;
        context->memory.guest_realloc =
            component_guest_realloc;
        context->memory.realloc_context =
            &context->realloc_context;
    }

    return TURBOWASM_OK;
}

static bool flat_kind_to_value_kind(
    turbowasm_component_flat_type flat,
    turbowasm_value_kind *out) {
    if (out == NULL)
        return false;
    switch (flat) {
        case TURBOWASM_COMPONENT_FLAT_I32:
            *out = TURBOWASM_VALUE_I32;
            return true;
        case TURBOWASM_COMPONENT_FLAT_I64:
            *out = TURBOWASM_VALUE_I64;
            return true;
        case TURBOWASM_COMPONENT_FLAT_F32:
            *out = TURBOWASM_VALUE_F32;
            return true;
        case TURBOWASM_COMPONENT_FLAT_F64:
            *out = TURBOWASM_VALUE_F64;
            return true;
        default:
            return false;
    }
}

static turbowasm_status initialize_canon_lower_state(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary) {
    uint32_t i;

    if (exec == NULL || binary == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (binary->canon_lower_count == 0u)
        return TURBOWASM_OK;
    if ((size_t)binary->canon_lower_count >
        SIZE_MAX / sizeof(*exec->canon_lower_contexts))
        return TURBOWASM_OUT_OF_MEMORY;

    exec->canon_lower_contexts =
        (turbowasm_component_exec_canon_lower_context *)
            turbowasm_rt_calloc(
                binary->canon_lower_count,
                sizeof(*exec->canon_lower_contexts));
    if (exec->canon_lower_contexts == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < binary->canon_lower_count; ++i) {
        const turbowasm_component_canon_lower *lower =
            &binary->canon_lowers[i];
        const turbowasm_component_function_alias *alias;
        const turbowasm_component_import *instance_import;
        const turbowasm_component_type *outer_instance_type;
        const turbowasm_component_instance_type_export *function_export;
        const turbowasm_component_type *function_type;
        turbowasm_component_exec_canon_lower_context *context =
            &exec->canon_lower_contexts[i];
        turbowasm_component_flat_signature signature;
        uint32_t j;

        if (lower->core_function_index >=
                exec->core_function_count ||
            lower->component_function_index >=
                binary->component_function_count)
            return TURBOWASM_MALFORMED_MODULE;

        context->exec = exec;
        context->local_adapter_index = exec->function_adapter_indices[lower->component_function_index];
        if (context->local_adapter_index != UINT32_MAX) {
            if (context->local_adapter_index >= binary->canon_lift_count)
                return TURBOWASM_MALFORMED_MODULE;
            context->graph = &binary->type_graph;
            context->function_type = binary->canon_lifts[context->local_adapter_index].type_index;
        } else {
            if (exec->imports.can_bind == NULL || exec->imports.invoke == NULL)
                return TURBOWASM_UNSUPPORTED;
            alias = find_component_function_alias(
                binary, lower->component_function_index);
            if (alias == NULL)
                return TURBOWASM_UNSUPPORTED;

            instance_import = find_component_instance_import(
                binary, alias->instance_index);
            if (instance_import == NULL)
                return TURBOWASM_UNSUPPORTED;

            outer_instance_type =
                turbowasm_component_type_graph_get(
                    &binary->type_graph,
                    instance_import->type_index);
            if (outer_instance_type == NULL ||
                outer_instance_type->kind !=
                    TURBOWASM_COMPONENT_TYPE_INSTANCE ||
                outer_instance_type->as.instance == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            function_export = find_instance_type_function_export(
                outer_instance_type->as.instance, alias->name);
            if (function_export == NULL)
                return TURBOWASM_MALFORMED_MODULE;

            context->instance_name = instance_import->name;
            context->function_name = alias->name;
            context->graph =
                &outer_instance_type->as.instance->type_graph;
            context->function_type = function_export->type_index;

        }
        function_type = turbowasm_component_type_graph_get(context->graph, context->function_type);
        if (function_type == NULL || function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
            return TURBOWASM_MALFORMED_MODULE;

        if (lower->has_realloc && !lower->has_memory)
            return TURBOWASM_MALFORMED_MODULE;

        {
            turbowasm_status status =
                configure_canon_lower_memory(
                    exec, binary, lower, context);
            if (status != TURBOWASM_OK)
                return status;
        }

        memset(&signature, 0, sizeof(signature));
        {
            turbowasm_component_pointer_type pointer_type =
                context->uses_memory
                    ? context->memory.pointer_type
                    : TURBOWASM_COMPONENT_POINTER_I32;
            turbowasm_status status =
                turbowasm_component_canonical_flatten_function(
                    context->graph,
                    context->function_type,
                    pointer_type,
                    TURBOWASM_COMPONENT_CANONICAL_LOWER,
                    &signature);
            if (status != TURBOWASM_OK)
                return status;
        }

        /*
         * W2b3c keeps >16-parameter tuple passing fail-closed, but admits the
         * standard synchronous indirect-result out-pointer shape.
         */
        if (signature.params_indirect ||
            signature.param_count >
                TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS ||
            signature.result_count >
                TURBOWASM_COMPONENT_MAX_FLAT_RESULTS)
            return TURBOWASM_UNSUPPORTED;

        if (signature.results_indirect && !context->uses_memory)
            return TURBOWASM_UNSUPPORTED;

        for (j = 0u;
             j < function_type->as.function.param_count;
             ++j) {
            if (type_ref_contains_dynamic_memory(
                    context->graph,
                    function_type->as.function.params[j]) &&
                !context->uses_memory)
                return TURBOWASM_UNSUPPORTED;
        }

        if (function_type->as.function.has_result &&
            type_ref_contains_dynamic_memory(
                context->graph,
                function_type->as.function.result) &&
            !lower->has_realloc)
            return TURBOWASM_UNSUPPORTED;

        context->flat_signature = signature;

        for (j = 0u; j < signature.param_count; ++j) {
            if (!flat_kind_to_value_kind(
                    signature.params[j],
                    &context->params[j]))
                return TURBOWASM_UNSUPPORTED;
        }
        for (j = 0u; j < signature.result_count; ++j) {
            if (!flat_kind_to_value_kind(
                    signature.results[j],
                    &context->results[j]))
                return TURBOWASM_UNSUPPORTED;
        }

        context->host_type.params =
            signature.param_count != 0u
                ? context->params
                : NULL;
        context->host_type.param_count = signature.param_count;
        context->host_type.results =
            signature.result_count != 0u
                ? context->results
                : NULL;
        context->host_type.result_count = signature.result_count;

        if (context->local_adapter_index == UINT32_MAX && !exec->imports.can_bind(
                exec->imports.context,
                context->instance_name,
                context->function_name,
                context->graph,
                context->function_type))
            return TURBOWASM_LINK_ERROR;

        if (exec->core_functions[
                lower->core_function_index].kind !=
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID)
            return TURBOWASM_MALFORMED_MODULE;

        exec->core_functions[
            lower->core_function_index].kind =
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_CANON_LOWER;
        exec->core_functions[
            lower->core_function_index].canon_lower_index = i;
    }

    return TURBOWASM_OK;
}

static turbowasm_status resolve_component_function_aliases(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary) {
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        exec->function_adapter_indices == NULL)
        return binary != NULL && binary->component_function_count == 0u
            ? TURBOWASM_OK
            : TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u;
         i < binary->component_function_alias_count;
         ++i) {
        const turbowasm_component_function_alias *alias =
            &binary->component_function_aliases[i];
        const turbowasm_component_instance_def *instance;
        const turbowasm_component_inline_export *export_desc;
        uint32_t source_adapter;

        if (alias->component_function_index >= exec->function_count)
            return TURBOWASM_MALFORMED_MODULE;

        if (alias->local_source) {
            if (alias->source_function_index >= alias->component_function_index)
                return TURBOWASM_MALFORMED_MODULE;
            exec->function_adapter_indices[alias->component_function_index] =
                exec->function_adapter_indices[alias->source_function_index];
            continue;
        }

        /*
         * Imported-instance functions are consumed by canon lower and do not
         * have a Core-call lift adapter. Their execution mapping was qualified
         * by initialize_canon_lower_state().
         */
        if (find_component_instance_import(
                binary, alias->instance_index) != NULL)
            continue;

        instance = find_component_instance_definition(
            binary, alias->instance_index);
        if (instance == NULL)
            return TURBOWASM_MALFORMED_MODULE;

        export_desc = find_component_instance_export(
            instance, alias->name);
        if (export_desc == NULL ||
            export_desc->item_index >= exec->function_count)
            return TURBOWASM_MALFORMED_MODULE;

        source_adapter =
            exec->function_adapter_indices[
                export_desc->item_index];
        if (source_adapter == UINT32_MAX ||
            source_adapter >= exec->adapter_count)
            return TURBOWASM_MALFORMED_MODULE;

        if (exec->function_adapter_indices[
                alias->component_function_index] != UINT32_MAX)
            return TURBOWASM_MALFORMED_MODULE;

        exec->function_adapter_indices[
            alias->component_function_index] = source_adapter;
    }

    return TURBOWASM_OK;
}

static void destroy_partial(
    turbowasm_component_exec *exec) {
    uint32_t i;

    if (exec == NULL)
        return;

    if (exec->functions != NULL) {
        for (i = 0u; i < exec->adapter_count; ++i)
            turbowasm_component_core_call_adapter_destroy(
                &exec->functions[i]);
    }

    if (exec->resource_bindings != NULL) {
        for (i = 0u; i < exec->resource_binding_count; ++i)
            turbowasm_component_resource_binding_destroy(
                &exec->resource_bindings[i]);
    }

    turbowasm_component_resource_table_destroy(
        &exec->resource_table);

    if (exec->core_instances != NULL &&
        exec->binary != NULL) {
        for (i = exec->core_instance_count; i != 0u; --i) {
            uint32_t index = i - 1u;
            if (index < exec->binary->core_instance_count &&
                exec->binary->core_instances[index].kind ==
                    TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
                turbowasm_instance_destroy(
                    &exec->core_instances[index]);
            }
        }
    }

    if (exec->core_modules != NULL) {
        for (i = exec->core_module_count; i != 0u; --i)
            turbowasm_module_destroy(
                &exec->core_modules[i - 1u]);
    }

    turbowasm_rt_free(exec->import_sets);
    turbowasm_rt_free(exec->function_adapter_indices);
    turbowasm_rt_free(exec->functions);
    turbowasm_rt_free(exec->realloc_contexts);
    turbowasm_rt_free(exec->resource_builtin_contexts);
    turbowasm_rt_free(exec->canon_lower_contexts);
    turbowasm_rt_free(exec->resource_contexts);
    turbowasm_rt_free(exec->resource_bindings);
    turbowasm_rt_free(exec->core_memories);
    turbowasm_rt_free(exec->core_functions);
    turbowasm_rt_free(exec->core_instances);
    turbowasm_rt_free(exec->core_modules);
    memset(exec, 0, sizeof(*exec));
}

static const turbowasm_component_exec_imports *
component_import_router_select(
    const turbowasm_component_exec *exec,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    const turbowasm_component_exec_imports *selected = NULL;
    uint32_t i;

    if (exec == NULL)
        return NULL;

    for (i = 0u; i < exec->import_set_count; ++i) {
        const turbowasm_component_exec_imports *candidate =
            &exec->import_sets[i];

        if (!candidate->can_bind(
                candidate->context,
                instance_name,
                function_name,
                graph,
                function_type))
            continue;

        /*
         * Import ownership is nominal and must be unambiguous. Refuse to make
         * registration order observable when two capability sets claim the
         * same imported function.
         */
        if (selected != NULL)
            return NULL;
        selected = candidate;
    }

    return selected;
}

static bool component_import_router_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    return component_import_router_select(
               (const turbowasm_component_exec *)context,
               instance_name,
               function_name,
               graph,
               function_type) != NULL;
}

static turbowasm_status component_import_router_invoke(
    void *context,
    turbowasm_host_call *call,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    turbowasm_component_exec *exec =
        (turbowasm_component_exec *)context;
    const turbowasm_component_exec_imports *selected =
        component_import_router_select(
            exec,
            instance_name,
            function_name,
            graph,
            function_type);

    if (selected == NULL)
        return TURBOWASM_LINK_ERROR;

    return selected->invoke(
        selected->context,
        call,
        instance_name,
        function_name,
        graph,
        function_type,
        arguments,
        argument_count,
        out_result,
        trap);
}

static turbowasm_status component_import_router_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_component_exec *exec =
        (turbowasm_component_exec *)context;
    uint32_t i;

    if (exec == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->import_set_count; ++i) {
        const turbowasm_component_exec_imports *candidate =
            &exec->import_sets[i];
        turbowasm_status status;

        if (candidate->resource_lower == NULL)
            continue;
        status = candidate->resource_lower(
            candidate->context,
            graph,
            type,
            value,
            out_handle);
        if (status == TURBOWASM_TYPE_MISMATCH)
            continue;
        return status;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status component_import_router_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_component_exec *exec =
        (turbowasm_component_exec *)context;
    uint32_t i;

    if (exec == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->import_set_count; ++i) {
        const turbowasm_component_exec_imports *candidate =
            &exec->import_sets[i];
        turbowasm_status status;

        if (candidate->resource_lift == NULL)
            continue;
        status = candidate->resource_lift(
            candidate->context,
            graph,
            type,
            handle,
            out);
        if (status == TURBOWASM_TYPE_MISMATCH)
            continue;
        return status;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status component_import_router_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    turbowasm_component_exec *exec =
        (turbowasm_component_exec *)context;
    uint32_t i;

    if (exec == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->import_set_count; ++i) {
        const turbowasm_component_exec_imports *candidate =
            &exec->import_sets[i];
        turbowasm_status status;

        if (candidate->resource_drop == NULL)
            continue;
        status = candidate->resource_drop(
            candidate->context,
            resource_identity,
            handle);
        if (status == TURBOWASM_TYPE_MISMATCH)
            continue;
        return status;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static turbowasm_status component_import_router_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_exec_imports *import_sets,
    size_t import_set_count) {
    size_t i;
    bool has_resource_lower = false;
    bool has_resource_lift = false;
    bool has_resource_drop = false;

    if (exec == NULL ||
        (import_set_count != 0u && import_sets == NULL) ||
        import_set_count > UINT32_MAX ||
        import_set_count >
            SIZE_MAX / sizeof(*exec->import_sets))
        return TURBOWASM_INVALID_ARGUMENT;

    if (import_set_count == 0u)
        return TURBOWASM_OK;

    for (i = 0u; i < import_set_count; ++i) {
        if (import_sets[i].can_bind == NULL ||
            import_sets[i].invoke == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        has_resource_lower =
            has_resource_lower ||
            import_sets[i].resource_lower != NULL;
        has_resource_lift =
            has_resource_lift ||
            import_sets[i].resource_lift != NULL;
        has_resource_drop =
            has_resource_drop ||
            import_sets[i].resource_drop != NULL;
    }

    exec->import_sets =
        (turbowasm_component_exec_imports *)turbowasm_rt_calloc(
            import_set_count,
            sizeof(*exec->import_sets));
    if (exec->import_sets == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    memcpy(
        exec->import_sets,
        import_sets,
        import_set_count * sizeof(*exec->import_sets));
    exec->import_set_count = (uint32_t)import_set_count;

    memset(&exec->imports, 0, sizeof(exec->imports));
    exec->imports.context = exec;
    exec->imports.can_bind = component_import_router_can_bind;
    exec->imports.invoke = component_import_router_invoke;
    if (has_resource_lower)
        exec->imports.resource_lower =
            component_import_router_resource_lower;
    if (has_resource_lift)
        exec->imports.resource_lift =
            component_import_router_resource_lift;
    if (has_resource_drop)
        exec->imports.resource_drop =
            component_import_router_resource_drop;

    return TURBOWASM_OK;
}

static turbowasm_status initialize_lift_adapter(
    turbowasm_component_exec *exec, const turbowasm_component_binary *binary, uint32_t i) {
    turbowasm_status status;
    if (i >= exec->adapter_count) return TURBOWASM_MALFORMED_MODULE;
    if (exec->functions[i].initialized) return TURBOWASM_OK;
    const turbowasm_component_canon_lift *lift =
        &binary->canon_lifts[i];
    const turbowasm_component_exec_core_function *core_function;
    turbowasm_component_canonical_memory memory = {0};
    const turbowasm_component_canonical_memory *memory_option = NULL;

    if (lift->component_function_index >= exec->function_count ||
        lift->core_function_index >= exec->core_function_count ||
        lift->type_index >= binary->type_graph.count ||
        exec->function_adapter_indices[
            lift->component_function_index] != i) {
        status = TURBOWASM_MALFORMED_MODULE;
        return status;
    }

    core_function =
        &exec->core_functions[lift->core_function_index];
    if (core_function->kind !=
            TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE ||
        core_function->instance_index >=
            exec->core_instance_count ||
        binary->core_instances[
            core_function->instance_index].kind !=
            TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
        status = TURBOWASM_TYPE_MISMATCH;
        return status;
    }

    if (lift->has_realloc && !lift->has_memory) {
        status = TURBOWASM_MALFORMED_MODULE;
        return status;
    }

    if (lift->has_memory) {
        const turbowasm_component_exec_core_memory *core_memory;
        const turbowasm_module *memory_module;
        turbowasm_memory_desc memory_desc;

        if (lift->memory_index >= exec->core_memory_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            return status;
        }

        core_memory = &exec->core_memories[lift->memory_index];
        if (core_memory->instance_index >=
                exec->core_instance_count ||
            binary->core_instances[
                core_memory->instance_index].kind !=
                TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
            status = TURBOWASM_MALFORMED_MODULE;
            return status;
        }

        memory.instance =
            &exec->core_instances[core_memory->instance_index];
        memory.memory_index = core_memory->memory_index;
        memory.string_encoding = lift->string_encoding;

        memory_module =
            turbowasm_instance_module(memory.instance);
        if (memory_module == NULL ||
            !turbowasm_module_memory_at(
                memory_module,
                memory.memory_index,
                &memory_desc)) {
            status = TURBOWASM_MALFORMED_MODULE;
            return status;
        }

        memory.pointer_type = memory_desc.memory64
            ? TURBOWASM_COMPONENT_POINTER_I64
            : TURBOWASM_COMPONENT_POINTER_I32;

        if (lift->has_realloc) {
            const turbowasm_component_exec_core_function
                *realloc_function;
            turbowasm_component_exec_realloc_context *context;

            if (lift->realloc_function_index >=
                exec->core_function_count) {
                status = TURBOWASM_MALFORMED_MODULE;
                return status;
            }

            realloc_function =
                &exec->core_functions[
                    lift->realloc_function_index];
            if (realloc_function->kind !=
                    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE ||
                realloc_function->instance_index >=
                    exec->core_instance_count ||
                binary->core_instances[
                    realloc_function->instance_index].kind !=
                    TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE) {
                status = TURBOWASM_TYPE_MISMATCH;
                return status;
            }

            context = &exec->realloc_contexts[i];
            context->instance =
                &exec->core_instances[
                    realloc_function->instance_index];
            context->function_index =
                realloc_function->function_index;
            context->pointer_type = memory.pointer_type;
            context->may_leave = &exec->may_leave;

            if (!core_function_has_pointer_signature(
                    context->instance,
                    context->function_index,
                    context->pointer_type)) {
                status = TURBOWASM_TYPE_MISMATCH;
                return status;
            }

            memory.guest_realloc = component_guest_realloc;
            memory.realloc_context = context;
        }

        memory_option = &memory;
    }

    status =
        turbowasm_component_core_call_adapter_init_with_resources(
            &exec->functions[i],
            &binary->type_graph,
            lift->type_index,
            &exec->core_instances[
                core_function->instance_index],
            core_function->function_index,
            memory_option,
            &exec->resource_table);
    if (status != TURBOWASM_OK)
        return status;

    exec->function_adapter_indices[
        lift->component_function_index] = i;
    exec->functions[i].defines_local_resources = true;
    exec->functions[i].may_leave = &exec->may_leave;
    if (lift->has_post_return) {
        const turbowasm_component_exec_core_function *post;
        if (lift->post_return_function_index >= exec->core_function_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            return status;
        }
        post = &exec->core_functions[lift->post_return_function_index];
        if (post->kind == TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE &&
            post->instance_index < exec->core_instance_count) {
            status = turbowasm_component_core_call_set_post_return(
                &exec->functions[i], &exec->core_instances[post->instance_index],
                post->function_index);
        } else if (post->kind == TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_CANON_LOWER &&
                   post->canon_lower_index < binary->canon_lower_count) {
            status = turbowasm_component_core_call_set_canonical_post_return(
                &exec->functions[i],
                &exec->canon_lower_contexts[post->canon_lower_index].host_type);
        } else if (post->kind == TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN &&
                   post->resource_builtin_index < binary->resource_builtin_count) {
            turbowasm_host_function_type signature;
            if (!resource_builtin_host_type(
                    &exec->resource_builtin_contexts[post->resource_builtin_index], &signature)) {
                status = TURBOWASM_MALFORMED_MODULE;
                return status;
            }
            status = turbowasm_component_core_call_set_canonical_post_return(
                &exec->functions[i], &signature);
        } else {
            status = TURBOWASM_MALFORMED_MODULE;
        }
        if (status != TURBOWASM_OK)
            return status;
    }

    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_init_with_import_sets(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_component_exec_imports *import_sets,
    size_t import_set_count) {
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        exec->initialized || exec->binary != NULL ||
        binary->bytes == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if ((import_set_count != 0u && import_sets == NULL) ||
        import_set_count > UINT32_MAX ||
        import_set_count >
            SIZE_MAX / sizeof(*exec->import_sets))
        return TURBOWASM_INVALID_ARGUMENT;

    if (binary->import_count != 0u) {
        uint32_t import_index;

        if (import_set_count == 0u)
            return TURBOWASM_UNSUPPORTED;

        /*
         * W2b2 admits interface-instance imports only. Direct Component
         * function/value/type/component imports stay fail-closed.
         */
        for (import_index = 0u;
             import_index < binary->import_count;
             ++import_index) {
            if (binary->imports[import_index].kind !=
                TURBOWASM_COMPONENT_EXTERN_INSTANCE)
                return TURBOWASM_UNSUPPORTED;
        }
    }

    {
        size_t import_index;
        for (import_index = 0u;
             import_index < import_set_count;
             ++import_index) {
            if (import_sets[import_index].can_bind == NULL ||
                import_sets[import_index].invoke == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
        }
    }

    scope = turbowasm_runtime_scope_enter(&binary->config);
    exec->binary = binary;
    exec->may_leave = true;

    status = component_import_router_init(
        exec, import_sets, import_set_count);
    if (status != TURBOWASM_OK)
        goto fail;

    if (binary->core_module_count != 0u) {
        if ((size_t)binary->core_module_count >
            SIZE_MAX / sizeof(*exec->core_modules)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_modules =
            (turbowasm_module *)turbowasm_rt_calloc(
                binary->core_module_count,
                sizeof(*exec->core_modules));
        if (exec->core_modules == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    for (i = 0u; i < binary->core_module_count; ++i) {
        const turbowasm_component_core_module *record =
            &binary->core_modules[i];

        status = turbowasm_module_load_borrowed_with_config(
            &exec->core_modules[i],
            record->bytes,
            record->size,
            &binary->config);
        if (status != TURBOWASM_OK)
            goto fail;
        ++exec->core_module_count;
    }

    if (binary->core_instance_count != 0u) {
        if ((size_t)binary->core_instance_count >
            SIZE_MAX / sizeof(*exec->core_instances)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_instances =
            (turbowasm_instance *)turbowasm_rt_calloc(
                binary->core_instance_count,
                sizeof(*exec->core_instances));
        if (exec->core_instances == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    exec->core_function_count = binary->core_function_count;
    if (exec->core_function_count != 0u) {
        if ((size_t)exec->core_function_count >
            SIZE_MAX / sizeof(*exec->core_functions)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_functions =
            (turbowasm_component_exec_core_function *)
                turbowasm_rt_calloc(
                    exec->core_function_count,
                    sizeof(*exec->core_functions));
        if (exec->core_functions == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    if (binary->core_memory_alias_count != 0u) {
        if ((size_t)binary->core_memory_alias_count >
            SIZE_MAX / sizeof(*exec->core_memories)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_memories =
            (turbowasm_component_exec_core_memory *)
                turbowasm_rt_calloc(
                    binary->core_memory_alias_count,
                    sizeof(*exec->core_memories));
        if (exec->core_memories == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }
    exec->core_memory_count = binary->core_memory_alias_count;

    for (i = 0u; i < binary->core_memory_alias_count; ++i) {
        const turbowasm_component_core_memory_alias *alias =
            &binary->core_memory_aliases[i];
        const turbowasm_component_core_instance_def *provider;
        const turbowasm_export_desc *export_desc;
        const turbowasm_module *module;

        if (alias->core_memory_index >=
                exec->core_memory_count ||
            alias->instance_index >=
                binary->core_instance_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        provider =
            &binary->core_instances[alias->instance_index];
        if (provider->kind !=
            TURBOWASM_COMPONENT_CORE_INSTANCE_INSTANTIATE ||
            provider->module_index >= exec->core_module_count) {
            status = TURBOWASM_UNSUPPORTED;
            goto fail;
        }

        module = &exec->core_modules[provider->module_index];
        export_desc = find_core_memory_export(
            module, alias->name);
        if (export_desc == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        exec->core_memories[
            alias->core_memory_index].instance_index =
                alias->instance_index;
        exec->core_memories[
            alias->core_memory_index].memory_index =
                export_desc->item_index;
    }

    exec->adapter_count = binary->canon_lift_count;
    exec->function_count = binary->component_function_count;

    if (exec->adapter_count != 0u) {
        if ((size_t)exec->adapter_count >
                SIZE_MAX / sizeof(*exec->functions) ||
            (size_t)exec->adapter_count >
                SIZE_MAX / sizeof(*exec->realloc_contexts)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }

        exec->functions =
            (turbowasm_component_core_call_adapter *)
                turbowasm_rt_calloc(
                    exec->adapter_count,
                    sizeof(*exec->functions));
        exec->realloc_contexts =
            (turbowasm_component_exec_realloc_context *)
                turbowasm_rt_calloc(
                    exec->adapter_count,
                    sizeof(*exec->realloc_contexts));

        if (exec->functions == NULL ||
            exec->realloc_contexts == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    if (exec->function_count != 0u) {
        if ((size_t)exec->function_count >
            SIZE_MAX / sizeof(*exec->function_adapter_indices)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->function_adapter_indices =
            (uint32_t *)turbowasm_rt_malloc(
                (size_t)exec->function_count *
                sizeof(*exec->function_adapter_indices));
        if (exec->function_adapter_indices == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        for (i = 0u; i < exec->function_count; ++i)
            exec->function_adapter_indices[i] = UINT32_MAX;
    }

    for (i = 0u; i < binary->canon_lift_count; ++i) {
        uint32_t function = binary->canon_lifts[i].component_function_index;
        if (function >= exec->function_count || exec->function_adapter_indices[function] != UINT32_MAX) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }
        exec->function_adapter_indices[function] = i;
    }
    status = resolve_component_function_aliases(exec, binary);
    if (status != TURBOWASM_OK) goto fail;

    status = initialize_resource_state(exec, binary);
    if (status != TURBOWASM_OK)
        goto fail;

    status = initialize_canon_lower_state(exec, binary);
    if (status != TURBOWASM_OK)
        goto fail;

    /*
     * Process Core instances in index order. Inline instances are provider
     * namespaces only; ordinary instances are created through the existing
     * Runtime/linker. Resource builtins are already present in the Core
     * function map before any consumer start function can call them.
     */
    for (i = 0u; i < binary->core_instance_count; ++i) {
        status = instantiate_core_instance(exec, binary, i);
        if (status != TURBOWASM_OK)
            goto fail;

        ++exec->core_instance_count;

        status = resolve_core_aliases_for_provider(
            exec, binary, i);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    for (i = 0u; i < binary->core_function_alias_count; ++i) {
        const turbowasm_component_core_function_alias *alias =
            &binary->core_function_aliases[i];

        if (alias->core_function_index >=
                exec->core_function_count ||
            exec->core_functions[
                alias->core_function_index].kind ==
                TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }
    }

    for (i = 0u; i < binary->canon_lift_count; ++i) {
        status = initialize_lift_adapter(exec, binary, i);
        if (status != TURBOWASM_OK) goto fail;
    }

    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &binary->exports[i];
        uint32_t adapter_index;

        if (export_desc->kind == TURBOWASM_COMPONENT_EXTERN_TYPE)
            continue;

        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->item_index >= exec->function_count) {
            status = TURBOWASM_UNSUPPORTED;
            goto fail;
        }

        adapter_index =
            exec->function_adapter_indices[
                export_desc->item_index];
        if (adapter_index == UINT32_MAX ||
            adapter_index >= exec->adapter_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        if (export_desc->has_ascribed_type) {
            const turbowasm_component_canon_lift *lift =
                &binary->canon_lifts[adapter_index];

            if (export_desc->type_index != lift->type_index) {
                status = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }
        }
    }

    exec->initialized = true;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

fail:
    destroy_partial(exec);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

turbowasm_status turbowasm_component_exec_init_with_imports(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_component_exec_imports *imports) {
    return turbowasm_component_exec_init_with_import_sets(
        exec,
        binary,
        imports,
        imports != NULL ? 1u : 0u);
}

turbowasm_status turbowasm_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary) {
    return turbowasm_component_exec_init_with_import_sets(
        exec, binary, NULL, 0u);
}

void turbowasm_component_exec_destroy(
    turbowasm_component_exec *exec) {
    if (exec == NULL)
        return;
    destroy_partial(exec);
}

turbowasm_status turbowasm_component_exec_invoke_export(
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    uint32_t i;

    if (exec == NULL || !exec->initialized ||
        exec->binary == NULL ||
        (name_size != 0u && name == NULL) ||
        trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &exec->binary->exports[i];

        if (export_desc->kind ==
                TURBOWASM_COMPONENT_EXTERN_FUNCTION &&
            component_name_equal(
                export_desc->name, name, name_size)) {
            uint32_t adapter_index;
            if (export_desc->item_index >= exec->function_count)
                return TURBOWASM_MALFORMED_MODULE;
            adapter_index =
                exec->function_adapter_indices[
                    export_desc->item_index];
            if (adapter_index == UINT32_MAX ||
                adapter_index >= exec->adapter_count)
                return TURBOWASM_MALFORMED_MODULE;
            return turbowasm_component_core_call_invoke(
                &exec->functions[adapter_index],
                arguments,
                argument_count,
                out_result,
                trap);
        }
    }

    return TURBOWASM_INVALID_ARGUMENT;
}


turbowasm_status turbowasm_component_exec_call_create(
    turbowasm_component_exec_call *call,
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count) {
    uint32_t i;

    if (call == NULL || call->initialized ||
        exec == NULL || !exec->initialized ||
        exec->binary == NULL ||
        (name_size != 0u && name == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &exec->binary->exports[i];

        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            !component_name_equal(
                export_desc->name, name, name_size))
            continue;

        {
            uint32_t adapter_index;
            turbowasm_status status;

            if (export_desc->item_index >= exec->function_count)
                return TURBOWASM_MALFORMED_MODULE;
            adapter_index =
                exec->function_adapter_indices[
                    export_desc->item_index];
            if (adapter_index == UINT32_MAX ||
                adapter_index >= exec->adapter_count)
                return TURBOWASM_MALFORMED_MODULE;

            status = turbowasm_component_core_execution_create(
                &call->core,
                &exec->functions[adapter_index],
                arguments,
                argument_count);
            if (status != TURBOWASM_OK)
                return status;

            call->initialized = true;
            return TURBOWASM_OK;
        }
    }

    return TURBOWASM_INVALID_ARGUMENT;
}

void turbowasm_component_exec_call_destroy(
    turbowasm_component_exec_call *call) {
    if (call == NULL)
        return;
    if (call->initialized)
        turbowasm_component_core_execution_destroy(
            &call->core);
    memset(call, 0, sizeof(*call));
}

turbowasm_status turbowasm_component_exec_call_resume(
    turbowasm_component_exec_call *call,
    const turbowasm_execution_options *options) {
    if (call == NULL || !call->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_core_execution_resume(
        &call->core, options);
}

turbowasm_execution_state turbowasm_component_exec_call_state_get(
    const turbowasm_component_exec_call *call) {
    return call != NULL && call->initialized
        ? turbowasm_component_core_execution_state_get(
              &call->core)
        : TURBOWASM_EXECUTION_FAILED;
}

turbowasm_yield_reason turbowasm_component_exec_call_yield_reason_get(
    const turbowasm_component_exec_call *call) {
    return call != NULL && call->initialized
        ? turbowasm_component_core_execution_yield_reason_get(
              &call->core)
        : TURBOWASM_YIELD_NONE;
}

bool turbowasm_component_exec_call_pending_host_wait(
    const turbowasm_component_exec_call *call,
    turbowasm_host_wait *out_wait) {
    return call != NULL && call->initialized &&
        turbowasm_component_core_execution_pending_host_wait(
            &call->core, out_wait);
}

turbowasm_status turbowasm_component_exec_call_complete_host_wait(
    turbowasm_component_exec_call *call,
    turbowasm_host_wait wait,
    int status) {
    if (call == NULL || !call->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_core_execution_complete_host_wait(
        &call->core, wait, status);
}

turbowasm_status turbowasm_component_exec_call_terminal_status(
    const turbowasm_component_exec_call *call) {
    return call != NULL && call->initialized
        ? turbowasm_component_core_execution_terminal_status(
              &call->core)
        : TURBOWASM_INVALID_ARGUMENT;
}

turbowasm_trap turbowasm_component_exec_call_trap(
    const turbowasm_component_exec_call *call) {
    return call != NULL && call->initialized
        ? turbowasm_component_core_execution_trap(
              &call->core)
        : TURBOWASM_TRAP_NONE;
}

size_t turbowasm_component_exec_call_result_count(
    const turbowasm_component_exec_call *call) {
    return call != NULL && call->initialized
        ? turbowasm_component_core_execution_result_count(
              &call->core)
        : 0u;
}

turbowasm_status turbowasm_component_exec_call_take_result(
    turbowasm_component_exec_call *call,
    turbowasm_component_value *out_result) {
    if (call == NULL || !call->initialized)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_core_execution_take_result(
        &call->core, out_result);
}
