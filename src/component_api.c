#include <turbowasm/component.h>

#include "component_api_internal.h"
#include "component_type_graph.h"
#include "runtime_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool component_instance_retain(turbowasm_component_instance_public_impl *impl);
static void component_instance_release(turbowasm_component_instance_public_impl *impl);

struct turbowasm_component_host_resource {
    turbowasm_component_instance_public_impl *instance;
    struct turbowasm_component_host_resource *next;
    uint64_t identity;
    turbowasm_value rep;
    uint32_t loans;
    bool busy;
    bool external;
};

typedef struct component_resource_argument {
    const turbowasm_component_host_value *source;
    turbowasm_component_host_resource *owner;
    bool own;
    bool reserved;
} component_resource_argument;

typedef struct component_public_admission {
    turbowasm_component_instance_public_impl *instance;
    component_resource_argument *entries;
    size_t count, capacity;
    bool move, committed, started;
} component_public_admission;

#define TW_COMPONENT_PUBLIC_KIND_MATCH(public_kind, internal_kind) \
    _Static_assert( \
        (int)(public_kind) == (int)(internal_kind), \
        "public/internal Component value kind drift")

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_BOOL, TURBOWASM_COMPONENT_TYPE_BOOL);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S8, TURBOWASM_COMPONENT_TYPE_S8);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U8, TURBOWASM_COMPONENT_TYPE_U8);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S16, TURBOWASM_COMPONENT_TYPE_S16);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U16, TURBOWASM_COMPONENT_TYPE_U16);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S32, TURBOWASM_COMPONENT_TYPE_S32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U32, TURBOWASM_COMPONENT_TYPE_U32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_S64, TURBOWASM_COMPONENT_TYPE_S64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_U64, TURBOWASM_COMPONENT_TYPE_U64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_F32, TURBOWASM_COMPONENT_TYPE_F32);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_F64, TURBOWASM_COMPONENT_TYPE_F64);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_CHAR, TURBOWASM_COMPONENT_TYPE_CHAR);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_STRING, TURBOWASM_COMPONENT_TYPE_STRING);
TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_LIST, TURBOWASM_COMPONENT_TYPE_LIST);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_RECORD, TURBOWASM_COMPONENT_TYPE_RECORD);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_TUPLE, TURBOWASM_COMPONENT_TYPE_TUPLE);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_VARIANT, TURBOWASM_COMPONENT_TYPE_VARIANT);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_OPTION, TURBOWASM_COMPONENT_TYPE_OPTION);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_RESULT, TURBOWASM_COMPONENT_TYPE_RESULT);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_ENUM, TURBOWASM_COMPONENT_TYPE_ENUM);

TW_COMPONENT_PUBLIC_KIND_MATCH(
    TURBOWASM_COMPONENT_HOST_FLAGS, TURBOWASM_COMPONENT_TYPE_FLAGS);

TW_COMPONENT_PUBLIC_KIND_MATCH(TURBOWASM_COMPONENT_HOST_OWN, TURBOWASM_COMPONENT_TYPE_OWN);
TW_COMPONENT_PUBLIC_KIND_MATCH(TURBOWASM_COMPONENT_HOST_BORROW, TURBOWASM_COMPONENT_TYPE_BORROW);
#undef TW_COMPONENT_PUBLIC_KIND_MATCH

turbowasm_component_public_impl *
turbowasm_component_public_impl_get(
    const turbowasm_component *component) {
    return component != NULL
        ? (turbowasm_component_public_impl *)component->impl
        : NULL;
}

turbowasm_component_instance_public_impl *
turbowasm_component_instance_public_impl_get(
    const turbowasm_component_instance *instance) {
    return instance != NULL
        ? (turbowasm_component_instance_public_impl *)instance->impl
        : NULL;
}

bool turbowasm_component_public_impl_retain(
    turbowasm_component_public_impl *impl) {
    if (impl == NULL || impl->ref_count == 0u ||
        impl->ref_count == UINT32_MAX)
        return false;
    ++impl->ref_count;
    return true;
}

void turbowasm_component_public_impl_release(
    turbowasm_component_public_impl *impl) {
    turbowasm_runtime_config config;
    turbowasm_runtime_scope scope;

    if (impl == NULL || impl->ref_count == 0u)
        return;

    --impl->ref_count;
    if (impl->ref_count != 0u)
        return;

    config = impl->binary.config;
    turbowasm_component_binary_destroy(&impl->binary);

    scope = turbowasm_runtime_scope_enter(&config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
}

enum { COMPONENT_PUBLIC_MAX_DEPTH = 64, COMPONENT_PUBLIC_FLAGS_WORDS = 1 };

static bool public_kind_supported(
    turbowasm_component_host_value_kind kind) {
    return (kind >= TURBOWASM_COMPONENT_HOST_BOOL &&
            kind <= TURBOWASM_COMPONENT_HOST_FLAGS) ||
        kind == TURBOWASM_COMPONENT_HOST_OWN || kind == TURBOWASM_COMPONENT_HOST_BORROW;
}

static bool public_type_ref_supported(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref) {
    uint32_t features;
    return turbowasm_component_value_type_features(graph, ref, &features);
}

static const turbowasm_component_host_sequence *public_sequence(
    const turbowasm_component_host_value *value) {
    switch (value->kind) {
        case TURBOWASM_COMPONENT_HOST_LIST: return &value->as.list;
        case TURBOWASM_COMPONENT_HOST_RECORD: return &value->as.record;
        case TURBOWASM_COMPONENT_HOST_TUPLE: return &value->as.tuple;
        default: return NULL;
    }
}

static const turbowasm_component_host_variant *public_variant(
    const turbowasm_component_host_value *value) {
    switch (value->kind) {
        case TURBOWASM_COMPONENT_HOST_VARIANT: return &value->as.variant;
        case TURBOWASM_COMPONENT_HOST_OPTION: return &value->as.option;
        case TURBOWASM_COMPONENT_HOST_RESULT: return &value->as.result;
        default: return NULL;
    }
}

typedef turbowasm_status (*component_resource_visit_fn)(
    const turbowasm_component_host_value *value, void *context);

/* No allocation: O(value tree), stack bounded by the canonical nesting limit. */
static turbowasm_status visit_public_resources(
    const turbowasm_component_host_value *value, uint32_t depth,
    component_resource_visit_fn visit, void *context) {
    const turbowasm_component_host_sequence *sequence;
    const turbowasm_component_host_variant *variant;
    size_t i;
    if (value == NULL)
        return TURBOWASM_OK;
    if (depth >= COMPONENT_PUBLIC_MAX_DEPTH)
        return TURBOWASM_TRAPPED;
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN || value->kind == TURBOWASM_COMPONENT_HOST_BORROW)
        return visit(value, context);
    sequence = public_sequence(value);
    variant = public_variant(value);
    if (sequence != NULL) {
        if (sequence->count != 0u && sequence->items == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        if (sequence->count > SIZE_MAX / sizeof(*sequence->items))
            return TURBOWASM_OUT_OF_MEMORY;
        for (i = 0u; i < sequence->count; ++i) {
            turbowasm_status status = visit_public_resources(
                &sequence->items[i], depth + 1u, visit, context);
            if (status != TURBOWASM_OK)
                return status;
        }
    } else if (variant != NULL && variant->payload != NULL) {
        return visit_public_resources(variant->payload, depth + 1u, visit, context);
    }
    return TURBOWASM_OK;
}

static void resource_owner_forget(turbowasm_component_host_resource *owner) {
    turbowasm_component_instance_public_impl *instance = owner->instance;
    turbowasm_component_host_resource **cursor = &instance->resources;
    while (*cursor != owner)
        cursor = &(*cursor)->next;
    *cursor = owner->next;
    --instance->resource_count;
    turbowasm_rt_free(owner);
    component_instance_release(instance);
}

static turbowasm_status resource_owner_release(void *context) {
    turbowasm_component_host_resource *owner = context;
    turbowasm_status status;
    if (owner == NULL || owner->loans != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    owner->busy = true;
    status = turbowasm_component_exec_resource_release(
        &owner->instance->exec, owner->identity, owner->rep);
    resource_owner_forget(owner);
    return status;
}

static turbowasm_status acquire_result_owner(void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref, turbowasm_component_value *value) {
    turbowasm_component_instance_public_impl *instance = context;
    const turbowasm_component_type *handle = turbowasm_component_type_graph_get(graph, ref.as.indexed);
    const turbowasm_component_type *resource = handle != NULL
        ? turbowasm_component_resource_definition(graph, handle->as.handle.resource_type) : NULL;
    turbowasm_component_host_resource *owner;
    turbowasm_status status = TURBOWASM_OUT_OF_MEMORY;
    if (resource == NULL || value->kind != TURBOWASM_COMPONENT_TYPE_OWN)
        return TURBOWASM_TYPE_MISMATCH;
    /* Capability reps are already provider handles. Reject a second live host
     * owner without dropping the first owner's resource. Local reps may repeat. */
    if (resource->as.resource.identity_alias) {
        for (owner = instance->resources; owner != NULL; owner = owner->next) {
            if (owner->external && owner->identity == resource->as.resource.identity &&
                owner->rep.kind == value->as.resource_rep.kind &&
                owner->rep.as.i32 == value->as.resource_rep.as.i32) {
                memset(value, 0, sizeof(*value));
                return TURBOWASM_TRAPPED;
            }
        }
    }
    if (instance->resource_count >= TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS)
        goto fail;
    owner = turbowasm_rt_calloc(1u, sizeof(*owner));
    if (owner == NULL)
        goto fail;
    if (!component_instance_retain(instance)) {
        turbowasm_rt_free(owner);
        status = TURBOWASM_INVALID_ARGUMENT;
        goto fail;
    }
    owner->instance = instance;
    owner->identity = resource->as.resource.identity;
    owner->rep = value->as.resource_rep;
    owner->external = resource->as.resource.identity_alias;
    owner->next = instance->resources;
    instance->resources = owner;
    ++instance->resource_count;
    value->release = resource_owner_release;
    value->release_context = owner;
    value->resource_identity = owner->identity;
    return TURBOWASM_OK;
fail:
    /* The canonical table no longer owns this rep, even when host allocation fails. */
    (void)turbowasm_component_exec_resource_release(
        &instance->exec, resource->as.resource.identity, value->as.resource_rep);
    memset(value, 0, sizeof(*value));
    return status;
}

static turbowasm_status count_resource_argument(
    const turbowasm_component_host_value *value, void *context) {
    component_public_admission *admission = context;
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN && !admission->move)
        return TURBOWASM_INVALID_ARGUMENT;
    if (admission->capacity == TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS)
        return TURBOWASM_OUT_OF_MEMORY;
    ++admission->capacity;
    return TURBOWASM_OK;
}

static turbowasm_status convert_resource_argument(
    component_public_admission *admission,
    const turbowasm_component_host_value *source, turbowasm_component_value *out) {
    bool own = source->kind == TURBOWASM_COMPONENT_HOST_OWN;
    turbowasm_component_host_resource *owner = own ? source->as.own : source->as.borrow;
    component_resource_argument *entry;
    if (owner == NULL || owner->instance != admission->instance || owner->busy ||
        (own && (!admission->move || owner->loans != 0u)) ||
        admission->count >= admission->capacity)
        return TURBOWASM_INVALID_ARGUMENT;
    entry = &admission->entries[admission->count++];
    entry->source = source; entry->owner = owner; entry->own = own;
    out->as.resource_rep = owner->rep;
    out->resource_identity = owner->identity;
    return TURBOWASM_OK;
}

static turbowasm_status reserve_resource_arguments(component_public_admission *admission) {
    size_t i;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->owner->busy || (entry->own && entry->owner->loans != 0u) ||
            (!entry->own && entry->owner->loans == UINT32_MAX))
            return TURBOWASM_INVALID_ARGUMENT;
        if (entry->own)
            entry->owner->busy = true;
        else
            ++entry->owner->loans;
        entry->reserved = true;
    }
    return TURBOWASM_OK;
}

static void admission_commit(void *context) {
    component_public_admission *admission = context;
    size_t i;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->own) {
            /* Only move entry points can place an own leaf in this transaction. */
            memset((turbowasm_component_host_value *)entry->source, 0, sizeof(*entry->source));
            entry->source = NULL;
        }
    }
    admission->committed = true;
}

static void admission_start(component_public_admission *admission) {
    size_t i;
    if (admission->started)
        return;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->own && entry->owner != NULL) {
            resource_owner_forget(entry->owner);
            entry->owner = NULL;
        }
    }
    admission->started = true;
}

static void admission_commit_and_start(void *context) {
    admission_commit(context);
    admission_start(context);
}

static void admission_destroy(component_public_admission *admission) {
    size_t i;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (!entry->reserved || entry->owner == NULL)
            continue;
        if (!entry->own)
            --entry->owner->loans;
        else if (admission->committed)
            (void)resource_owner_release(entry->owner);
        else
            entry->owner->busy = false;
    }
    turbowasm_rt_free(admission->entries);
    memset(admission, 0, sizeof(*admission));
}

static turbowasm_component_value_list *internal_sequence(
    turbowasm_component_value *value) {
    switch (value->kind) {
        case TURBOWASM_COMPONENT_TYPE_LIST: return &value->as.list;
        case TURBOWASM_COMPONENT_TYPE_RECORD: return &value->as.record;
        case TURBOWASM_COMPONENT_TYPE_TUPLE: return &value->as.tuple;
        default: return NULL;
    }
}

static turbowasm_component_value_variant *internal_variant(
    turbowasm_component_value *value) {
    switch (value->kind) {
        case TURBOWASM_COMPONENT_TYPE_VARIANT: return &value->as.variant;
        case TURBOWASM_COMPONENT_TYPE_OPTION: return &value->as.option;
        case TURBOWASM_COMPONENT_TYPE_RESULT: return &value->as.result;
        default: return NULL;
    }
}

static void internal_input_destroy(
    turbowasm_component_value *value) {
    uint64_t i;

    if (value == NULL)
        return;

    {
        turbowasm_component_value_list *sequence = internal_sequence(value);
        turbowasm_component_value_variant *variant = internal_variant(value);
        if (sequence != NULL) {
            for (i = 0u; i < sequence->count; ++i)
                internal_input_destroy(&sequence->items[i]);
            turbowasm_rt_free(sequence->items);
        } else if (variant != NULL && variant->payload != NULL) {
            internal_input_destroy(variant->payload);
            turbowasm_rt_free(variant->payload);
        }
    }

    /*
     * Input strings borrow caller bytes and therefore must never flow through
     * turbowasm_component_value_destroy(), which owns lifted string storage.
     */
    memset(value, 0, sizeof(*value));
}

static turbowasm_status public_to_internal(
    const turbowasm_component_host_value *source,
    turbowasm_component_value *out,
    uint32_t depth, component_public_admission *admission) {
    size_t i;

    if (source == NULL || out == NULL ||
        !public_kind_supported(source->kind))
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= COMPONENT_PUBLIC_MAX_DEPTH)
        return TURBOWASM_TRAPPED;

    memset(out, 0, sizeof(*out));
    out->kind = (turbowasm_component_type_kind)source->kind;

    switch (source->kind) {
        case TURBOWASM_COMPONENT_HOST_BOOL:
            out->as.boolean = source->as.boolean;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S8:
            out->as.s8 = source->as.s8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U8:
            out->as.u8 = source->as.u8;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S16:
            out->as.s16 = source->as.s16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U16:
            out->as.u16 = source->as.u16;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S32:
            out->as.s32 = source->as.s32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U32:
            out->as.u32 = source->as.u32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_S64:
            out->as.s64 = source->as.s64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_U64:
            out->as.u64 = source->as.u64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_F32:
            out->as.f32 = source->as.f32;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_F64:
            out->as.f64 = source->as.f64;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_CHAR:
            out->as.character = source->as.character;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_HOST_STRING:
            if (source->as.string.size != 0u &&
                source->as.string.data == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            out->as.string.data = source->as.string.data;
            out->as.string.size = source->as.string.size;
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_HOST_LIST:
        case TURBOWASM_COMPONENT_HOST_RECORD:
        case TURBOWASM_COMPONENT_HOST_TUPLE: {
            const turbowasm_component_host_sequence *input = public_sequence(source);
            turbowasm_component_value_list *sequence = internal_sequence(out);
            if (input->count != 0u && input->items == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            if (input->count > SIZE_MAX / sizeof(*sequence->items))
                return TURBOWASM_OUT_OF_MEMORY;
            if (input->count != 0u) {
                sequence->items = (turbowasm_component_value *)
                    turbowasm_rt_calloc(input->count, sizeof(*sequence->items));
                if (sequence->items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            sequence->count = input->count;
            for (i = 0u; i < input->count; ++i) {
                turbowasm_status status = public_to_internal(
                    &input->items[i], &sequence->items[i], depth + 1u, admission);
                if (status != TURBOWASM_OK) {
                    internal_input_destroy(out);
                    return status;
                }
            }
            return TURBOWASM_OK;
        }
        case TURBOWASM_COMPONENT_HOST_VARIANT:
        case TURBOWASM_COMPONENT_HOST_OPTION:
        case TURBOWASM_COMPONENT_HOST_RESULT: {
            const turbowasm_component_host_variant *input = public_variant(source);
            turbowasm_component_value_variant *variant = internal_variant(out);
            variant->case_index = input->case_index;
            if (input->payload != NULL) {
                turbowasm_status status;
                variant->payload = (turbowasm_component_value *)
                    turbowasm_rt_calloc(1u, sizeof(*variant->payload));
                if (variant->payload == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
                status = public_to_internal(input->payload,
                    variant->payload, depth + 1u, admission);
                if (status != TURBOWASM_OK) {
                    internal_input_destroy(out);
                    return status;
                }
            }
            return TURBOWASM_OK;
        }
        case TURBOWASM_COMPONENT_HOST_ENUM:
            out->as.enum_index = source->as.enum_index;
            return TURBOWASM_OK;
        case TURBOWASM_COMPONENT_HOST_FLAGS:
            if (source->as.flags.word_count != COMPONENT_PUBLIC_FLAGS_WORDS ||
                source->as.flags.words == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            out->as.flags = source->as.flags.words[0];
            return TURBOWASM_OK;

        case TURBOWASM_COMPONENT_HOST_OWN:
        case TURBOWASM_COMPONENT_HOST_BORROW:
            return convert_resource_argument(admission, source, out);

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status prepare_public_arguments(
    component_public_admission *admission,
    const turbowasm_component_core_call_adapter *adapter,
    const turbowasm_component_host_value *arguments, size_t count,
    turbowasm_component_value **out_arguments) {
    const turbowasm_component_type *function = turbowasm_component_type_graph_get(
        adapter->graph, adapter->function_type);
    turbowasm_component_value *values;
    turbowasm_status status;
    size_t i;
    if (function == NULL || count != function->as.function.param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < count; ++i) {
        status = visit_public_resources(&arguments[i], 0u,
            count_resource_argument, admission);
        if (status != TURBOWASM_OK)
            return status;
    }
    if (admission->capacity != 0u) {
        admission->entries = turbowasm_rt_calloc(admission->capacity, sizeof(*admission->entries));
        if (admission->entries == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }
    if (count == 0u)
        return TURBOWASM_OK;
    values = turbowasm_rt_calloc(count, sizeof(*values));
    if (values == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    *out_arguments = values;
    for (i = 0u; i < count; ++i) {
        status = public_to_internal(&arguments[i], &values[i], 0u, admission);
        if (status != TURBOWASM_OK)
            return status;
        status = turbowasm_component_canonical_validate_value(
            adapter->graph, function->as.function.params[i], &values[i]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return reserve_resource_arguments(admission);
}

static turbowasm_status internal_to_public(
    turbowasm_component_value *source,
    turbowasm_component_host_value *out,
    uint32_t depth) {
    uint64_t i;

    if (source == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= COMPONENT_PUBLIC_MAX_DEPTH)
        return TURBOWASM_TRAPPED;
    if (!public_kind_supported((turbowasm_component_host_value_kind)source->kind))
        return TURBOWASM_UNSUPPORTED;

    memset(out, 0, sizeof(*out));
    out->kind =
        (turbowasm_component_host_value_kind)source->kind;

    switch (source->kind) {
        case TURBOWASM_COMPONENT_TYPE_BOOL:
            out->as.boolean = source->as.boolean;
            break;
        case TURBOWASM_COMPONENT_TYPE_S8:
            out->as.s8 = source->as.s8;
            break;
        case TURBOWASM_COMPONENT_TYPE_U8:
            out->as.u8 = source->as.u8;
            break;
        case TURBOWASM_COMPONENT_TYPE_S16:
            out->as.s16 = source->as.s16;
            break;
        case TURBOWASM_COMPONENT_TYPE_U16:
            out->as.u16 = source->as.u16;
            break;
        case TURBOWASM_COMPONENT_TYPE_S32:
            out->as.s32 = source->as.s32;
            break;
        case TURBOWASM_COMPONENT_TYPE_U32:
            out->as.u32 = source->as.u32;
            break;
        case TURBOWASM_COMPONENT_TYPE_S64:
            out->as.s64 = source->as.s64;
            break;
        case TURBOWASM_COMPONENT_TYPE_U64:
            out->as.u64 = source->as.u64;
            break;
        case TURBOWASM_COMPONENT_TYPE_F32:
            out->as.f32 = source->as.f32;
            break;
        case TURBOWASM_COMPONENT_TYPE_F64:
            out->as.f64 = source->as.f64;
            break;
        case TURBOWASM_COMPONENT_TYPE_CHAR:
            out->as.character = source->as.character;
            break;

        case TURBOWASM_COMPONENT_TYPE_STRING:
            out->as.string.data = source->as.string.data;
            out->as.string.size = source->as.string.size;
            source->as.string.data = NULL;
            source->as.string.size = 0u;
            break;

        case TURBOWASM_COMPONENT_TYPE_LIST:
        case TURBOWASM_COMPONENT_TYPE_RECORD:
        case TURBOWASM_COMPONENT_TYPE_TUPLE: {
            turbowasm_component_value_list *input = internal_sequence(source);
            turbowasm_component_host_sequence sequence = {0};
            if (input->count > SIZE_MAX / sizeof(*sequence.items))
                return TURBOWASM_OUT_OF_MEMORY;
            if (input->count != 0u) {
                sequence.items = (turbowasm_component_host_value *)
                    turbowasm_rt_calloc((size_t)input->count, sizeof(*sequence.items));
                if (sequence.items == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
            }
            sequence.count = (size_t)input->count;
            switch (out->kind) {
                case TURBOWASM_COMPONENT_HOST_LIST: out->as.list = sequence; break;
                case TURBOWASM_COMPONENT_HOST_RECORD: out->as.record = sequence; break;
                default: out->as.tuple = sequence; break;
            }
            for (i = 0u; i < input->count; ++i) {
                turbowasm_status status = internal_to_public(
                    &input->items[i], &sequence.items[i], depth + 1u);
                if (status != TURBOWASM_OK) {
                    turbowasm_component_host_value_destroy(out);
                    return status;
                }
            }
            turbowasm_rt_free(input->items);
            input->items = NULL;
            input->count = 0u;
            break;
        }
        case TURBOWASM_COMPONENT_TYPE_VARIANT:
        case TURBOWASM_COMPONENT_TYPE_OPTION:
        case TURBOWASM_COMPONENT_TYPE_RESULT: {
            turbowasm_component_value_variant *input = internal_variant(source);
            turbowasm_component_host_variant variant = {0};
            variant.case_index = input->case_index;
            if (input->payload != NULL) {
                turbowasm_status status;
                variant.payload = (turbowasm_component_host_value *)
                    turbowasm_rt_calloc(1u, sizeof(*variant.payload));
                if (variant.payload == NULL)
                    return TURBOWASM_OUT_OF_MEMORY;
                status = internal_to_public(input->payload,
                    variant.payload, depth + 1u);
                if (status != TURBOWASM_OK) {
                    turbowasm_component_host_value_destroy(variant.payload);
                    turbowasm_rt_free(variant.payload);
                    return status;
                }
                turbowasm_rt_free(input->payload);
                input->payload = NULL;
            }
            switch (out->kind) {
                case TURBOWASM_COMPONENT_HOST_VARIANT: out->as.variant = variant; break;
                case TURBOWASM_COMPONENT_HOST_OPTION: out->as.option = variant; break;
                default: out->as.result = variant; break;
            }
            break;
        }
        case TURBOWASM_COMPONENT_TYPE_ENUM:
            out->as.enum_index = source->as.enum_index;
            break;
        case TURBOWASM_COMPONENT_TYPE_FLAGS:
            out->as.flags.words = (uint32_t *)turbowasm_rt_calloc(
                COMPONENT_PUBLIC_FLAGS_WORDS, sizeof(*out->as.flags.words));
            if (out->as.flags.words == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
            out->as.flags.words[0] = source->as.flags;
            out->as.flags.word_count = COMPONENT_PUBLIC_FLAGS_WORDS;
            break;

        case TURBOWASM_COMPONENT_TYPE_OWN:
            if (source->release != resource_owner_release || source->release_context == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            out->as.own = source->release_context;
            break;

        default:
            return TURBOWASM_UNSUPPORTED;
    }

    memset(source, 0, sizeof(*source));
    return TURBOWASM_OK;
}

static turbowasm_status check_destroy_owner(
    const turbowasm_component_host_value *value, void *context) {
    (void)context;
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN &&
        (value->as.own == NULL || value->as.own->loans != 0u || value->as.own->busy))
        return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_OK;
}

static turbowasm_status mark_destroy_owner(
    const turbowasm_component_host_value *value, void *context) {
    bool lock = *(const bool *)context;
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN) {
        if (lock && value->as.own->busy)
            return TURBOWASM_INVALID_ARGUMENT;
        value->as.own->busy = lock;
    }
    return TURBOWASM_OK;
}

static turbowasm_status destroy_public_value(turbowasm_component_host_value *value) {
    turbowasm_component_host_value owned;
    const turbowasm_component_host_sequence *sequence;
    const turbowasm_component_host_variant *variant;
    turbowasm_status first = TURBOWASM_OK;
    size_t i;
    if (value == NULL)
        return TURBOWASM_OK;
    owned = *value;
    memset(value, 0, sizeof(*value));
    sequence = public_sequence(&owned);
    variant = public_variant(&owned);
    if (sequence != NULL) {
        for (i = 0u; i < sequence->count; ++i) {
            turbowasm_status status = destroy_public_value(&sequence->items[i]);
            if (first == TURBOWASM_OK)
                first = status;
        }
        turbowasm_rt_free(sequence->items);
    } else if (variant != NULL) {
        first = destroy_public_value(variant->payload);
        turbowasm_rt_free(variant->payload);
    } else if (owned.kind == TURBOWASM_COMPONENT_HOST_STRING) {
        turbowasm_rt_free(owned.as.string.data);
    } else if (owned.kind == TURBOWASM_COMPONENT_HOST_FLAGS) {
        turbowasm_rt_free(owned.as.flags.words);
    } else if (owned.kind == TURBOWASM_COMPONENT_HOST_OWN) {
        first = resource_owner_release(owned.as.own);
    }
    return first;
}

turbowasm_status turbowasm_component_host_value_destroy(
    turbowasm_component_host_value *value) {
    bool lock = true;
    turbowasm_status status = visit_public_resources(value, 0u, check_destroy_owner, NULL);
    if (status != TURBOWASM_OK)
        return status;
    status = visit_public_resources(value, 0u, mark_destroy_owner, &lock);
    if (status != TURBOWASM_OK) {
        lock = false;
        (void)visit_public_resources(value, 0u, mark_destroy_owner, &lock);
        return status;
    }
    return destroy_public_value(value);
}

turbowasm_status turbowasm_component_host_value_borrow(
    const turbowasm_component_host_value *source,
    turbowasm_component_host_value *out_borrow) {
    if (source == NULL || out_borrow == NULL || source == out_borrow ||
        (int)out_borrow->kind != 0 || source->kind != TURBOWASM_COMPONENT_HOST_OWN ||
        source->as.own == NULL || source->as.own->busy)
        return TURBOWASM_INVALID_ARGUMENT;
    out_borrow->kind = TURBOWASM_COMPONENT_HOST_BORROW;
    out_borrow->as.borrow = source->as.own;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_load_borrowed(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size) {
    return turbowasm_component_load_borrowed_with_config(
        component, bytes, size, NULL);
}

turbowasm_status turbowasm_component_load_borrowed_with_config(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_component_public_impl *impl;
    turbowasm_status status;

    if (component == NULL || bytes == NULL ||
        component->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(&normalized);
    impl = (turbowasm_component_public_impl *)turbowasm_rt_calloc(
        1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    status = turbowasm_component_binary_load_with_config(
        &impl->binary, bytes, size, &normalized);
    if (status != TURBOWASM_OK) {
        scope = turbowasm_runtime_scope_enter(&normalized);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        return status;
    }

    impl->ref_count = 1u;
    component->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_component_destroy(
    turbowasm_component *component) {
    turbowasm_component_public_impl *impl;

    if (component == NULL)
        return;
    impl = turbowasm_component_public_impl_get(component);
    component->impl = NULL;
    turbowasm_component_public_impl_release(impl);
}

turbowasm_status turbowasm_component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component) {
    turbowasm_component_public_impl *component_state;
    turbowasm_component_instance_public_impl *impl;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (instance == NULL || component == NULL ||
        instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    component_state = turbowasm_component_public_impl_get(component);
    if (component_state == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(
        &component_state->binary.config);
    impl = (turbowasm_component_instance_public_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if (!turbowasm_component_public_impl_retain(
            component_state)) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    impl->component = component_state;

    status = turbowasm_component_exec_init(
        &impl->exec, &component_state->binary);
    if (status != TURBOWASM_OK) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        turbowasm_component_public_impl_release(component_state);
        return status;
    }

    impl->ref_count = 1u;
    instance->impl = impl;
    return TURBOWASM_OK;
}

static bool component_instance_retain(
    turbowasm_component_instance_public_impl *impl) {
    if (impl == NULL || impl->ref_count == 0u || impl->ref_count == UINT32_MAX)
        return false;
    ++impl->ref_count;
    return true;
}

static void component_instance_release(
    turbowasm_component_instance_public_impl *impl) {
    turbowasm_component_public_impl *component_state;
    turbowasm_runtime_config config;
    turbowasm_runtime_scope scope;

    if (impl == NULL || impl->ref_count == 0u)
        return;
    if (--impl->ref_count != 0u)
        return;

    component_state = impl->component;
    config = component_state->binary.config;

    turbowasm_component_exec_destroy(&impl->exec);

    if (impl->owner_release != NULL)
        impl->owner_release(impl->owner_context);

    scope = turbowasm_runtime_scope_enter(&config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);

    turbowasm_component_public_impl_release(component_state);
}

void turbowasm_component_instance_destroy(
    turbowasm_component_instance *instance) {
    turbowasm_component_instance_public_impl *impl;
    if (instance == NULL)
        return;
    impl = turbowasm_component_instance_public_impl_get(instance);
    instance->impl = NULL;
    component_instance_release(impl);
}

static bool public_export_result_type(
    const turbowasm_component_instance_public_impl *instance,
    turbowasm_name export_name,
    bool *out_has_result,
    turbowasm_component_core_call_adapter *out_adapter) {
    const turbowasm_component_binary *binary;
    uint32_t i;

    if (instance == NULL || out_has_result == NULL)
        return false;

    binary = instance->exec.binary;
    if (binary == NULL)
        return false;

    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &binary->exports[i];
        uint32_t adapter_index;
        const turbowasm_component_core_call_adapter *adapter;
        const turbowasm_component_type *function;

        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->name.size != export_name.size ||
            (export_name.size != 0u &&
             (export_name.bytes == NULL ||
              memcmp(
                  export_desc->name.bytes,
                  export_name.bytes,
                  export_name.size) != 0)))
            continue;

        if (export_desc->item_index >=
                instance->exec.function_count ||
            instance->exec.function_adapter_indices == NULL)
            return false;

        adapter_index =
            instance->exec.function_adapter_indices[
                export_desc->item_index];
        if (adapter_index == UINT32_MAX ||
            adapter_index >= instance->exec.adapter_count)
            return false;

        adapter = &instance->exec.functions[adapter_index];
        function = turbowasm_component_type_graph_get(
            adapter->graph, adapter->function_type);
        if (function == NULL ||
            function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION)
            return false;

        {
            uint32_t param_index;
            for (param_index = 0u;
                 param_index < function->as.function.param_count;
                 ++param_index) {
                if (!public_type_ref_supported(
                        adapter->graph,
                        function->as.function.params[param_index]))
                    return false;
            }
        }

        if (function->as.function.has_result &&
            !public_type_ref_supported(
                adapter->graph,
                function->as.function.result))
            return false;

        *out_adapter = *adapter;
        *out_has_result = function->as.function.has_result;
        return true;
    }

    return false;
}

static turbowasm_status component_instance_invoke(
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count,
    turbowasm_component_host_value *result,
    size_t result_capacity,
    size_t *out_result_count,
    turbowasm_trap *trap, bool move) {
    turbowasm_component_instance_public_impl *impl;
    turbowasm_component_value *internal_arguments = NULL;
    turbowasm_component_value internal_result = {0};
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    bool has_result;
    component_public_admission admission = {0};
    turbowasm_component_core_call_adapter adapter;
    turbowasm_component_host_value public_result = {0};
    size_t i;

    if (instance == NULL || out_result_count == NULL ||
        trap == NULL ||
        (export_name.size != 0u && export_name.bytes == NULL) ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    impl = turbowasm_component_instance_public_impl_get(instance);
    if (impl == NULL || impl->component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!public_export_result_type(
            impl, export_name, &has_result, &adapter))
        return TURBOWASM_UNSUPPORTED;

    if (has_result) {
        if (result == NULL || result_capacity < 1u)
            return TURBOWASM_INVALID_ARGUMENT;
    }

    *out_result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    if (!component_instance_retain(impl))
        return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(
        &impl->component->binary.config);

    admission.instance = impl;
    admission.move = move;
    adapter.result_owner = acquire_result_owner;
    adapter.result_owner_context = impl;
    adapter.admission_commit = admission_commit_and_start;
    adapter.admission_context = &admission;
    status = prepare_public_arguments(&admission, &adapter, arguments,
        argument_count, &internal_arguments);
    if (status != TURBOWASM_OK)
        goto done;
    status = turbowasm_component_core_call_invoke(&adapter, internal_arguments,
        argument_count, has_result ? &internal_result : NULL, trap);
    if (status != TURBOWASM_OK)
        goto done;

    if (has_result) {
        status = internal_to_public(
            &internal_result, &public_result, 0u);
        if (status != TURBOWASM_OK)
            goto done;
        *result = public_result;
        memset(&public_result, 0, sizeof(public_result));
        *out_result_count = 1u;
    }

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i]);
        turbowasm_rt_free(internal_arguments);
    }
    if (internal_result.kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(&internal_result);

    turbowasm_component_host_value_destroy(&public_result);
    admission_destroy(&admission);
    turbowasm_runtime_scope_leave(scope);
    component_instance_release(impl);
    return status;
}

turbowasm_status turbowasm_component_instance_invoke(
    turbowasm_component_instance *instance, turbowasm_name export_name,
    const turbowasm_component_host_value *arguments, size_t argument_count,
    turbowasm_component_host_value *result, size_t result_capacity,
    size_t *out_result_count, turbowasm_trap *trap) {
    return component_instance_invoke(instance, export_name, arguments, argument_count,
        result, result_capacity, out_result_count, trap, false);
}

turbowasm_status turbowasm_component_instance_invoke_move(
    turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_host_value *arguments, size_t argument_count,
    turbowasm_component_host_value *result, size_t result_capacity,
    size_t *out_result_count, turbowasm_trap *trap) {
    return component_instance_invoke(instance, export_name, arguments, argument_count,
        result, result_capacity, out_result_count, trap, true);
}


typedef struct turbowasm_component_call_public_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_exec_call call;
    turbowasm_component_core_call_adapter adapter;
    component_public_admission admission;
    bool has_result;
} turbowasm_component_call_public_impl;

static turbowasm_component_call_public_impl *
component_call_public_impl_mut(
    turbowasm_component_call *call) {
    return call != NULL
        ? (turbowasm_component_call_public_impl *)call->impl
        : NULL;
}

static const turbowasm_component_call_public_impl *
component_call_public_impl_get(
    const turbowasm_component_call *call) {
    return call != NULL
        ? (const turbowasm_component_call_public_impl *)call->impl
        : NULL;
}

static turbowasm_status component_call_create(
    turbowasm_component_call *call,
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count, bool move) {
    turbowasm_component_instance_public_impl *instance_impl;
    turbowasm_component_call_public_impl *call_impl = NULL;
    turbowasm_component_value *internal_arguments = NULL;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    bool has_result = false;
    turbowasm_component_core_call_adapter adapter;
    size_t i;

    if (call == NULL || call->impl != NULL ||
        instance == NULL ||
        (export_name.size != 0u && export_name.bytes == NULL) ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    instance_impl =
        turbowasm_component_instance_public_impl_get(instance);
    if (instance_impl == NULL || instance_impl->component == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!public_export_result_type(
            instance_impl, export_name, &has_result, &adapter))
        return TURBOWASM_UNSUPPORTED;

    if (!component_instance_retain(instance_impl))
        return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(
        &instance_impl->component->binary.config);

    call_impl =
        (turbowasm_component_call_public_impl *)
            turbowasm_rt_calloc(1u, sizeof(*call_impl));
    if (call_impl == NULL) {
        status = TURBOWASM_OUT_OF_MEMORY;
        goto done;
    }

    call_impl->adapter = adapter;
    call_impl->adapter.result_owner = acquire_result_owner;
    call_impl->adapter.result_owner_context = instance_impl;
    call_impl->admission.instance = instance_impl;
    call_impl->admission.move = move;
    status = prepare_public_arguments(&call_impl->admission, &call_impl->adapter,
        arguments, argument_count, &internal_arguments);
    if (status != TURBOWASM_OK)
        goto done;
    status = turbowasm_component_core_execution_create(&call_impl->call.core,
        &call_impl->adapter, internal_arguments, argument_count);
    if (status != TURBOWASM_OK)
        goto done;

    call_impl->call.initialized = true;
    admission_commit(&call_impl->admission);
    call_impl->instance = instance_impl;
    call_impl->has_result = has_result;
    call->impl = call_impl;
    call_impl = NULL;

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i]);
        turbowasm_rt_free(internal_arguments);
    }
    if (call_impl != NULL) {
        turbowasm_component_exec_call_destroy(
            &call_impl->call);
        admission_destroy(&call_impl->admission);
        turbowasm_rt_free(call_impl);
    }
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK)
        component_instance_release(instance_impl);
    return status;
}

turbowasm_status turbowasm_component_call_create(
    turbowasm_component_call *call, turbowasm_component_instance *instance,
    turbowasm_name export_name, const turbowasm_component_host_value *arguments,
    size_t argument_count) {
    return component_call_create(call, instance, export_name, arguments, argument_count, false);
}

turbowasm_status turbowasm_component_call_create_move(
    turbowasm_component_call *call, turbowasm_component_instance *instance,
    turbowasm_name export_name, turbowasm_component_host_value *arguments,
    size_t argument_count) {
    return component_call_create(call, instance, export_name, arguments, argument_count, true);
}

void turbowasm_component_call_destroy(
    turbowasm_component_call *call) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);
    turbowasm_component_instance_public_impl *instance;
    turbowasm_runtime_scope scope;

    if (impl == NULL)
        return;
    instance = impl->instance;
    call->impl = NULL;
    scope = turbowasm_runtime_scope_enter(
        &instance->component->binary.config);
    turbowasm_component_exec_call_destroy(&impl->call);
    admission_destroy(&impl->admission);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    component_instance_release(instance);
}

turbowasm_status turbowasm_component_call_resume(
    turbowasm_component_call *call,
    const turbowasm_execution_options *options) {
    turbowasm_component_call_public_impl *impl = component_call_public_impl_mut(call);
    turbowasm_status status;
    turbowasm_execution_state state;
    turbowasm_runtime_scope scope;
    if (impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    state = turbowasm_component_exec_call_state_get(&impl->call);
    if (state != TURBOWASM_EXECUTION_READY && state != TURBOWASM_EXECUTION_YIELDED)
        return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(&impl->instance->component->binary.config);
    admission_start(&impl->admission);
    status = turbowasm_component_exec_call_resume(&impl->call, options);
    if (status != TURBOWASM_YIELDED)
        admission_destroy(&impl->admission);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

turbowasm_execution_state turbowasm_component_call_state_get(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_state_get(
              &impl->call)
        : TURBOWASM_EXECUTION_FAILED;
}

turbowasm_yield_reason turbowasm_component_call_yield_reason_get(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_yield_reason_get(
              &impl->call)
        : TURBOWASM_YIELD_NONE;
}

bool turbowasm_component_call_pending_host_wait(
    const turbowasm_component_call *call,
    turbowasm_host_wait *out_wait) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL &&
        turbowasm_component_exec_call_pending_host_wait(
            &impl->call, out_wait);
}

turbowasm_status turbowasm_component_call_complete_host_wait(
    turbowasm_component_call *call,
    turbowasm_host_wait wait,
    int status) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);

    if (impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_component_exec_call_complete_host_wait(
        &impl->call, wait, status);
}

turbowasm_status turbowasm_component_call_terminal_status(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_terminal_status(
              &impl->call)
        : TURBOWASM_INVALID_ARGUMENT;
}

turbowasm_trap turbowasm_component_call_trap(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_trap(
              &impl->call)
        : TURBOWASM_TRAP_NONE;
}

size_t turbowasm_component_call_result_count(
    const turbowasm_component_call *call) {
    const turbowasm_component_call_public_impl *impl =
        component_call_public_impl_get(call);

    return impl != NULL
        ? turbowasm_component_exec_call_result_count(
              &impl->call)
        : 0u;
}

turbowasm_status turbowasm_component_call_take_result(
    turbowasm_component_call *call,
    turbowasm_component_host_value *out_result) {
    turbowasm_component_call_public_impl *impl =
        component_call_public_impl_mut(call);
    turbowasm_component_value internal_result = {0};
    turbowasm_status status;
    turbowasm_runtime_scope scope;

    if (impl == NULL || out_result == NULL ||
        !impl->has_result ||
        turbowasm_component_exec_call_result_count(
            &impl->call) != 1u)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_exec_call_take_result(
        &impl->call, &internal_result);
    if (status != TURBOWASM_OK)
        return status;

    /* A repeated take must not erase an already-owned caller result. */
    memset(out_result, 0, sizeof(*out_result));
    scope = turbowasm_runtime_scope_enter(
        &impl->instance->component->binary.config);
    status = internal_to_public(
        &internal_result, out_result, 0u);
    if (internal_result.kind !=
        TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(
            &internal_result);
    if (status != TURBOWASM_OK)
        turbowasm_component_host_value_destroy(
            out_result);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
