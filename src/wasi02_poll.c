#include "wasi02_poll.h"

#include "wasi02_component.h"
#include "wasi02_descriptor.h"
#include "runtime_alloc.h"

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
    turbowasm_host_wait *wait_storage = &wait;
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
        (poll->provider.arm == NULL &&
         poll->provider.arm_routed == NULL))
        return TURBOWASM_UNSUPPORTED;

    /*
     * A routed provider may retain the exact Runtime wait storage so an
     * external completion source can complete it with generation checking.
     * Plain providers continue to use local storage.
     */
    if (poll->provider.arm_routed != NULL) {
        status = poll->provider.arm_routed(
            poll->provider.context,
            rep,
            call,
            &operation_token,
            &wait_storage);
        if (status != TURBOWASM_OK)
            return status;
        if (wait_storage == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
    } else {
        status = poll->provider.arm(
            poll->provider.context,
            rep,
            &operation_token);
        if (status != TURBOWASM_OK)
            return status;
    }

    status = turbowasm_host_call_wait(
        call,
        operation_token,
        wait_storage,
        &completion_status);
    if (status != TURBOWASM_OK)
        return status;

    (void)wait;
    (void)completion_status;
    return TURBOWASM_OK;
}

static turbowasm_status poll_many_scan(
    turbowasm_wasi02_poll *poll,
    const uint32_t *resources,
    size_t resource_count,
    turbowasm_value *reps,
    bool *ready_flags,
    size_t *out_ready_count) {
    size_t i;
    size_t ready_count = 0u;

    if (poll == NULL || resources == NULL ||
        reps == NULL || ready_flags == NULL ||
        out_ready_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < resource_count; ++i) {
        turbowasm_status status;
        bool ready = false;

        status = pollable_rep_get(
            poll, resources[i], &reps[i]);
        if (status != TURBOWASM_OK)
            return TURBOWASM_TRAPPED;

        status = poll->provider.ready(
            poll->provider.context,
            reps[i],
            &ready);
        if (status != TURBOWASM_OK)
            return status;

        ready_flags[i] = ready;
        if (ready)
            ++ready_count;
    }

    *out_ready_count = ready_count;
    return TURBOWASM_OK;
}

static turbowasm_status poll_many_build_result(
    const bool *ready_flags,
    size_t resource_count,
    size_t ready_count,
    turbowasm_component_value *out_result) {
    turbowasm_component_value *items = NULL;
    size_t i;
    size_t cursor = 0u;

    if (ready_flags == NULL || out_result == NULL ||
        ready_count == 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (ready_count > SIZE_MAX / sizeof(*items))
        return TURBOWASM_OUT_OF_MEMORY;

    items = (turbowasm_component_value *)turbowasm_rt_calloc(
        ready_count, sizeof(*items));
    if (items == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < resource_count; ++i) {
        if (!ready_flags[i])
            continue;
        items[cursor].kind = TURBOWASM_COMPONENT_TYPE_U32;
        items[cursor].as.u32 = (uint32_t)i;
        ++cursor;
    }

    if (cursor != ready_count) {
        turbowasm_rt_free(items);
        return TURBOWASM_TRAPPED;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = TURBOWASM_COMPONENT_TYPE_LIST;
    out_result->as.list.items = items;
    out_result->as.list.count = ready_count;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_poll_many(
    turbowasm_wasi02_poll *poll,
    const uint32_t *resources,
    size_t resource_count,
    turbowasm_host_call *call,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    turbowasm_value *reps = NULL;
    bool *ready_flags = NULL;
    size_t ready_count = 0u;
    uintptr_t operation_token = 0u;
    turbowasm_host_wait wait = {0};
    turbowasm_host_wait *wait_storage = &wait;
    int completion_status = 0;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        out_result == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    memset(out_result, 0, sizeof(*out_result));

    if (resource_count == 0u ||
        resource_count > UINT32_MAX ||
        resources == NULL) {
        *trap = TURBOWASM_TRAP_UNREACHABLE;
        return TURBOWASM_TRAPPED;
    }

    if (resource_count > SIZE_MAX / sizeof(*reps) ||
        resource_count > SIZE_MAX / sizeof(*ready_flags))
        return TURBOWASM_OUT_OF_MEMORY;

    reps = (turbowasm_value *)turbowasm_rt_calloc(
        resource_count, sizeof(*reps));
    ready_flags = (bool *)turbowasm_rt_calloc(
        resource_count, sizeof(*ready_flags));
    if (reps == NULL || ready_flags == NULL) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    status = poll_many_scan(
        poll, resources, resource_count,
        reps, ready_flags, &ready_count);
    if (status != TURBOWASM_OK)
        goto done;

    if (ready_count != 0u) {
        status = poll_many_build_result(
            ready_flags, resource_count,
            ready_count, out_result);
        goto done;
    }

    if (call == NULL ||
        !turbowasm_host_call_can_wait(call) ||
        (poll->provider.arm_many == NULL &&
         poll->provider.arm_many_routed == NULL)) {
        status = TURBOWASM_UNSUPPORTED;
        goto done;
    }

    if (poll->provider.arm_many_routed != NULL) {
        status = poll->provider.arm_many_routed(
            poll->provider.context,
            reps,
            resource_count,
            call,
            &operation_token,
            &wait_storage);
        if (status != TURBOWASM_OK)
            goto done;
        if (wait_storage == NULL) {
            status = TURBOWASM_INVALID_ARGUMENT;
            goto done;
        }
    } else {
        status = poll->provider.arm_many(
            poll->provider.context,
            reps,
            resource_count,
            &operation_token);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_host_call_wait(
        call,
        operation_token,
        wait_storage,
        &completion_status);
    if (status != TURBOWASM_OK)
        goto done;

    (void)wait;
    (void)completion_status;

    memset(ready_flags, 0, resource_count * sizeof(*ready_flags));
    status = poll_many_scan(
        poll, resources, resource_count,
        reps, ready_flags, &ready_count);
    if (status != TURBOWASM_OK)
        goto done;

    /*
     * Provider arm_many promised wakeup only when at least one source becomes
     * ready. Treat a spurious wake as an adapter contract violation.
     */
    if (ready_count == 0u) {
        status = TURBOWASM_TRAPPED;
        goto done;
    }

    status = poll_many_build_result(
        ready_flags, resource_count,
        ready_count, out_result);

done:
    turbowasm_rt_free(ready_flags);
    turbowasm_rt_free(reps);
    if (status != TURBOWASM_OK)
        turbowasm_component_value_destroy(out_result);
    return status;
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

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION ||
        function_type->as.function.param_count != 1u)
        return false;

    if (strcmp(function->name, "poll") == 0) {
        const turbowasm_component_type *input_list;
        const turbowasm_component_type *output_list;

        if (!function_type->as.function.has_result ||
            function_type->as.function.params[0].kind !=
                TURBOWASM_COMPONENT_TYPE_REF_INDEXED ||
            function_type->as.function.result.kind !=
                TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
            return false;

        input_list = turbowasm_component_type_graph_get(
            graph,
            function_type->as.function.params[0].as.indexed);
        output_list = turbowasm_component_type_graph_get(
            graph,
            function_type->as.function.result.as.indexed);
        if (input_list == NULL ||
            input_list->kind != TURBOWASM_COMPONENT_TYPE_LIST ||
            output_list == NULL ||
            output_list->kind != TURBOWASM_COMPONENT_TYPE_LIST)
            return false;

        return bind_pollable_identity(
                   poll,
                   graph,
                   input_list->as.list.element_type,
                   TURBOWASM_COMPONENT_TYPE_BORROW) &&
               type_ref_is_kind(
                   graph,
                   output_list->as.list.element_type,
                   TURBOWASM_COMPONENT_TYPE_U32);
    }

    if (!bind_pollable_identity(
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

bool turbowasm_wasi02_poll_import_can_bind(
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

turbowasm_status turbowasm_wasi02_poll_import_resource_lower(
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

turbowasm_status turbowasm_wasi02_poll_import_resource_lift(
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

turbowasm_status turbowasm_wasi02_poll_import_resource_drop(
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

turbowasm_status turbowasm_wasi02_poll_import_invoke(
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
        !turbowasm_wasi02_poll_import_can_bind(
            context, instance_name, function_name,
            graph, function_type))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;

    if (component_name_is(
            function_name,
            "poll")) {
        uint32_t *resources = NULL;
        uint64_t i;
        uint64_t count;

        if (out_result == NULL ||
            arguments[0].kind != TURBOWASM_COMPONENT_TYPE_LIST)
            return TURBOWASM_TYPE_MISMATCH;

        count = arguments[0].as.list.count;
        if (count > SIZE_MAX / sizeof(*resources))
            return TURBOWASM_OUT_OF_MEMORY;
        if (count != 0u) {
            resources = (uint32_t *)turbowasm_rt_calloc(
                (size_t)count, sizeof(*resources));
            if (resources == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }

        for (i = 0u; i < count; ++i) {
            const turbowasm_component_value *item =
                &arguments[0].as.list.items[i];
            if (item->kind != TURBOWASM_COMPONENT_TYPE_BORROW ||
                item->as.resource_rep.kind != TURBOWASM_VALUE_I32) {
                turbowasm_rt_free(resources);
                return TURBOWASM_TYPE_MISMATCH;
            }
            resources[i] =
                (uint32_t)item->as.resource_rep.as.i32;
        }

        status = turbowasm_wasi02_poll_many(
            poll,
            resources,
            (size_t)count,
            call,
            out_result,
            trap);
        turbowasm_rt_free(resources);
        return status;
    }

    if (arguments[0].kind != TURBOWASM_COMPONENT_TYPE_BORROW ||
        arguments[0].as.resource_rep.kind !=
            TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

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
    imports.can_bind = turbowasm_wasi02_poll_import_can_bind;
    imports.invoke = turbowasm_wasi02_poll_import_invoke;
    imports.resource_lower = turbowasm_wasi02_poll_import_resource_lower;
    imports.resource_lift = turbowasm_wasi02_poll_import_resource_lift;
    imports.resource_drop = turbowasm_wasi02_poll_import_resource_drop;

    return turbowasm_component_exec_init_with_imports(
        exec, binary, &imports);
}
