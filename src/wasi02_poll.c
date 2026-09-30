#include "wasi02_poll.h"

#include "runtime_alloc.h"

#include <string.h>

#define TURBOWASM_WASI02_POLLABLE_REP_ID \
    UINT64_C(0x776173693032706f)

static bool component_name_is(
    turbowasm_component_name name,
    const char *expected) {
    size_t size;

    if (expected == NULL)
        return false;
    size = strlen(expected);
    return size == name.size &&
           (size == 0u ||
            (name.bytes != NULL &&
             memcmp(name.bytes, expected, size) == 0));
}

static turbowasm_status pollable_token(
    const turbowasm_wasi02_poll *poll,
    uint32_t handle,
    uint64_t *out_token) {
    turbowasm_value rep = {0};
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        out_token == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_resource_rep(
        &poll->resources,
        handle,
        TURBOWASM_WASI02_POLLABLE_REP_ID,
        &rep);
    if (status != TURBOWASM_OK)
        return status;
    if (rep.kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_TRAPPED;

    *out_token = (uint64_t)rep.as.i64;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_poll_init(
    turbowasm_wasi02_poll *poll,
    const turbowasm_wasi02_poll_config *config,
    uint32_t max_pollables) {
    if (poll == NULL || config == NULL ||
        poll->initialized ||
        config->ready == NULL ||
        config->block == NULL ||
        config->poll_many == NULL ||
        config->drop == NULL ||
        max_pollables == 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(poll, 0, sizeof(*poll));
    if (!turbowasm_component_resource_table_init(
            &poll->resources, max_pollables))
        return TURBOWASM_OUT_OF_MEMORY;

    poll->config = *config;
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
    uint64_t token,
    uint32_t *out_handle) {
    turbowasm_value rep = {0};

    if (poll == NULL || !poll->initialized ||
        out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = (int64_t)token;
    return turbowasm_component_resource_new_owned(
        &poll->resources,
        TURBOWASM_WASI02_POLLABLE_REP_ID,
        rep,
        out_handle);
}

turbowasm_status turbowasm_wasi02_pollable_drop(
    turbowasm_wasi02_poll *poll,
    uint32_t handle) {
    uint64_t token;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_token(poll, handle, &token);
    if (status != TURBOWASM_OK)
        return status;

    status = poll->config.drop(
        poll->config.context, token);
    if (status != TURBOWASM_OK)
        return status;

    return turbowasm_component_resource_drop(
        &poll->resources,
        handle,
        TURBOWASM_WASI02_POLLABLE_REP_ID,
        NULL,
        NULL);
}

turbowasm_status turbowasm_wasi02_pollable_ready(
    turbowasm_wasi02_poll *poll,
    uint32_t handle,
    bool *out_ready) {
    uint64_t token;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_token(poll, handle, &token);
    if (status != TURBOWASM_OK)
        return status;
    return poll->config.ready(
        poll->config.context, token, out_ready);
}

turbowasm_status turbowasm_wasi02_pollable_block(
    turbowasm_wasi02_poll *poll,
    turbowasm_host_call *call,
    uint32_t handle) {
    uint64_t token;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        call == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pollable_token(poll, handle, &token);
    if (status != TURBOWASM_OK)
        return status;
    return poll->config.block(
        poll->config.context, call, token);
}

turbowasm_status turbowasm_wasi02_poll_many(
    turbowasm_wasi02_poll *poll,
    turbowasm_host_call *call,
    const uint32_t *handles,
    size_t handle_count,
    uint32_t *out_indices,
    size_t result_capacity,
    size_t *out_count) {
    uint64_t *tokens = NULL;
    size_t i;
    turbowasm_status status;

    if (poll == NULL || !poll->initialized ||
        call == NULL ||
        handles == NULL ||
        handle_count == 0u ||
        out_indices == NULL ||
        result_capacity == 0u ||
        out_count == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (handle_count > poll->resources.max_entries ||
        handle_count > SIZE_MAX / sizeof(*tokens))
        return TURBOWASM_INVALID_ARGUMENT;

    tokens = (uint64_t *)turbowasm_rt_calloc(
        handle_count, sizeof(*tokens));
    if (tokens == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < handle_count; ++i) {
        status = pollable_token(
            poll, handles[i], &tokens[i]);
        if (status != TURBOWASM_OK) {
            turbowasm_rt_free(tokens);
            return status;
        }
    }

    *out_count = 0u;
    status = poll->config.poll_many(
        poll->config.context,
        call,
        tokens,
        handle_count,
        out_indices,
        result_capacity,
        out_count);
    turbowasm_rt_free(tokens);

    if (status != TURBOWASM_OK)
        return status;
    if (*out_count == 0u ||
        *out_count > result_capacity)
        return TURBOWASM_TRAPPED;
    for (i = 0u; i < *out_count; ++i) {
        if ((size_t)out_indices[i] >= handle_count)
            return TURBOWASM_TRAPPED;
    }
    return TURBOWASM_OK;
}

static const turbowasm_component_type *
type_from_ref(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    if (graph == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return NULL;
    return turbowasm_component_type_graph_get(
        graph, ref.as.indexed);
}

static bool bind_pollable_identity(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_type *handle_type,
    turbowasm_component_type_kind expected_kind) {
    const turbowasm_component_type *resource_type;
    uint64_t identity;

    if (poll == NULL || graph == NULL ||
        handle_type == NULL ||
        handle_type->kind != expected_kind)
        return false;

    resource_type = turbowasm_component_type_graph_get(
        graph, handle_type->as.handle.resource_type);
    if (resource_type == NULL ||
        resource_type->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE)
        return false;

    identity = resource_type->as.resource.identity;
    if (identity == 0u)
        return false;
    if (poll->pollable_identity_bound &&
        poll->pollable_identity != identity)
        return false;

    poll->pollable_identity = identity;
    poll->pollable_identity_bound = true;
    return true;
}

static bool bind_poll_method(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type_index,
    turbowasm_component_name function_name) {
    const turbowasm_component_type *function_type;
    const turbowasm_component_type *self_type;

    function_type = turbowasm_component_type_graph_get(
        graph, function_type_index);
    if (function_type == NULL ||
        function_type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
        return false;

    if (component_name_is(
            function_name,
            "[method]pollable.ready")) {
        const turbowasm_component_type *result_type = NULL;

        if (function_type->as.function.param_count != 1u ||
            !function_type->as.function.has_result)
            return false;
        self_type = type_from_ref(
            graph, function_type->as.function.params[0]);
        if (!bind_pollable_identity(
                poll, graph, self_type,
                TURBOWASM_COMPONENT_TYPE_BORROW))
            return false;

        if (function_type->as.function.result.kind ==
            TURBOWASM_COMPONENT_TYPE_REF_INLINE)
            return function_type->as.function.result.as.inline_type ==
                   TURBOWASM_COMPONENT_TYPE_BOOL;

        result_type = type_from_ref(
            graph, function_type->as.function.result);
        return result_type != NULL &&
               result_type->kind == TURBOWASM_COMPONENT_TYPE_BOOL;
    }

    if (component_name_is(
            function_name,
            "[method]pollable.block")) {
        if (function_type->as.function.param_count != 1u ||
            function_type->as.function.has_result)
            return false;
        self_type = type_from_ref(
            graph, function_type->as.function.params[0]);
        return bind_pollable_identity(
            poll, graph, self_type,
            TURBOWASM_COMPONENT_TYPE_BORROW);
    }

    if (component_name_is(function_name, "poll")) {
        const turbowasm_component_type *input_list;
        const turbowasm_component_type *borrow_type;
        const turbowasm_component_type *output_list;
        turbowasm_component_type_ref out_element;

        if (function_type->as.function.param_count != 1u ||
            !function_type->as.function.has_result)
            return false;

        input_list = type_from_ref(
            graph, function_type->as.function.params[0]);
        if (input_list == NULL ||
            input_list->kind != TURBOWASM_COMPONENT_TYPE_LIST)
            return false;
        borrow_type = type_from_ref(
            graph, input_list->as.list.element_type);
        if (!bind_pollable_identity(
                poll, graph, borrow_type,
                TURBOWASM_COMPONENT_TYPE_BORROW))
            return false;

        output_list = type_from_ref(
            graph, function_type->as.function.result);
        if (output_list == NULL ||
            output_list->kind != TURBOWASM_COMPONENT_TYPE_LIST)
            return false;
        out_element = output_list->as.list.element_type;
        if (out_element.kind ==
            TURBOWASM_COMPONENT_TYPE_REF_INLINE)
            return out_element.as.inline_type ==
                   TURBOWASM_COMPONENT_TYPE_U32;

        {
            const turbowasm_component_type *element =
                type_from_ref(graph, out_element);
            return element != NULL &&
                   element->kind == TURBOWASM_COMPONENT_TYPE_U32;
        }
    }

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

    if (poll == NULL || !poll->initialized ||
        !component_name_is(
            instance_name, "wasi:io/poll@0.2.8"))
        return false;
    return bind_poll_method(
        poll, graph, function_type, function_name);
}

static bool imported_pollable_identity(
    turbowasm_wasi02_poll *poll,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    turbowasm_component_type_kind *out_kind) {
    const turbowasm_component_type *handle_type;
    const turbowasm_component_type *resource_type;

    if (poll == NULL ||
        !poll->pollable_identity_bound ||
        graph == NULL ||
        type.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED)
        return false;

    handle_type = turbowasm_component_type_graph_get(
        graph, type.as.indexed);
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
    uint64_t token;
    uint32_t handle;

    if (value == NULL || out_handle == NULL ||
        !imported_pollable_identity(
            poll, graph, type, &kind) ||
        value->kind != kind ||
        value->as.resource_rep.kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_TYPE_MISMATCH;

    handle = (uint32_t)value->as.resource_rep.as.i32;
    if (pollable_token(poll, handle, &token) !=
        TURBOWASM_OK)
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
    uint64_t token;

    if (out == NULL ||
        !imported_pollable_identity(
            poll, graph, type, &kind) ||
        pollable_token(poll, handle, &token) !=
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

    (void)instance_name;
    (void)graph;
    (void)function_type;

    if (poll == NULL || !poll->initialized ||
        trap == NULL ||
        !wasi02_poll_can_bind(
            context, instance_name, function_name,
            graph, function_type))
        return TURBOWASM_TYPE_MISMATCH;

    *trap = TURBOWASM_TRAP_NONE;

    if (component_name_is(
            function_name,
            "[method]pollable.ready")) {
        uint32_t handle;
        bool ready = false;
        turbowasm_status status;

        if (argument_count != 1u ||
            arguments == NULL ||
            out_result == NULL ||
            arguments[0].kind !=
                TURBOWASM_COMPONENT_TYPE_BORROW ||
            arguments[0].as.resource_rep.kind !=
                TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;

        handle =
            (uint32_t)arguments[0].as.resource_rep.as.i32;
        status = turbowasm_wasi02_pollable_ready(
            poll, handle, &ready);
        if (status != TURBOWASM_OK)
            return status;

        memset(out_result, 0, sizeof(*out_result));
        out_result->kind = TURBOWASM_COMPONENT_TYPE_BOOL;
        out_result->as.boolean = ready;
        return TURBOWASM_OK;
    }

    if (component_name_is(
            function_name,
            "[method]pollable.block")) {
        uint32_t handle;

        if (argument_count != 1u ||
            arguments == NULL ||
            out_result != NULL ||
            arguments[0].kind !=
                TURBOWASM_COMPONENT_TYPE_BORROW ||
            arguments[0].as.resource_rep.kind !=
                TURBOWASM_VALUE_I32)
            return TURBOWASM_TYPE_MISMATCH;

        handle =
            (uint32_t)arguments[0].as.resource_rep.as.i32;
        return turbowasm_wasi02_pollable_block(
            poll, call, handle);
    }

    if (component_name_is(function_name, "poll")) {
        const turbowasm_component_value *list;
        uint32_t *handles = NULL;
        uint32_t *indices = NULL;
        turbowasm_component_value *items = NULL;
        size_t count;
        size_t ready_count = 0u;
        size_t i;
        turbowasm_status status;

        if (argument_count != 1u ||
            arguments == NULL ||
            out_result == NULL)
            return TURBOWASM_TYPE_MISMATCH;
        list = &arguments[0];
        if (list->kind != TURBOWASM_COMPONENT_TYPE_LIST ||
            list->as.list.count == 0u ||
            list->as.list.count > SIZE_MAX / sizeof(*handles) ||
            (list->as.list.count != 0u &&
             list->as.list.items == NULL)) {
            *trap = TURBOWASM_TRAP_UNREACHABLE;
            return TURBOWASM_TRAPPED;
        }

        count = (size_t)list->as.list.count;
        handles = (uint32_t *)turbowasm_rt_calloc(
            count, sizeof(*handles));
        indices = (uint32_t *)turbowasm_rt_calloc(
            count, sizeof(*indices));
        if (handles == NULL || indices == NULL) {
            turbowasm_rt_free(handles);
            turbowasm_rt_free(indices);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        for (i = 0u; i < count; ++i) {
            const turbowasm_component_value *item =
                &list->as.list.items[i];
            if (item->kind !=
                    TURBOWASM_COMPONENT_TYPE_BORROW ||
                item->as.resource_rep.kind !=
                    TURBOWASM_VALUE_I32) {
                status = TURBOWASM_TYPE_MISMATCH;
                goto poll_done;
            }
            handles[i] =
                (uint32_t)item->as.resource_rep.as.i32;
        }

        status = turbowasm_wasi02_poll_many(
            poll, call, handles, count,
            indices, count, &ready_count);
        if (status != TURBOWASM_OK)
            goto poll_done;

        items = (turbowasm_component_value *)
            turbowasm_rt_calloc(
                ready_count, sizeof(*items));
        if (items == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto poll_done;
        }
        for (i = 0u; i < ready_count; ++i) {
            items[i].kind = TURBOWASM_COMPONENT_TYPE_U32;
            items[i].as.u32 = indices[i];
        }

        memset(out_result, 0, sizeof(*out_result));
        out_result->kind = TURBOWASM_COMPONENT_TYPE_LIST;
        out_result->as.list.items = items;
        out_result->as.list.count = ready_count;
        items = NULL;
        status = TURBOWASM_OK;

poll_done:
        turbowasm_rt_free(handles);
        turbowasm_rt_free(indices);
        turbowasm_rt_free(items);
        return status;
    }

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
