#include "component_host_endpoint_internal.h"
#include <string.h>

/* The public façade reuses existing task, endpoint and transfer owners. */
static turbowasm_component_instance_public_impl *async_instance(
    const turbowasm_component_instance *instance) {
    turbowasm_component_instance_public_impl *impl = turbowasm_component_instance_public_impl_get(instance);
    if (impl == NULL || !impl->exec.initialized || !impl->host_budget_owned ||
        impl->exec.task_domain.table == NULL) return NULL;
    return impl;
}

void turbowasm_component_async_options_init(turbowasm_component_async_options *options) {
    turbowasm_component_async_options_init_private(options);
}
turbowasm_status turbowasm_component_load_async_borrowed(turbowasm_component *component,
    const uint8_t *bytes, size_t size) {
    return turbowasm_component_load_async_private(component, bytes, size, NULL);
}
turbowasm_status turbowasm_component_load_async_borrowed_with_config(turbowasm_component *component,
    const uint8_t *bytes, size_t size, const turbowasm_runtime_config *config) {
    return turbowasm_component_load_async_private(component, bytes, size, config);
}
turbowasm_status turbowasm_component_instance_create_async_with_options(turbowasm_component_instance *instance,
    const turbowasm_component *component, const turbowasm_component_async_options *options) {
    turbowasm_component_async_options defaults;
    if (options == NULL) { turbowasm_component_async_options_init(&defaults); options = &defaults; }
    return turbowasm_component_instance_create_async_with_options_private(instance, component, options, NULL, 0u);
}

static turbowasm_status export_type(const turbowasm_component_instance *instance,
    turbowasm_name name, const turbowasm_component_type **out) {
    turbowasm_component_instance_public_impl *impl = turbowasm_component_instance_public_impl_get(instance);
    const turbowasm_component_binary *binary;
    uint32_t i;
    if (impl == NULL || !impl->exec.initialized || impl->shutdown_driving || impl->host_activity != 0u ||
        (name.size != 0u && name.bytes == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    binary = impl->exec.binary;
    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *entry = &binary->exports[i];
        uint32_t adapter;
        if (entry->kind != TURBOWASM_COMPONENT_EXTERN_FUNCTION || entry->name.size != name.size ||
            (name.size != 0u && memcmp(entry->name.bytes, name.bytes, name.size) != 0)) continue;
        if (entry->item_index >= impl->exec.function_count) return TURBOWASM_MALFORMED_MODULE;
        adapter = impl->exec.function_adapter_indices[entry->item_index];
        if (adapter >= binary->canon_lift_count) return TURBOWASM_LINK_ERROR;
        *out = turbowasm_component_type_graph_get(&binary->type_graph, binary->canon_lifts[adapter].type_index);
        return *out != NULL && (*out)->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION ? TURBOWASM_OK : TURBOWASM_MALFORMED_MODULE;
    }
    return TURBOWASM_LINK_ERROR;
}
static turbowasm_component_type_token type_token(const turbowasm_component_instance *instance,
    turbowasm_component_type_ref type) {
    return (turbowasm_component_type_token){instance->impl,
        type.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE ? (uint32_t)type.as.inline_type : type.as.indexed,
        type.kind == TURBOWASM_COMPONENT_TYPE_REF_INLINE};
}
static turbowasm_status token_type(const turbowasm_component_instance *instance,
    turbowasm_component_type_token token, const turbowasm_component_type **out) {
    turbowasm_component_instance_public_impl *impl = turbowasm_component_instance_public_impl_get(instance);
    if (impl == NULL || token.scope != impl || !impl->exec.initialized || impl->shutdown_driving ||
        impl->host_activity != 0u) return TURBOWASM_INVALID_ARGUMENT;
    if (token.inline_type) {
        if (token.id < TURBOWASM_COMPONENT_TYPE_BOOL || token.id > TURBOWASM_COMPONENT_TYPE_STRING)
            return TURBOWASM_INVALID_ARGUMENT;
        return TURBOWASM_TYPE_MISMATCH;
    }
    *out = turbowasm_component_type_graph_get(&impl->exec.binary->type_graph, token.id);
    return *out != NULL ? TURBOWASM_OK : TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_component_instance_parameter_type(const turbowasm_component_instance *instance,
    turbowasm_name name, size_t index, turbowasm_component_type_token *out) {
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = export_type(instance, name, &type);
    if (status != TURBOWASM_OK) return status;
    if (index >= type->as.function.param_count) return TURBOWASM_INVALID_ARGUMENT;
    *out = type_token(instance, type->as.function.params[index]); return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_instance_result_type(const turbowasm_component_instance *instance,
    turbowasm_name name, turbowasm_component_type_token *out) {
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = export_type(instance, name, &type);
    if (status != TURBOWASM_OK) return status;
    if (!type->as.function.has_result) return TURBOWASM_TYPE_MISMATCH;
    *out = type_token(instance, type->as.function.result); return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_instance_type_child(const turbowasm_component_instance *instance,
    turbowasm_component_type_token parent, turbowasm_component_type_edge edge,
    size_t index, turbowasm_component_type_token *out) {
    const turbowasm_component_type *type;
    turbowasm_component_type_ref child;
    turbowasm_status status;
    if (out == NULL || (edge != TURBOWASM_COMPONENT_TYPE_EDGE_FIELD &&
        edge != TURBOWASM_COMPONENT_TYPE_EDGE_CASE && index != 0u)) return TURBOWASM_INVALID_ARGUMENT;
    status = token_type(instance, parent, &type);
    if (status != TURBOWASM_OK) return status;
    switch (edge) {
        case TURBOWASM_COMPONENT_TYPE_EDGE_ELEMENT:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_LIST) return TURBOWASM_TYPE_MISMATCH;
            child = type->as.list.element_type; break;
        case TURBOWASM_COMPONENT_TYPE_EDGE_FIELD:
            if (type->kind == TURBOWASM_COMPONENT_TYPE_RECORD) {
                if (index >= type->as.record.count) return TURBOWASM_INVALID_ARGUMENT;
                child = type->as.record.fields[index].type;
            } else if (type->kind == TURBOWASM_COMPONENT_TYPE_TUPLE) {
                if (index >= type->as.tuple.count) return TURBOWASM_INVALID_ARGUMENT;
                child = type->as.tuple.elements[index];
            } else return TURBOWASM_TYPE_MISMATCH;
            break;
        case TURBOWASM_COMPONENT_TYPE_EDGE_CASE:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_VARIANT) return TURBOWASM_TYPE_MISMATCH;
            if (index >= type->as.variant.count) return TURBOWASM_INVALID_ARGUMENT;
            if (!type->as.variant.cases[index].has_payload) return TURBOWASM_TYPE_MISMATCH;
            child = type->as.variant.cases[index].payload; break;
        case TURBOWASM_COMPONENT_TYPE_EDGE_SOME:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_OPTION) return TURBOWASM_TYPE_MISMATCH;
            child = type->as.option.payload; break;
        case TURBOWASM_COMPONENT_TYPE_EDGE_OK:
        case TURBOWASM_COMPONENT_TYPE_EDGE_ERROR:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_RESULT) return TURBOWASM_TYPE_MISMATCH;
            if (edge == TURBOWASM_COMPONENT_TYPE_EDGE_OK) {
                if (!type->as.result.has_ok) return TURBOWASM_TYPE_MISMATCH;
                child = type->as.result.ok;
            } else {
                if (!type->as.result.has_error) return TURBOWASM_TYPE_MISMATCH;
                child = type->as.result.error;
            }
            break;
        case TURBOWASM_COMPONENT_TYPE_EDGE_PAYLOAD:
            if (type->kind != TURBOWASM_COMPONENT_TYPE_FUTURE && type->kind != TURBOWASM_COMPONENT_TYPE_STREAM)
                return TURBOWASM_TYPE_MISMATCH;
            if (!type->as.async_value.has_payload) return TURBOWASM_TYPE_MISMATCH;
            child = type->as.async_value.payload; break;
        default: return TURBOWASM_INVALID_ARGUMENT;
    }
    *out = type_token(instance, child); return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_instance_endpoint_type_get(const turbowasm_component_instance *instance,
    turbowasm_component_type_token token, turbowasm_component_async_endpoint_type *out) {
    const turbowasm_component_type *type;
    turbowasm_status status;
    turbowasm_component_async_endpoint_type info = {0};
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = token_type(instance, token, &type);
    if (status != TURBOWASM_OK) return status;
    if (type->kind != TURBOWASM_COMPONENT_TYPE_FUTURE && type->kind != TURBOWASM_COMPONENT_TYPE_STREAM)
        return TURBOWASM_TYPE_MISMATCH;
    info.future = type->kind == TURBOWASM_COMPONENT_TYPE_FUTURE;
    info.has_payload = type->as.async_value.has_payload;
    if (info.has_payload) info.payload = type_token(instance, type->as.async_value.payload);
    *out = info; return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_async_task_create(turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name name,
    const turbowasm_component_host_value *arguments, size_t count) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    return impl != NULL && impl->host_activity == 0u ? turbowasm_component_host_task_create(task, impl, name, arguments, count, false,
        &impl->host_budget) : TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_component_async_task_create_move(turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name name,
    turbowasm_component_host_value *arguments, size_t count) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    return impl != NULL && impl->host_activity == 0u ? turbowasm_component_host_task_create(task, impl, name, arguments, count, true,
        &impl->host_budget) : TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_component_async_task_resume(turbowasm_component_async_task *task,
    const turbowasm_execution_options *options) {
    turbowasm_status status = turbowasm_component_host_task_resume(task, options);
    const turbowasm_component_task *view = turbowasm_component_host_task_view(task);
    if (status == TURBOWASM_OK && view != NULL && view->state >= TURBOWASM_EXECUTION_COMPLETED &&
        view->phase == TURBOWASM_COMPONENT_TASK_CANCELLED) return TURBOWASM_INTERRUPTED;
    return status;
}
turbowasm_status turbowasm_component_async_task_request_cancel(turbowasm_component_async_task *task) {
    turbowasm_component_async_task_state state;
    turbowasm_status status = turbowasm_component_async_task_state_get(task, &state);
    if (status != TURBOWASM_OK) return status;
    if (state.terminal || state.cancellation_requested) return TURBOWASM_OK;
    return turbowasm_component_host_task_request_cancel(task);
}
turbowasm_status turbowasm_component_async_task_take_result(turbowasm_component_async_task *task,
    turbowasm_component_host_value *out, size_t *count) { return turbowasm_component_host_task_take_result(task, out, count); }
turbowasm_status turbowasm_component_async_task_destroy(turbowasm_component_async_task *task) {
    return turbowasm_component_host_task_destroy(task);
}
bool turbowasm_component_async_task_pending_host_wait(const turbowasm_component_async_task *task,
    turbowasm_component_async_wait *out) {
    turbowasm_component_task_host_wait wait;
    if (out == NULL || !turbowasm_component_host_task_pending_host_wait(task, &wait)) return false;
    *out = (turbowasm_component_async_wait){task->impl, wait.task_generation, wait.core_generation, wait.wait};
    return true;
}
static bool same_wait(turbowasm_component_async_wait ticket, const void *owner,
    uint64_t generation, uint64_t continuation, turbowasm_host_wait wait) {
    return ticket.owner == owner && ticket.generation == generation && ticket.continuation == continuation &&
        ticket.wait.generation == wait.generation && ticket.wait.operation_token == wait.operation_token;
}
turbowasm_status turbowasm_component_async_task_complete_host_wait(turbowasm_component_async_task *task,
    turbowasm_component_async_wait ticket, int completion) {
    turbowasm_component_task_host_wait wait;
    if (!turbowasm_component_host_task_pending_host_wait(task, &wait) ||
        !same_wait(ticket, task->impl, wait.task_generation, wait.core_generation, wait.wait))
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_host_task_complete_host_wait(task, wait, completion);
}

turbowasm_status turbowasm_component_async_endpoint_pair_create(turbowasm_component_async_endpoint *reader,
    turbowasm_component_async_endpoint *writer, turbowasm_component_instance *instance,
    turbowasm_component_type_token token) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    turbowasm_component_async_endpoint_type info;
    turbowasm_status status;
    if (impl == NULL) return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_instance_endpoint_type_get(instance, token, &info);
    if (status != TURBOWASM_OK) return status;
    return turbowasm_component_host_endpoint_pair_create(reader, writer, impl, token.id, &impl->host_budget);
}
turbowasm_status turbowasm_component_async_endpoint_state_get(const turbowasm_component_async_endpoint *endpoint,
    turbowasm_component_async_endpoint_state *out) {
    const component_host_endpoint_impl *body = endpoint != NULL ? endpoint->impl : NULL;
    const turbowasm_component_endpoint *end;
    if (body == NULL || out == NULL || body->driving || body->instance->shutdown_driving)
        return TURBOWASM_INVALID_ARGUMENT;
    end = body->endpoint;
    *out = (turbowasm_component_async_endpoint_state){end->waitable.state.endpoint.future,
        end->readable, end->has_payload, end->waitable.state.endpoint.peer_dropped,
        end->waitable.state.endpoint.future && end->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_DONE};
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_async_endpoint_into_value(turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *out) {
    const turbowasm_component_endpoint *end = turbowasm_component_host_endpoint_view(reader);
    const component_host_endpoint_impl *body = reader != NULL ? reader->impl : NULL;
    if (out == NULL || (int)out->kind != 0 || (void *)out == (void *)reader || end == NULL || !end->readable ||
        body->instance->host_activity != 0u ||
        !turbowasm_component_host_endpoint_destroy_ready(reader)) return TURBOWASM_INVALID_ARGUMENT;
    out->kind = end->waitable.state.endpoint.future ? TURBOWASM_COMPONENT_HOST_FUTURE : TURBOWASM_COMPONENT_HOST_STREAM;
    if (end->waitable.state.endpoint.future) out->as.future = *reader; else out->as.stream = *reader;
    reader->impl = NULL; return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_async_endpoint_from_value(turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *source) {
    turbowasm_component_async_endpoint *carrier;
    const turbowasm_component_endpoint *end;
    if (reader == NULL || reader->impl != NULL || source == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (source->kind == TURBOWASM_COMPONENT_HOST_FUTURE) carrier = &source->as.future;
    else if (source->kind == TURBOWASM_COMPONENT_HOST_STREAM) carrier = &source->as.stream;
    else return TURBOWASM_TYPE_MISMATCH;
    end = turbowasm_component_host_endpoint_view(carrier);
    if (reader == carrier || end == NULL || !end->readable ||
        ((component_host_endpoint_impl *)carrier->impl)->instance->host_activity != 0u ||
        end->waitable.state.endpoint.future != (source->kind == TURBOWASM_COMPONENT_HOST_FUTURE) ||
        !turbowasm_component_host_endpoint_destroy_ready(carrier)) return TURBOWASM_INVALID_ARGUMENT;
    *reader = *carrier; memset(source, 0, sizeof(*source)); return TURBOWASM_OK;
}
turbowasm_status turbowasm_component_async_endpoint_destroy(turbowasm_component_async_endpoint *endpoint) {
    if (endpoint == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (endpoint->impl == NULL) return TURBOWASM_OK;
    if (((component_host_endpoint_impl *)endpoint->impl)->instance->host_activity != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_endpoint_destroy_ready(endpoint)) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_host_endpoint_destroy(endpoint);
}

turbowasm_status turbowasm_component_async_transfer_result_destroy(turbowasm_component_async_transfer_result *result) {
    component_host_endpoint_impl *body;
    turbowasm_component_async_transfer_result owned;
    turbowasm_status status, cleanup;
    if (result == NULL) return TURBOWASM_INVALID_ARGUMENT;
    body = result->endpoint.impl;
    if (body != NULL && (body->instance->host_activity != 0u ||
        !turbowasm_component_host_endpoint_destroy_ready(&result->endpoint)))
        return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_host_value_destroy_preflight(&result->values);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_component_host_value_destroy_lock(&result->values, true);
    if (status != TURBOWASM_OK) return status;
    if (body != NULL && body->driving) {
        (void)turbowasm_component_host_value_destroy_lock(&result->values, false);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    if (body != NULL) body->driving = true;
    owned = *result; memset(result, 0, sizeof(*result));
    status = turbowasm_component_host_value_destroy_locked(&owned.values);
    cleanup = turbowasm_component_host_endpoint_destroy_locked(&owned.endpoint);
    return status != TURBOWASM_OK ? status : cleanup;
}

turbowasm_status turbowasm_component_instance_request_shutdown(turbowasm_component_instance *instance) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    return impl != NULL ? turbowasm_component_instance_request_shutdown_private(impl) : TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_component_instance_poll_shutdown(turbowasm_component_instance *instance,
    const turbowasm_execution_options *options) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    return impl != NULL ? turbowasm_component_instance_poll_shutdown_private(impl, options) : TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_component_instance_shutdown_state_get(const turbowasm_component_instance *instance,
    turbowasm_component_async_shutdown_state *out) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    turbowasm_component_async_shutdown_state state = {0};
    turbowasm_yield_reason reason;
    if (impl == NULL || out == NULL || impl->shutdown_driving || impl->host_activity != 0u ||
        impl->exec.task_domain.active != NULL || impl->exec.async_driving)
        return TURBOWASM_INVALID_ARGUMENT;
    state.requested = impl->admission_closed; state.complete = impl->shutdown_complete;
    state.status = state.complete ? impl->shutdown_status : TURBOWASM_YIELDED;
    reason = turbowasm_component_instance_shutdown_yield_reason_private(impl);
    if (state.requested && !state.complete) {
        state.wait_reason = reason == TURBOWASM_YIELD_FUEL ? TURBOWASM_COMPONENT_ASYNC_WAIT_FUEL :
            reason == TURBOWASM_YIELD_INTERRUPTION ? TURBOWASM_COMPONENT_ASYNC_WAIT_INTERRUPTION :
            reason == TURBOWASM_YIELD_HOST_WAIT ? TURBOWASM_COMPONENT_ASYNC_WAIT_HOST_IO :
            TURBOWASM_COMPONENT_ASYNC_WAIT_COMPONENT_EVENT;
    }
    *out = state; return TURBOWASM_OK;
}
bool turbowasm_component_instance_shutdown_pending_host_wait(const turbowasm_component_instance *instance,
    turbowasm_component_async_wait *out) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    turbowasm_component_shutdown_wait wait;
    if (impl == NULL || out == NULL || !turbowasm_component_instance_shutdown_pending_host_wait_private(impl, &wait))
        return false;
    *out = (turbowasm_component_async_wait){impl, wait.shutdown_generation, 0u, wait.wait}; return true;
}
turbowasm_status turbowasm_component_instance_shutdown_complete_host_wait(turbowasm_component_instance *instance,
    turbowasm_component_async_wait ticket, int completion) {
    turbowasm_component_instance_public_impl *impl = async_instance(instance);
    turbowasm_component_shutdown_wait wait;
    if (impl == NULL || !turbowasm_component_instance_shutdown_pending_host_wait_private(impl, &wait) ||
        !same_wait(ticket, impl, wait.shutdown_generation, 0u, wait.wait)) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_instance_shutdown_complete_host_wait_private(impl, wait, completion);
}
