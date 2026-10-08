#include "component_exec.h"
#include "runtime_alloc.h"

const turbowasm_component_resource_identity *turbowasm_component_exec_resource_identity(
    const turbowasm_component_exec *exec, uint64_t declaration) {
    uint32_t i;
    if (exec == NULL || exec->type_view == NULL) return NULL;
    for (i = 0u; i < exec->type_view->identity_count; ++i)
        if (exec->type_view->identities[i].declaration == declaration) return &exec->type_view->identities[i];
    return NULL;
}

static turbowasm_status bind_resource_import(turbowasm_component_exec *exec,
    turbowasm_component_name instance, const turbowasm_component_instance_type_export *export_desc,
    const turbowasm_component_type *resource) {
    turbowasm_component_exec *provider = NULL;
    const turbowasm_component_type *target;
    turbowasm_component_resource_identity *identity;
    uint32_t i, resource_type = 0u;
    turbowasm_component_name name = {export_desc->name, export_desc->name_size};
    for (i = 0u; i < exec->import_set_count; ++i) {
        const turbowasm_component_exec_imports *imports = &exec->import_sets[i];
        turbowasm_component_exec *candidate = NULL;
        uint32_t type = 0u;
        turbowasm_status status;
        if (imports->resource_target == NULL) continue;
        status = imports->resource_target(imports->context, instance, name, &candidate, &type);
        if (status == TURBOWASM_TYPE_MISMATCH) continue;
        if (status != TURBOWASM_OK) return status;
        if (provider != NULL || candidate == NULL || candidate == exec || !candidate->initialized ||
            candidate->task_domain.table == NULL) return TURBOWASM_TYPE_MISMATCH;
        provider = candidate; resource_type = type;
    }
    /* Synchronous capability adapters use their existing resource callbacks. */
    if (provider == NULL) return TURBOWASM_OK;
    target = turbowasm_component_resource_definition(&provider->binary->type_graph, resource_type);
    if (target == NULL || target->as.resource.instance_key == NULL ||
        target->as.resource.rep_type != resource->as.resource.rep_type) return TURBOWASM_TYPE_MISMATCH;
    if (target->as.resource.identity_alias) {
        const turbowasm_component_resource_identity *parent =
            turbowasm_component_exec_resource_identity(provider, target->as.resource.identity);
        if (parent == NULL || parent->provider == NULL) return TURBOWASM_TYPE_MISMATCH;
    }
    identity = (turbowasm_component_resource_identity *)
        turbowasm_component_exec_resource_identity(exec, resource->as.resource.identity);
    if (identity == NULL) return TURBOWASM_TYPE_MISMATCH;
    if (identity->provider != NULL)
        return identity->runtime == target->as.resource.instance_key ? TURBOWASM_OK : TURBOWASM_TYPE_MISMATCH;
    if (provider->async_import_owners == UINT32_MAX) return TURBOWASM_OUT_OF_MEMORY;
    identity->provider = provider; identity->provider_declaration = target->as.resource.identity;
    identity->runtime = target->as.resource.instance_key;
    ++provider->async_import_owners;
    turbowasm_component_type_view_bind(exec->type_view, identity->declaration, identity->runtime);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_resource_imports_init(turbowasm_component_exec *exec) {
    uint32_t i, j;
    if (exec->type_view == NULL) return TURBOWASM_OK;
    for (i = 0u; i < exec->binary->import_count; ++i) {
        const turbowasm_component_import *import = &exec->binary->imports[i];
        const turbowasm_component_type *type;
        if (import->kind != TURBOWASM_COMPONENT_EXTERN_INSTANCE) continue;
        type = turbowasm_component_type_graph_get(&exec->binary->type_graph, import->type_index);
        if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_INSTANCE || type->as.instance == NULL)
            return TURBOWASM_MALFORMED_MODULE;
        for (j = 0u; j < type->as.instance->export_count; ++j) {
            const turbowasm_component_instance_type_export *export_desc = &type->as.instance->exports[j];
            const turbowasm_component_type *resource;
            turbowasm_status status;
            if (export_desc->kind != TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE) continue;
            resource = turbowasm_component_type_graph_get(&type->as.instance->type_graph, export_desc->type_index);
            if (resource == NULL || resource->kind != TURBOWASM_COMPONENT_TYPE_RESOURCE) continue;
            status = bind_resource_import(exec, import->name, export_desc, resource);
            if (status != TURBOWASM_OK) return status;
        }
    }
    return TURBOWASM_OK;
}

void turbowasm_component_exec_resource_imports_destroy(turbowasm_component_exec *exec) {
    uint32_t i;
    if (exec->type_view == NULL) return;
    for (i = 0u; i < exec->type_view->identity_count; ++i) {
        turbowasm_component_resource_identity *identity = &exec->type_view->identities[i];
        if (identity->provider != NULL) --identity->provider->async_import_owners;
        identity->provider = NULL;
    }
}

typedef struct turbowasm_component_async_resource_owner {
    turbowasm_component_exec *exec;
    turbowasm_component_exec_resource_codec *lower_scope;
    struct turbowasm_component_async_resource_owner *lower_next;
    uint64_t identity;
    uint64_t reserved_identity;
    const void *instance_key;
    turbowasm_value rep;
    uint32_t lender, reserved;
    bool borrowed, committed;
    const bool *host_admitted;
    turbowasm_component_resource_host_published_fn host_published;
    turbowasm_component_resource_host_finish_fn host_finish;
    void *host_context;
    /* Fresh lifted values can leave their task/endpoint before destruction.
     * Public domains retain their exec owner independently of that producer. */
    void *instance_owner;
    void (*instance_release)(void *owner);
} turbowasm_component_async_resource_owner;

static turbowasm_status release_resource(void *context) {
    turbowasm_component_async_resource_owner *owner = context;
    turbowasm_status status = TURBOWASM_OK;
    void *instance_owner = owner->instance_owner;
    void (*instance_release)(void *) = owner->instance_release;
    if (owner->lower_scope != NULL) return TURBOWASM_TRAPPED;
    if (owner->host_finish != NULL)
        status = owner->host_finish(owner->host_context, owner->committed);
    else if (owner->borrowed)
        status = turbowasm_component_resource_lend_release(&owner->exec->resource_table, owner->lender, owner->identity);
    else if (!owner->committed)
        status = turbowasm_component_exec_resource_release(owner->exec, owner->identity, owner->rep);
    --owner->exec->async_resource_owners;
    turbowasm_rt_free(owner);
    /* This can destroy the exec, its graph and the defining Core instance. */
    if (instance_release != NULL) instance_release(instance_owner);
    return status;
}

size_t turbowasm_component_exec_resource_adopt_size(void) {
    return sizeof(turbowasm_component_async_resource_owner);
}

turbowasm_status turbowasm_component_exec_resource_adopt(
    turbowasm_component_exec *exec, turbowasm_component_value *value,
    const bool *admitted, turbowasm_component_resource_host_published_fn published,
    turbowasm_component_resource_host_finish_fn finish,
    void *context) {
    const turbowasm_component_resource_identity *identity;
    const turbowasm_component_type *resource = NULL;
    turbowasm_component_async_resource_owner *owner;
    uint32_t i;
    if (exec == NULL || !exec->initialized || exec->task_domain.table == NULL ||
        value == NULL || value->release != NULL || value->release_context != NULL ||
        (value->kind != TURBOWASM_COMPONENT_TYPE_OWN && value->kind != TURBOWASM_COMPONENT_TYPE_BORROW) ||
        admitted == NULL || published == NULL || finish == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    identity = turbowasm_component_exec_resource_identity(exec, value->resource_identity);
    if (identity == NULL || identity->runtime == NULL ||
        (value->resource_instance_key != NULL && value->resource_instance_key != identity->runtime))
        return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < exec->binary->type_graph.count; ++i) {
        const turbowasm_component_type *candidate =
            turbowasm_component_type_graph_get(&exec->binary->type_graph, i);
        if (candidate->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE &&
            candidate->as.resource.identity == identity->declaration) { resource = candidate; break; }
    }
    if (resource == NULL || (resource->as.resource.identity_alias && identity->provider == NULL) ||
        value->as.resource_rep.kind !=
        (resource->as.resource.rep_type == 0x7fu ? TURBOWASM_VALUE_I32 : TURBOWASM_VALUE_I64))
        return TURBOWASM_TYPE_MISMATCH;
    if (exec->async_resource_owners >= exec->resource_table.max_entries)
        return TURBOWASM_OUT_OF_MEMORY;
    owner = turbowasm_rt_calloc(1u, sizeof(*owner));
    if (owner == NULL) return TURBOWASM_OUT_OF_MEMORY;
    owner->exec = exec; owner->identity = identity->declaration;
    owner->instance_key = identity->runtime; owner->rep = value->as.resource_rep;
    owner->borrowed = value->kind == TURBOWASM_COMPONENT_TYPE_BORROW;
    owner->host_admitted = admitted; owner->host_published = published;
    owner->host_finish = finish; owner->host_context = context;
    ++exec->async_resource_owners;
    value->resource_instance_key = owner->instance_key;
    value->release = release_resource; value->release_context = owner;
    return TURBOWASM_OK;
}

bool turbowasm_component_exec_resource_value_idle(const turbowasm_component_value *value) {
    const turbowasm_component_async_resource_owner *owner;
    if (value == NULL || value->release != release_resource || value->release_context == NULL)
        return false;
    owner = value->release_context;
    return owner->lower_scope == NULL;
}

bool turbowasm_component_exec_resource_value_owned(
    const turbowasm_component_exec *exec, const turbowasm_component_value *value) {
    const turbowasm_component_async_resource_owner *owner;
    if (value == NULL || value->kind != TURBOWASM_COMPONENT_TYPE_OWN ||
        !turbowasm_component_exec_resource_value_idle(value)) return false;
    owner = value->release_context;
    return owner->exec == exec && !owner->borrowed && !owner->committed &&
        owner->host_finish == NULL && value->resource_identity == owner->identity &&
        value->resource_instance_key == owner->instance_key &&
        value->as.resource_rep.kind == owner->rep.kind &&
        (owner->rep.kind == TURBOWASM_VALUE_I32
            ? value->as.resource_rep.as.i32 == owner->rep.as.i32
            : owner->rep.kind == TURBOWASM_VALUE_I64 && value->as.resource_rep.as.i64 == owner->rep.as.i64);
}

void turbowasm_component_exec_resource_value_disown(turbowasm_component_value *value) {
    turbowasm_component_async_resource_owner *owner = value->release_context;
    owner->committed = true;
    (void)release_resource(owner);
    memset(value, 0, sizeof(*value));
}

static const turbowasm_component_type *instance_resource(turbowasm_component_exec_resource_codec *codec,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref ref,
    const turbowasm_component_type **out_type) {
    const turbowasm_component_type *type, *resource;
    const turbowasm_component_resource_identity *identity;
    if (codec == NULL || codec->exec == NULL ||
        ref.kind != TURBOWASM_COMPONENT_TYPE_REF_INDEXED) return NULL;
    type = turbowasm_component_type_graph_get(graph, ref.as.indexed);
    if (type == NULL || (type->kind != TURBOWASM_COMPONENT_TYPE_OWN && type->kind != TURBOWASM_COMPONENT_TYPE_BORROW))
        return NULL;
    resource = turbowasm_component_resource_definition(graph, type->as.handle.resource_type);
    if (resource == NULL) return NULL;
    identity = turbowasm_component_exec_resource_identity(codec->exec, resource->as.resource.identity);
    if (identity == NULL || identity->runtime != resource->as.resource.instance_key ||
        (resource->as.resource.identity_alias && identity->provider == NULL)) return NULL;
    *out_type = type;
    return resource;
}

static turbowasm_status lift_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, uint32_t handle, turbowasm_component_value *out) {
    turbowasm_component_exec_resource_codec *codec = context;
    const turbowasm_component_type *type, *resource = instance_resource(codec, graph, ref, &type);
    turbowasm_component_async_resource_owner *owner;
    turbowasm_value rep = {0};
    turbowasm_status status;
    void *instance_owner;
    void (*instance_release)(void *);
    if (out == NULL || out->kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED || out->release != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (resource == NULL) return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_rep(&codec->exec->resource_table, handle, resource->as.resource.identity, &rep);
    if (status != TURBOWASM_OK) return status;
    if (rep.kind != (resource->as.resource.rep_type == 0x7fu ? TURBOWASM_VALUE_I32 : TURBOWASM_VALUE_I64))
        return TURBOWASM_TYPE_MISMATCH;
    if (codec->exec->async_resource_owners >= codec->exec->resource_table.max_entries)
        return TURBOWASM_OUT_OF_MEMORY;
    if ((codec->exec->task_domain.pair_retain == NULL) != (codec->exec->task_domain.pair_release == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    instance_owner = codec->exec->task_domain.pair_owner;
    instance_release = codec->exec->task_domain.pair_release;
    if (instance_release != NULL && !codec->exec->task_domain.pair_retain(instance_owner))
        return TURBOWASM_INVALID_ARGUMENT;
    owner = turbowasm_rt_calloc(1u, sizeof(*owner));
    if (owner == NULL) {
        if (instance_release != NULL) instance_release(instance_owner);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    owner->exec = codec->exec; owner->identity = resource->as.resource.identity; owner->rep = rep;
    owner->instance_owner = instance_owner; owner->instance_release = instance_release;
    owner->instance_key = resource->as.resource.instance_key;
    owner->borrowed = type->kind == TURBOWASM_COMPONENT_TYPE_BORROW; owner->lender = handle;
    status = owner->borrowed
        ? turbowasm_component_resource_lend_acquire(&codec->exec->resource_table, handle, owner->identity)
        : turbowasm_component_resource_take_owned(&codec->exec->resource_table, handle, owner->identity, &rep);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(owner);
        if (instance_release != NULL) instance_release(instance_owner);
        return status;
    }
    ++codec->exec->async_resource_owners;
    out->kind = type->kind; out->resource_identity = owner->identity; out->as.resource_rep = rep;
    out->resource_instance_key = owner->instance_key;
    out->release = release_resource; out->release_context = owner;
    return TURBOWASM_OK;
}

static turbowasm_status lower_resource(void *context, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, const turbowasm_component_value *value, uint32_t *out) {
    turbowasm_component_exec_resource_codec *codec = context;
    const turbowasm_component_type *type, *resource = instance_resource(codec, graph, ref, &type);
    turbowasm_component_async_resource_owner *owner;
    turbowasm_status status;
    if (out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (resource == NULL || value == NULL || value->kind != type->kind || value->release != release_resource)
        return TURBOWASM_TYPE_MISMATCH;
    owner = value->release_context;
    if (owner == NULL || owner->instance_key != resource->as.resource.instance_key ||
        value->resource_instance_key != owner->instance_key ||
        value->resource_identity != owner->identity || owner->committed || owner->lower_scope != NULL)
        return TURBOWASM_TYPE_MISMATCH;
    if (owner->host_admitted != NULL && !*owner->host_admitted)
        return TURBOWASM_INVALID_ARGUMENT;
    if (owner->borrowed) {
        const turbowasm_component_resource_identity *identity =
            turbowasm_component_exec_resource_identity(codec->exec, resource->as.resource.identity);
        /* Canonical lower_borrow into the defining instance passes the rep;
         * only the source lender remains pinned, with no callee borrow handle. */
        if (identity->provider == NULL) {
            if (owner->rep.kind != TURBOWASM_VALUE_I32) return TURBOWASM_TYPE_MISMATCH;
            *out = (uint32_t)owner->rep.as.i32;
            return TURBOWASM_OK;
        }
        if (codec->borrow_scope == NULL || codec->borrow_scope->domain != &codec->exec->task_domain ||
            codec->borrow_scope->destroying) return TURBOWASM_TRAPPED;
    }
    status = turbowasm_component_handle_insert(&codec->exec->resource_table,
        TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION, owner, &owner->reserved);
    if (status != TURBOWASM_OK) return status;
    owner->reserved_identity = resource->as.resource.identity;
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
    uint32_t borrows = 0u;
    if (codec == NULL || codec->exec == NULL) return TURBOWASM_INVALID_ARGUMENT;
    for (owner = codec->lower_head; owner != NULL; owner = owner->lower_next) {
        if (owner->lower_scope != codec || owner->committed || owner->reserved_identity == 0u ||
            (owner->rep.kind != TURBOWASM_VALUE_I32 && owner->rep.kind != TURBOWASM_VALUE_I64) ||
            turbowasm_component_handle_object(&codec->exec->resource_table, owner->reserved,
                TURBOWASM_COMPONENT_HANDLE_RESOURCE_RESERVATION) != owner) return TURBOWASM_TRAPPED;
        if (owner->borrowed) {
            if (codec->borrow_scope == NULL || codec->borrow_scope->domain != &codec->exec->task_domain ||
                codec->borrow_scope->destroying) return TURBOWASM_TRAPPED;
            if (borrows == UINT32_MAX - codec->borrow_scope->borrowed_handles) return TURBOWASM_OUT_OF_MEMORY;
            ++borrows;
        }
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_exec_resource_codec_commit(turbowasm_component_exec_resource_codec *codec) {
    turbowasm_status status = turbowasm_component_exec_resource_codec_preflight(codec);
    if (status != TURBOWASM_OK) return status;
    while (codec->lower_head != NULL) {
        turbowasm_component_async_resource_owner *owner = codec->lower_head;
        status = owner->borrowed
            ? turbowasm_component_resource_publish_borrowed(&codec->exec->resource_table,
                owner->reserved, owner, owner->reserved_identity, owner->rep, &codec->borrow_scope->borrowed_handles)
            : turbowasm_component_resource_publish(&codec->exec->resource_table,
                owner->reserved, owner, owner->reserved_identity, owner->rep);
        if (status != TURBOWASM_OK) return status;
        codec->lower_head = owner->lower_next;
        owner->lower_scope = NULL; owner->lower_next = NULL; owner->reserved = 0u; owner->committed = true;
        if (owner->host_published != NULL) owner->host_published(owner->host_context);
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
