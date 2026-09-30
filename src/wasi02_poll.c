#include "wasi02_poll.h"

#include "wasi02_component.h"
#include "wasi02_descriptor.h"

#include <string.h>

#define TURBOWASM_WASI02_POLLABLE_ID UINT64_C(0x776173693032706f)

static bool component_name_is(
    turbowasm_component_name name,
    const char *text) {
    size_t size;

    if (text == NULL || (name.size != 0u && name.bytes == NULL))
        return false;
    size = strlen(text);
    return size == name.size &&
           (size == 0u ||
            memcmp(name.bytes, text, size) == 0);
}

static turbowasm_status pollable_rep_get(
    const turbowasm_wasi02_poll *poll,
    uint32_t resource,
    turbowasm_value *out_rep) {
    if (poll == NULL || !poll->initialized || out_rep == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_resource_rep(
        &poll->resources,
        resource,
        TURBOWASM_WASI02_POLLABLE_ID,
        out_rep);
}

turbowasm_status turbowasm_wasi02_poll_init(
    turbowasm_wasi02_poll *poll,
    const turbowasm_wasi02_poll_provider *provider,
    uint32_t max_pollables) {
    if (poll == NULL || provider == NULL ||
        provider->ready == NULL ||
        max_pollables == 0u ||
        poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(poll, 0, sizeof(*poll));
    if (!turbowasm_component_resource_table_init(
            &poll->resources, max_pollables))
        return TURBOWASM_OUT_OF_MEMORY;

    poll->provider = *provider;
    poll->initialized = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_poll_destroy(
    turbowasm_wasi02_poll *poll) {
    if (poll == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!poll->initialized)
        return TURBOWASM_OK;
    if (poll->resources.live_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_component_resource_table_destroy(
        &poll->resources);
    memset(poll, 0, sizeof(*poll));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_pollable_new(
    turbowasm_wasi02_poll *poll,
    turbowasm_value provider_rep,
    uint32_t *out_resource) {
    if (poll == NULL || !poll->initialized ||
        out_resource == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_component_resource_new_owned(
        &poll->resources,
        TURBOWASM_WASI02_POLLABLE_ID,
        provider_rep,
        out_resource);
}

turbowasm_status turbowasm_wasi02_pollable_drop(
    turbowasm_wasi02_poll *poll,
    uint32_t resource) {
    turbowasm_value rep = {0};
    turbowasm_status status;

    if (poll == NULL || !poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_rep_get(poll, resource, &rep);
    if (status != TURBOWASM_OK)
        return status;

    if (poll->provider.drop != NULL) {
        status = poll->provider.drop(
            poll->provider.context, rep);
        if (status != TURBOWASM_OK)
            return status;
    }

    return turbowasm_component_resource_drop(
        &poll->resources,
        resource,
        TURBOWASM_WASI02_POLLABLE_ID,
        NULL,
        NULL);
}

turbowasm_status turbowasm_wasi02_pollable_ready(
    turbowasm_wasi02_poll *poll,
    uint32_t resource,
    bool *out_ready) {
    turbowasm_value rep = {0};
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_rep_get(poll, resource, &rep);
    if (status != TURBOWASM_OK)
        return status;

    return poll->provider.ready(
        poll->provider.context,
        rep,
        out_ready);
}

turbowasm_status turbowasm_wasi02_pollable_block(
    turbowasm_wasi02_poll *poll,
    uint32_t resource,
    turbowasm_host_call *call) {
    turbowasm_value rep = {0};
    turbowasm_status status;
    bool ready = false;
    uintptr_t operation_token = 0u;
    turbowasm_host_wait wait = {0};
    int completion_status = 0;

    if (poll == NULL || !poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_rep_get(poll, resource, &rep);
    if (status != TURBOWASM_OK)
        return status;

    status = poll->provider.ready(
        poll->provider.context,
        rep,
        &ready);
    if (status != TURBOWASM_OK || ready)
        return status;

    if (call == NULL ||
        !turbowasm_host_call_can_wait(call) ||
        poll->provider.arm == NULL)
        return TURBOWASM_UNSUPPORTED;

    /*
     * arm promises that completion is published only when this pollable is
     * ready (including terminal/error readiness). Runtime owns suspension and
     * generation-checked wakeup; the provider token remains opaque.
     */
    status = poll->provider.arm(
        poll->provider.context,
        rep,
        &operation_token);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_host_call_wait(
        call,
        operation_token,
        &wait,
        &completion_status);
    if (status != TURBOWASM_OK)
        return status;

    (void)wait;
    (void)completion_status;
    return TURBOWASM_OK;
}

static bool type_ref_is_kind(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind expected) {
    const turbowasm_component_type *type;

    if (ref.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE)
        return ref.as.inline_type == expected;
    if (ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED ||
        graph == NULL)
        return false;

    type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    return type != NULL && type->kind == expected;
}

static bool bind_pollable_identity(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind expected_handle_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;
    uint64_t identity;

    if (poll == NULL || graph == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (handle_type == NULL ||
        handle_type->kind != expected_handle_kind)
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity == 0u)
        return false;

    identity = resource_type->as.resource.identity;
    if (poll->pollable_identity_bound &&
        poll->pollable_identity != identity)
        return false;

    poll->pollable_identity = identity;
    poll->pollable_identity_bound = true;
    return true;
}

static const turbowasm_wasi02_function_desc *
poll_function_by_component_name(
    turbowasm_component_name name) {
    const turbowasm_wasi02_interface_desc *iface =
        turbowasm_wasi02_find_interface("wasi:io", "poll");
    uint32_t i;

    if (iface == NULL ||
        (name.size != 0u && name.bytes == NULL))
        return NULL;

    for (i = 0u; i < iface->function_count; ++i) {
        const turbowasm_wasi02_function_desc *function =
            &iface->functions[i];
        size_t size = strlen(function->name);

        if (size == name.size &&
            (size == 0u ||
             memcmp(name.bytes, function->name, size) == 0))
            return function;
    }
    return NULL;
}

static bool bind_poll_method_shape(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    const turbowasm_wasi02_function_desc *function) {
    const turbowasm_component_type *function_type;

    if (poll == NULL || graph == NULL ||
        function == NULL ||
        function_type_index >= graph->count)
        return false;

    if (strcmp(function->name, "poll") == 0)
        return false; /* W4b2b wait-any slice. */

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count != 1u ||
        !bind_pollable_identity(
            poll,
            graph,
            function_type->as.function.params[0],
            TURBOWASM_COMPONENT_TYPE_BORROW))
        return false;

    if (strcmp(
            function->name,
            "[method]pollable.ready") == 0) {
        return function_type->as.function.has_result &&
               type_ref_is_kind(
                   graph,
                   function_type->as.function.result,
                   TURBOWASM_COMPONENT_TYPE_BOOL);
    }

    if (strcmp(
            function->name,
            "[method]pollable.block") == 0)
        return !function_type->as.function.has_result;

    return false;
}

static bool wasi02_poll_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    turbowasm_wasi02_poll *poll =
        (turbowasm_wasi02_poll *)context;
    const turbowasm_wasi02_function_desc *function;

    if (poll == NULL || !poll->initialized ||
        !component_name_is(
            instance_name,
            "wasi:io/poll@0.2.8"))
        return false;

    function = poll_function_by_component_name(
        function_name);
    return bind_poll_method_shape(
        poll, graph, function_type, function);
}

static bool imported_pollable_identity(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    turbowasm_component_type_kind *out_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;

    if (poll == NULL ||
        !poll->pollable_identity_bound ||
        graph == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
    if (handle_type == NULL ||
        (handle_type->kind != TURBOWASM_COMPONENT_TYPE_OWN &&
         handle_type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE ||
        resource_type->as.resource.identity !=
            poll->pollable_identity)
        return false;

    if (out_kind != NULL)
        *out_kind = handle_type->kind;
    return true;
}

static turbowasm_status wasi02_poll_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    turbowasm_wasi02_poll *poll =
        (turbowasm_wasi02_poll *)context;
    turbowasm_component_type_kind kind;
    turbowasm_value rep = {0};
    uint32_t handle;
    turbowasm_status status;

    if (value == NULL || out_handle == NULL ||
        !imported_pollable_identity(
            poll, graph, type, &kind) ||
        value->kind != kind ||
        value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle = (uint32_t)value->as.resource_rep.as.i32;
    status = pollable_rep_get(
        poll, handle, &rep);
    if (status != TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    *out_handle = handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_poll_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    turbowasm_wasi02_poll *poll =
        (turbowasm_wasi02_poll *)context;
    turbowasm_component_type_kind kind;
    turbowasm_value rep = {0};

    if (out == NULL ||
        !imported_pollable_identity(
            poll, graph, type, &kind) ||
        pollable_rep_get(poll, handle, &rep) !=
            TURBOWASM_OK)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->as.resource_rep.kind = TURBOWASM_VALUE_I32;
    out->as.resource_rep.as.i32 = (int32_t)handle;
    return TURBOWASM_OK;
}

static turbowasm_status wasi02_poll_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    turbowasm_wasi02_poll *poll =
        (turbowasm_wasi02_poll *)context;

    if (poll == NULL || !poll->initialized ||
        !poll->pollable_identity_bound ||
        resource_identity != poll->pollable_identity)
        return TURBOWASM_TYPE_MISMATCH;

    return turbowasm_wasi02_pollable_drop(
        poll, handle);
}

static turbowasm_status wasi02_poll_invoke(
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
    turbowasm_wasi02_poll *poll =
        (turbowasm_wasi02_poll *)context;
    uint32_t resource;
    bool ready = false;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        trap == NULL ||
        argument_count != 1u ||
        arguments == NULL ||
        !wasi02_poll_can_bind(
            context, instance_name, function_name,
            graph, function_type) ||
        arguments[0].kind != TURBOWASM_COMPONENT_TYPE_BORROW ||
        arguments[0].as.resource_rep.kind !=
            TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;
    resource =
        (uint32_t)arguments[0].as.resource_rep.as.i32;

    if (component_name_is(
            function_name,
            "[method]pollable.ready")) {
        if (out_result == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        status = turbowasm_wasi02_pollable_ready(
            poll, resource, &ready);
        if (status != TURBOWASM_OK)
            return status;

        memset(out_result, 0, sizeof(*out_result));
        out_result->kind = TURBOWASM_COMPONENT_TYPE_BOOL;
        out_result->as.boolean = ready;
        return TURBOWASM_OK;
    }

    if (component_name_is(
            function_name,
            "[method]pollable.block"))
        return turbowasm_wasi02_pollable_block(
            poll, resource, call);

    return TURBOWASM_UNSUPPORTED;
}

turbowasm_status turbowasm_wasi02_poll_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_poll *poll) {
    turbowasm_component_exec_imports imports;

    if (exec == NULL || binary == NULL ||
        poll == NULL || !poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(&imports, 0, sizeof(imports));
    imports.context = poll;
    imports.can_bind = wasi02_poll_can_bind;
    imports.invoke = wasi02_poll_invoke;
    imports.resource_lower = wasi02_poll_resource_lower;
    imports.resource_lift = wasi02_poll_resource_lift;
    imports.resource_drop = wasi02_poll_resource_drop;

    return turbowasm_component_exec_init_with_imports(
        exec, binary, &imports);
}
