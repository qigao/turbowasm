#include "component_exec.h"
#include "runtime_alloc.h"

typedef struct turbowasm_component_async_resource_owner {
    turbowasm_component_exec *exec;
    turbowasm_component_exec_resource_codec *lower_scope;
    struct turbowasm_component_async_resource_owner *lower_next;
    uint64_t identity;
    turbowasm_value rep;
    uint32_t lender, reserved;
    bool borrowed, committed;
} turbowasm_component_async_resource_owner;

static turbowasm_status release_resource(void *context) {
    turbowasm_component_async_resource_owner *owner = context;
    turbowasm_status status = TURBOWASM_OK;
    if (owner->lower_scope != NULL) return TURBOWASM_TRAPPED;
    if (owner->borrowed)
        status = turbowasm_component_resource_lend_release(&owner->exec->resource_table, owner->lender, owner->identity);
    else if (!owner->committed)
        status = turbowasm_component_exec_resource_release(owner->exec, owner->identity, owner->rep);
    --owner->exec->async_resource_owners;
    turbowasm_rt_free(owner);
    return status;
}

static const turbowasm_component_type *local_resource(turbowasm_component_exec_resource_codec *codec,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    const turbowasm_component_type **out_type) {
    const turbowasm_component_type *type, *resource;
    if (codec == NULL || codec->exec == NULL || graph != &codec->exec->binary->type_graph ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED) return NULL;
    type = turbowasm_component_type_graph_get(graph, ref.as.indexed);
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_OWN && type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return NULL;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    if (resource == NULL || resource->as.resource.identity_alias) return NULL;
    *out_type = type;
    return resource;
}

static turbowasm_status lift_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, uint32_t handle, turbowasm_component_value *out) {
    turbowasm_component_exec_resource_codec *codec = context;
    const turbowasm_component_type *type, *resource = local_resource(codec, graph, ref, &type);
    turbowasm_component_async_resource_owner *owner;
    turbowasm_value rep = {0};
    turbowasm_status status;
    if (out == NULL || out->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || out->release != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (resource == NULL) return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_rep(&codec->exec->resource_table, handle, resource->as.resource.identity, &rep);
    if (status != TURBOWASM_OK) return status;
    if (rep.kind != (resource->as.resource.rep_type == 0x7fu ? TURBOWASM_VALUE_I32 : TURBOWASM_VALUE_I64))
        return TURBOWASM_TYPE_MISMATCH;
    if (codec->exec->async_resource_owners >= codec->exec->resource_table.max_entries)
        return TURBOWASM_OUT_OF_MEMORY;
    owner = turbowasm_rt_calloc(1u, sizeof(*owner));
    if (owner == NULL) return TURBOWASM_OUT_OF_MEMORY;
    owner->exec = codec->exec; owner->identity = resource->as.resource.identity; owner->rep = rep;
    owner->borrowed = type->kind == TURBOWASM_COMPONENT_TYPE_BORROW; owner->lender = handle;
    status = owner->borrowed
        ? turbowasm_component_resource_lend_acquire(&codec->exec->resource_table, handle, owner->identity)
        : turbowasm_component_resource_take_owned(&codec->exec->resource_table, handle, owner->identity, &rep);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(owner); return status; }
    ++codec->exec->async_resource_owners;
    out->kind = type->kind; out->resource_identity = owner->identity; out->as.resource_rep = rep;
    out->release = release_resource; out->release_context = owner;
    return TURBOWASM_OK;
}

static turbowasm_status lower_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, const turbowasm_component_value *value, uint32_t *out) {
    turbowasm_component_exec_resource_codec *codec = context;
    const turbowasm_component_type *type, *resource = local_resource(codec, graph, ref, &type);
    turbowasm_component_async_resource_owner *owner;
    turbowasm_status status;
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (resource == NULL || value == NULL || value->kind != type->kind || value->release != release_resource)
        return TURBOWASM_TYPE_MISMATCH;
    owner = value->release_context;
    if (owner == NULL || owner->exec != codec->exec || owner->identity != resource->as.resource.identity ||
        value->resource_identity != owner->identity || owner->committed || owner->lower_scope != NULL)
        return TURBOWASM_TYPE_MISMATCH;
    if (owner->borrowed) {
        /* Canonical lower_borrow into the defining instance passes the rep;
         * only the source lender remains pinned, with no callee borrow handle. */
        if (owner->rep.kind != TURBOWASM_VALUE_I32) return TURBOWASM_TYPE_MISMATCH;
        *out = (uint32_t)owner->rep.as.i32;
        return TURBOWASM_OK;
    }
    status = turbowasm_component_handle_insert(&codec->exec->resource_table,
        TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION, owner, &owner->reserved);
    if (status != TURBOWASM_OK) return status;
    owner->lower_scope = codec; owner->lower_next = codec->lower_head; codec->lower_head = owner;
    *out = owner->reserved;
    return TURBOWASM_OK;
}

void turbowasm_component_exec_resource_codec_bind(turbowasm_component_exec_resource_codec *codec,
    turbowasm_component_exec *exec, turbowasm_component_canonical_memory *memory) {
    codec->exec = exec;
    memory->resource_lift = lift_resource; memory->resource_lower = lower_resource; memory->resource_context = codec;
}

turbowasm_status turbowasm_component_exec_resource_codec_preflight(const turbowasm_component_exec_resource_codec *codec) {
    const turbowasm_component_async_resource_owner *owner;
    if (codec == NULL || codec->exec == NULL) return TURBOWASM_INVALID_ARGUMENT;
    for (owner = codec->lower_head; owner != NULL; owner = owner->lower_next)
        if (owner->lower_scope != codec || owner->borrowed || owner->committed || owner->identity == 0u ||
            (owner->rep.kind != TURBOWASM_VALUE_I32 && owner->rep.kind != TURBOWASM_VALUE_I64) ||
            turbowasm_component_handle_object(&codec->exec->resource_table, owner->reserved,
                TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION) != owner) return TURBOWASM_TRAPPED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_resource_codec_commit(turbowasm_component_exec_resource_codec *codec) {
    turbowasm_status status = turbowasm_component_exec_resource_codec_preflight(codec);
    if (status != TURBOWASM_OK) return status;
    while (codec->lower_head != NULL) {
        turbowasm_component_async_resource_owner *owner = codec->lower_head;
        status = turbowasm_component_resource_publish(&codec->exec->resource_table,
            owner->reserved, owner, owner->identity, owner->rep);
        if (status != TURBOWASM_OK) return status;
        codec->lower_head = owner->lower_next;
        owner->lower_scope = NULL; owner->lower_next = NULL; owner->reserved = 0u; owner->committed = true;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_resource_codec_rollback(turbowasm_component_exec_resource_codec *codec) {
    turbowasm_status status = TURBOWASM_OK;
    if (codec == NULL || codec->exec == NULL) return TURBOWASM_INVALID_ARGUMENT;
    while (codec->lower_head != NULL) {
        turbowasm_component_async_resource_owner *owner = codec->lower_head;
        void *removed = NULL;
        turbowasm_status cleanup = turbowasm_component_handle_remove(&codec->exec->resource_table,
            owner->reserved, TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION, &removed);
        if (status == TURBOWASM_OK) status = cleanup != TURBOWASM_OK ? cleanup : removed == owner ? TURBOWASM_OK : TURBOWASM_TRAPPED;
        codec->lower_head = owner->lower_next;
        owner->lower_scope = NULL; owner->lower_next = NULL; owner->reserved = 0u;
    }
    return status;
}
