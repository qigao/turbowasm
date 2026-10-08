#include <turbowasm/component.h>

#include "component_api_internal.h"
#include "component_endpoint_builtin.h"
#include "component_host_endpoint_internal.h"
#include "component_type_graph.h"
#include "runtime_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool component_instance_retain(turbowasm_component_instance_public_impl *impl);
static void component_instance_release(turbowasm_component_instance_public_impl *impl);
static turbowasm_status destroy_host_value(turbowasm_component_host_value *value, bool internal);
static bool pair_instance_retain(void *owner) { return component_instance_retain(owner); }
static void pair_instance_release(void *owner) { component_instance_release(owner); }

struct turbowasm_component_host_resource {
    turbowasm_component_instance_public_impl *instance;
    struct turbowasm_component_host_resource *next;
    uint64_t identity;
    turbowasm_value rep;
    uint32_t loans;
    bool busy;
    bool external;
    turbowasm_component_value canonical;
};

typedef struct component_resource_argument {
    const turbowasm_component_host_value *source;
    turbowasm_component_host_resource *owner;
    turbowasm_component_value *value;
    turbowasm_component_host_endpoint *endpoint;
    struct component_public_admission *admission;
    bool own, promoted, published;
    bool reserved;
    void (*transfer_finish)(void *, bool);
    void *transfer_context;
} component_resource_argument;

typedef struct component_public_admission {
    turbowasm_component_instance_public_impl *instance;
    component_resource_argument *entries;
    size_t count, capacity;
    bool move, committed, started, copy_strings, async_values, has_endpoints, transfer;
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
TW_COMPONENT_PUBLIC_KIND_MATCH(TURBOWASM_COMPONENT_HOST_FUTURE, TURBOWASM_COMPONENT_TYPE_FUTURE);
TW_COMPONENT_PUBLIC_KIND_MATCH(TURBOWASM_COMPONENT_HOST_STREAM, TURBOWASM_COMPONENT_TYPE_STREAM);
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
        kind == TURBOWASM_COMPONENT_HOST_OWN || kind == TURBOWASM_COMPONENT_HOST_BORROW ||
        kind == TURBOWASM_COMPONENT_HOST_FUTURE || kind == TURBOWASM_COMPONENT_HOST_STREAM;
}

static turbowasm_component_host_endpoint *public_endpoint(const turbowasm_component_host_value *value) {
    switch (value->kind) {
        case TURBOWASM_COMPONENT_HOST_FUTURE: return (turbowasm_component_host_endpoint *)&value->as.future;
        case TURBOWASM_COMPONENT_HOST_STREAM: return (turbowasm_component_host_endpoint *)&value->as.stream;
        default: return NULL;
    }
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
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN || value->kind == TURBOWASM_COMPONENT_HOST_BORROW ||
        public_endpoint(value) != NULL)
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
    if (owner->canonical.kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_exec_resource_value_disown(&owner->canonical);
    turbowasm_rt_free(owner);
    component_instance_release(instance);
}

static turbowasm_status resource_owner_release(void *context) {
    turbowasm_component_host_resource *owner = context;
    turbowasm_status status;
    if (owner == NULL || owner->loans != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    owner->busy = true;
    status = owner->canonical.kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED
        ? turbowasm_component_value_destroy(&owner->canonical)
        : turbowasm_component_exec_resource_release(&owner->instance->exec, owner->identity, owner->rep);
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
    if (admission->transfer && value->kind == TURBOWASM_COMPONENT_HOST_BORROW) return TURBOWASM_TYPE_MISMATCH;
    if (public_endpoint(value) != NULL) admission->has_endpoints = true;
    if ((value->kind == TURBOWASM_COMPONENT_HOST_OWN || public_endpoint(value) != NULL) && !admission->move)
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
    entry->value = out; entry->admission = admission;
    out->as.resource_rep = owner->rep;
    out->resource_identity = owner->identity;
    return TURBOWASM_OK;
}

static turbowasm_status reserve_resource_arguments(component_public_admission *admission) {
    size_t i;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->endpoint != NULL) continue;
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
    /* All leaves are frozen. No callback may intervene between admission and
     * clearing every resource/endpoint source carrier. */
    admission->committed = true;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->endpoint != NULL)
            turbowasm_component_host_endpoint_move_publish(entry->endpoint);
        if (entry->own) {
            /* Only move entry points can place an own leaf in this transaction. */
            memset((turbowasm_component_host_value *)entry->source, 0, sizeof(*entry->source));
        }
        entry->source = NULL;
    }
}

static void admission_start(component_public_admission *admission) {
    size_t i;
    if (admission->started)
        return;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->endpoint == NULL && entry->own && entry->owner != NULL && (!entry->promoted || entry->published)) {
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

static turbowasm_status admission_destroy(component_public_admission *admission) {
    size_t i;
    turbowasm_status first = TURBOWASM_OK;
    for (i = 0u; i < admission->count; ++i) {
        component_resource_argument *entry = &admission->entries[i];
        if (entry->endpoint != NULL || !entry->reserved || entry->owner == NULL)
            continue;
        if (!entry->own)
            --entry->owner->loans;
        else if (admission->committed) {
            turbowasm_status status = resource_owner_release(entry->owner);
            if (first == TURBOWASM_OK) first = status;
        }
        else
            entry->owner->busy = false;
    }
    turbowasm_rt_free(admission->entries);
    memset(admission, 0, sizeof(*admission));
    return first;
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

static turbowasm_status internal_input_destroy(
    turbowasm_component_value *value, bool strings_owned) {
    uint64_t i;
    turbowasm_status first = TURBOWASM_OK;

    if (value == NULL)
        return TURBOWASM_OK;

    {
        turbowasm_component_value_list *sequence = internal_sequence(value);
        turbowasm_component_value_variant *variant = internal_variant(value);
        if (sequence != NULL) {
            for (i = 0u; i < sequence->count; ++i) {
                turbowasm_status status = internal_input_destroy(&sequence->items[i], strings_owned);
                if (first == TURBOWASM_OK) first = status;
            }
            turbowasm_rt_free(sequence->items);
        } else if (variant != NULL && variant->payload != NULL) {
            first = internal_input_destroy(variant->payload, strings_owned);
            turbowasm_rt_free(variant->payload);
        } else if (strings_owned && value->kind == TURBOWASM_COMPONENT_TYPE_STRING) {
            turbowasm_rt_free(value->as.string.data);
        } else if (value->release != NULL && (value->kind == TURBOWASM_COMPONENT_TYPE_OWN ||
            value->kind == TURBOWASM_COMPONENT_TYPE_BORROW || value->kind == TURBOWASM_COMPONENT_TYPE_FUTURE ||
            value->kind == TURBOWASM_COMPONENT_TYPE_STREAM)) {
            first = turbowasm_component_value_destroy(value);
        }
    }

    /*
     * Resource obligations belong to the admission transaction. Only deferred
     * inputs own their strings; synchronous inputs borrow caller bytes.
     */
    memset(value, 0, sizeof(*value));
    return first;
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
            if (admission->copy_strings) {
                if (source->as.string.size != 0u) {
                    out->as.string.data = turbowasm_rt_malloc(source->as.string.size);
                    if (out->as.string.data == NULL) return TURBOWASM_OUT_OF_MEMORY;
                    memcpy(out->as.string.data, source->as.string.data, source->as.string.size);
                }
            } else out->as.string.data = source->as.string.data;
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
                    /* Admission entries borrow these leaf addresses. Keep the
                     * partial tree stable until the enclosing owner unwinds. */
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

        case TURBOWASM_COMPONENT_HOST_FUTURE:
        case TURBOWASM_COMPONENT_HOST_STREAM: {
            component_resource_argument *entry;
            turbowasm_status status;
            const turbowasm_component_endpoint *end = turbowasm_component_host_endpoint_view(public_endpoint(source));
            if (!admission->async_values || !admission->move || admission->count >= admission->capacity)
                return TURBOWASM_INVALID_ARGUMENT;
            if (end == NULL || !end->readable ||
                end->waitable.state.endpoint.future != (source->kind == TURBOWASM_COMPONENT_HOST_FUTURE))
                return TURBOWASM_INVALID_ARGUMENT;
            entry = &admission->entries[admission->count];
            entry->endpoint = public_endpoint(source); entry->source = source;
            entry->own = true; entry->value = out; entry->admission = admission;
            memset(out, 0, sizeof(*out));
            status = turbowasm_component_host_endpoint_move_prepare(entry->endpoint, out, &admission->committed);
            if (status == TURBOWASM_OK) ++admission->count;
            return status;
        }

        default:
            return TURBOWASM_UNSUPPORTED;
    }
}

static turbowasm_status prepare_public_values(
    component_public_admission *admission,
    const turbowasm_component_type_graph *graph, const turbowasm_component_type_ref *types, bool repeated,
    const turbowasm_component_host_value *arguments, size_t count,
    turbowasm_component_value **out_arguments) {
    turbowasm_component_value *values;
    turbowasm_status status;
    size_t i;
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
            graph, types[repeated ? 0u : i], &values[i]);
        if (status != TURBOWASM_OK)
            return status;
    }
    return reserve_resource_arguments(admission);
}

static turbowasm_status prepare_public_arguments(component_public_admission *admission,
    const turbowasm_component_core_call_adapter *adapter, const turbowasm_component_host_value *arguments,
    size_t count, turbowasm_component_value **out) {
    const turbowasm_component_type *function = turbowasm_component_type_graph_get(adapter->graph, adapter->function_type);
    if (function == NULL || count != function->as.function.param_count) return TURBOWASM_INVALID_ARGUMENT;
    return prepare_public_values(admission, adapter->graph, function->as.function.params, false, arguments, count, out);
}

typedef struct component_host_arguments_impl {
    component_public_admission admission;
    turbowasm_component_value *values;
    size_t count, bytes;
    turbowasm_component_host_budget *budget;
    turbowasm_component_host_registration registration;
} component_host_arguments_impl;

static turbowasm_status argument_size_add(size_t *size, size_t count,
    size_t stride, size_t limit) {
    if (*size > limit || count > (limit - *size) / stride)
        return TURBOWASM_OUT_OF_MEMORY;
    *size += count * stride;
    return TURBOWASM_OK;
}

/* Account only allocations owned by admission. Owner objects and the instance
 * already exist; the snapshot charges its reservation records, not their reps. */
static turbowasm_status argument_storage_size(
    const turbowasm_component_host_value *value, uint32_t depth,
    size_t *size, size_t limit, bool async_resources) {
    const turbowasm_component_host_sequence *sequence;
    const turbowasm_component_host_variant *variant;
    turbowasm_status status;
    size_t i;
    if (depth >= COMPONENT_PUBLIC_MAX_DEPTH) return TURBOWASM_TRAPPED;
    if (!public_kind_supported(value->kind)) return TURBOWASM_INVALID_ARGUMENT;
    sequence = public_sequence(value);
    variant = public_variant(value);
    if (sequence != NULL) {
        if (sequence->count != 0u && sequence->items == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        status = argument_size_add(size, sequence->count,
            sizeof(turbowasm_component_value), limit);
        if (status != TURBOWASM_OK) return status;
        for (i = 0u; i < sequence->count; ++i) {
            status = argument_storage_size(&sequence->items[i], depth + 1u, size, limit, async_resources);
            if (status != TURBOWASM_OK) return status;
        }
    } else if (variant != NULL && variant->payload != NULL) {
        status = argument_size_add(size, 1u, sizeof(turbowasm_component_value), limit);
        if (status != TURBOWASM_OK) return status;
        return argument_storage_size(variant->payload, depth + 1u, size, limit, async_resources);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_STRING) {
        if (value->as.string.size != 0u && value->as.string.data == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        return argument_size_add(size, value->as.string.size, 1u, limit);
    } else if (public_endpoint(value) != NULL) {
        if (!async_resources) return TURBOWASM_INVALID_ARGUMENT;
        status = argument_size_add(size, 1u, sizeof(component_resource_argument), limit);
        if (status == TURBOWASM_OK)
            status = argument_size_add(size, 1u, sizeof(turbowasm_component_endpoint_value_owner), limit);
        return status;
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_OWN ||
        value->kind == TURBOWASM_COMPONENT_HOST_BORROW) {
        status = argument_size_add(size, 1u, sizeof(component_resource_argument), limit);
        if (status == TURBOWASM_OK && async_resources)
            status = argument_size_add(size, 1u, turbowasm_component_exec_resource_adopt_size(), limit);
        return status;
    }
    return TURBOWASM_OK;
}

static turbowasm_status finish_host_resource(void *context, bool published) {
    component_resource_argument *entry = context;
    turbowasm_component_host_resource *owner = entry->owner;
    entry->promoted = false;
    if (!entry->own || owner == NULL || (!published && !entry->admission->committed)) return TURBOWASM_OK;
    /* The admission is retained through this finalizer. Detach before invoking
     * a destructor that can reenter the same instance. */
    entry->owner = NULL;
    if (published) {
        resource_owner_forget(owner);
        return TURBOWASM_OK;
    }
    return resource_owner_release(owner);
}

static void publish_host_resource(void *context) {
    component_resource_argument *entry = context;
    /* Resource handles commit before endpoint handles. Defer allocator cleanup
     * until host_arguments_published, after both codecs have published. */
    entry->published = true;
    if (!entry->admission->has_endpoints && entry->own && entry->owner != NULL) {
        turbowasm_component_host_resource *owner = entry->owner;
        entry->owner = NULL; resource_owner_forget(owner);
    }
}

static turbowasm_status prepare_host_values(
    turbowasm_component_host_arguments *owner,
    turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, const turbowasm_component_type_ref *types, bool repeated,
    const turbowasm_component_host_value *arguments, size_t count, bool move, bool async_resources,
    turbowasm_component_host_budget *budget) {
    component_host_arguments_impl *impl;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    size_t bytes = sizeof(*impl), i, available;
    if (owner == NULL || owner->impl != NULL || instance == NULL ||
        instance->component == NULL || !instance->exec.initialized || instance->admission_closed ||
        instance->shutdown_driving ||
        graph == NULL || (count != 0u && types == NULL) ||
        !turbowasm_component_host_budget_valid(instance, budget) || (count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (async_resources && instance->exec.task_domain.table == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    available = budget->limit - budget->used;
    status = argument_size_add(&bytes, count, sizeof(turbowasm_component_value), available);
    for (i = 0u; status == TURBOWASM_OK && i < count; ++i)
        status = argument_storage_size(&arguments[i], 0u, &bytes, available, async_resources);
    if (status != TURBOWASM_OK) return status;
    if (!component_instance_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(instance, true)) {
        component_instance_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    budget->used += bytes;
    scope = turbowasm_runtime_scope_enter(&instance->component->binary.config);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL) {
        budget->used -= bytes;
        turbowasm_runtime_scope_leave(scope);
        turbowasm_component_host_activity_leave(instance);
        component_instance_release(instance);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    impl->admission.instance = instance;
    impl->admission.move = move;
    impl->admission.copy_strings = true;
    impl->admission.async_values = async_resources;
    impl->admission.transfer = repeated;
    impl->count = count; impl->bytes = bytes; impl->budget = budget;
    status = prepare_public_values(&impl->admission, graph, types, repeated, arguments, count, &impl->values);
    if (status == TURBOWASM_OK && async_resources) {
        size_t resources = 0u;
        for (i = 0u; i < impl->admission.count; ++i)
            if (impl->admission.entries[i].endpoint == NULL) ++resources;
        if (instance->exec.async_resource_owners > instance->exec.resource_table.max_entries ||
            resources > instance->exec.resource_table.max_entries - instance->exec.async_resource_owners)
            status = TURBOWASM_OUT_OF_MEMORY;
        for (i = 0u; status == TURBOWASM_OK && i < impl->admission.count; ++i) {
            component_resource_argument *entry = &impl->admission.entries[i];
            if (entry->endpoint != NULL) continue;
            status = turbowasm_component_exec_resource_adopt(&instance->exec, entry->value,
                &impl->admission.committed, publish_host_resource, finish_host_resource, entry);
            if (status == TURBOWASM_OK) entry->promoted = true;
            if (status == TURBOWASM_OK && repeated)
                status = turbowasm_component_exec_resource_prepare_host_transfer(entry->value);
        }
    }
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK) {
        turbowasm_component_host_arguments failed = {impl};
        turbowasm_component_host_activity_leave(instance);
        (void)turbowasm_component_host_arguments_destroy(&failed);
        return status;
    }
    owner->impl = impl;
    turbowasm_component_host_register(instance, &impl->registration, impl, NULL, NULL);
    turbowasm_component_host_activity_leave(instance);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_arguments_prepare(turbowasm_component_host_arguments *owner,
    turbowasm_component_instance_public_impl *instance, const turbowasm_component_type_graph *graph,
    uint32_t function_type, const turbowasm_component_host_value *arguments, size_t count, bool move,
    bool async_resources, turbowasm_component_host_budget *budget) {
    const turbowasm_component_type *function = turbowasm_component_type_graph_get(graph, function_type);
    if (function == NULL || function->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION || count != function->as.function.param_count)
        return TURBOWASM_INVALID_ARGUMENT;
    return prepare_host_values(owner, instance, graph, function->as.function.params, false,
        arguments, count, move, async_resources, budget);
}
turbowasm_status turbowasm_component_host_payload_prepare(turbowasm_component_host_arguments *owner,
    turbowasm_component_instance_public_impl *instance, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref element, const turbowasm_component_host_value *values, uint32_t count,
    bool move, turbowasm_component_host_budget *budget) {
    return prepare_host_values(owner, instance, graph, &element, true, values, count, move, true, budget);
}
void turbowasm_component_host_payload_publish(turbowasm_component_host_arguments *owner) {
    component_host_arguments_impl *impl = owner->impl;
    size_t i;
    /* All canonical leaves become independent before allocator cleanup runs. */
    for (i = 0u; i < impl->admission.count; ++i) {
        component_resource_argument *entry = &impl->admission.entries[i];
        if (entry->endpoint != NULL) {
            turbowasm_component_endpoint_value_owner *record =
                ((turbowasm_component_endpoint *)turbowasm_component_endpoint_value_get(entry->value))->value_owner;
            entry->transfer_finish = record->host_finish; entry->transfer_context = record->host_context;
            if (record->host_begin != NULL) record->host_begin(record->host_context);
            record->host_admitted = NULL; record->host_begin = NULL;
            record->host_finish = NULL; record->host_context = NULL;
        } else {
            turbowasm_component_exec_resource_publish_host_transfer(entry->value);
            entry->published = true;
        }
    }
    for (i = 0u; i < impl->admission.count; ++i) {
        component_resource_argument *entry = &impl->admission.entries[i];
        if (entry->transfer_finish != NULL) {
            entry->transfer_finish(entry->transfer_context, true);
            entry->transfer_finish = NULL; entry->transfer_context = NULL;
        }
    }
    admission_start(&impl->admission);
}
void turbowasm_component_host_payload_storage_moved(turbowasm_component_host_arguments *owner, size_t bytes) {
    component_host_arguments_impl *impl = owner->impl;
    /* Publication detached every hook and retired every source owner. The
     * transfer now owns the nested cells; they may be promoted and freed before
     * this empty staging allocation is retired. Keep no references to them. */
    impl->admission.count = 0u;
    impl->bytes -= bytes; impl->budget->used -= bytes;
}

const turbowasm_component_value *turbowasm_component_host_arguments_values(
    const turbowasm_component_host_arguments *owner, size_t *count) {
    const component_host_arguments_impl *impl = owner != NULL ? owner->impl : NULL;
    if (count != NULL) *count = impl != NULL ? impl->count : 0u;
    return impl != NULL ? impl->values : NULL;
}

turbowasm_status turbowasm_component_host_arguments_commit(
    turbowasm_component_host_arguments *owner) {
    component_host_arguments_impl *impl = owner != NULL ? owner->impl : NULL;
    if (impl == NULL || impl->admission.committed) return TURBOWASM_INVALID_ARGUMENT;
    for (size_t i = 0u; i < impl->admission.count; ++i) {
        const component_resource_argument *entry = &impl->admission.entries[i];
        if (entry->endpoint != NULL && !turbowasm_component_host_endpoint_move_ready(
            entry->endpoint, entry->value, &impl->admission.committed)) return TURBOWASM_INVALID_ARGUMENT;
    }
    admission_commit(&impl->admission);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_arguments_published(
    turbowasm_component_host_arguments *owner) {
    component_host_arguments_impl *impl = owner != NULL ? owner->impl : NULL;
    if (impl == NULL || !impl->admission.committed || impl->admission.started)
        return TURBOWASM_INVALID_ARGUMENT;
    admission_start(&impl->admission);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_arguments_destroy(
    turbowasm_component_host_arguments *owner) {
    component_host_arguments_impl *impl = owner != NULL ? owner->impl : NULL;
    turbowasm_component_instance_public_impl *instance;
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_OK, cleanup;
    size_t i;
    if (impl == NULL) return TURBOWASM_OK;
    for (i = 0u; i < impl->admission.count; ++i) {
        const component_resource_argument *entry = &impl->admission.entries[i];
        if (entry->endpoint != NULL) {
            const turbowasm_component_endpoint *end = turbowasm_component_endpoint_value_get(entry->value);
            if (end != NULL && (end->lower_scope != NULL || end->value_owner->publishing))
                return TURBOWASM_INVALID_ARGUMENT;
        }
        if (entry->promoted && !turbowasm_component_exec_resource_value_idle(entry->value))
            return TURBOWASM_INVALID_ARGUMENT;
    }
    instance = impl->admission.instance;
    if (!turbowasm_component_host_activity_enter(instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    owner->impl = NULL;
    scope = turbowasm_runtime_scope_enter(&instance->component->binary.config);
    if (impl->values != NULL) {
        for (i = 0u; i < impl->count; ++i) {
            cleanup = internal_input_destroy(&impl->values[i], true);
            if (status == TURBOWASM_OK) status = cleanup;
        }
        turbowasm_rt_free(impl->values);
    }
    cleanup = admission_destroy(&impl->admission);
    if (status == TURBOWASM_OK) status = cleanup;
    impl->budget->used -= impl->bytes;
    turbowasm_component_host_unregister(&impl->registration);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    component_instance_release(instance);
    return status;
}

typedef struct component_result_build {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_resource *pending;
    component_host_endpoint_impl *pending_endpoints;
    turbowasm_component_host_budget *budget;
    size_t endpoint_bytes;
    size_t allocation_bytes;
    uint32_t allocation_refs;
} component_result_build;

static void result_allocation_release(void *owner, void *context, size_t bytes) {
    turbowasm_component_host_budget *budget = context;
    budget->used -= bytes;
    component_instance_release(owner);
}

/* New nodes and borrowed source strings are still unpublished. Reserve one
 * instance reference per actual allocation, without changing source metadata.
 * The root result owner keeps the instance alive while failures roll back. */
static turbowasm_status retain_result_allocations(const turbowasm_component_host_value *value,
    component_result_build *build) {
    const turbowasm_component_host_sequence *sequence = public_sequence(value);
    const turbowasm_component_host_variant *variant = public_variant(value);
    const void *allocation = NULL;
    size_t i;
    turbowasm_status status;
    if (sequence != NULL) allocation = sequence->items;
    else if (variant != NULL) allocation = variant->payload;
    else if (value->kind == TURBOWASM_COMPONENT_HOST_STRING) allocation = value->as.string.data;
    else if (value->kind == TURBOWASM_COMPONENT_HOST_FLAGS) allocation = value->as.flags.words;
    else if (value->kind == TURBOWASM_COMPONENT_HOST_OWN) allocation = value->as.own;
    if (allocation != NULL) {
        if (!turbowasm_rt_allocation_unowned(allocation) || !component_instance_retain(build->instance))
            return TURBOWASM_INVALID_ARGUMENT;
        ++build->allocation_refs;
    }
    if (sequence != NULL) {
        for (i = 0u; i < sequence->count; ++i) {
            status = retain_result_allocations(&sequence->items[i], build);
            if (status != TURBOWASM_OK) return status;
        }
    } else if (variant != NULL && variant->payload != NULL)
        return retain_result_allocations(variant->payload, build);
    return TURBOWASM_OK;
}

static void commit_result_allocation(component_result_build *build, void *allocation, size_t bytes) {
    if (allocation == NULL) return;
    turbowasm_rt_allocation_own(allocation, build->instance, build->budget, bytes, result_allocation_release);
    build->allocation_bytes += bytes;
}

/* Synchronous exports are also callable on an async instance. Reuse the same
 * allocation authority so their returned storage cannot escape its byte quota.
 * These trees are already converted, but remain private until charging commits. */
static turbowasm_status sync_result_storage(turbowasm_component_host_value *value,
    component_result_build *build, size_t *bytes, size_t limit, bool commit) {
    const turbowasm_component_host_sequence *sequence = public_sequence(value);
    const turbowasm_component_host_variant *variant = public_variant(value);
    void *allocation = NULL;
    size_t count = 0u, stride = 1u, i;
    turbowasm_status status;
    if (sequence != NULL) { allocation = sequence->items; count = sequence->count; stride = sizeof(*sequence->items); }
    else if (variant != NULL) { allocation = variant->payload; count = allocation != NULL; stride = sizeof(*variant->payload); }
    else if (value->kind == TURBOWASM_COMPONENT_HOST_STRING) {
        allocation = value->as.string.data; count = value->as.string.size;
        if (allocation != NULL && count == 0u) count = 1u;
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_FLAGS) {
        allocation = value->as.flags.words; count = value->as.flags.word_count; stride = sizeof(uint32_t);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_OWN) {
        allocation = value->as.own; count = 1u; stride = sizeof(*value->as.own);
    }
    status = argument_size_add(bytes, count, stride, limit);
    if (status != TURBOWASM_OK) return status;
    if (commit) commit_result_allocation(build, allocation, count * stride);
    if (sequence != NULL) {
        for (i = 0u; i < sequence->count; ++i) {
            status = sync_result_storage(&sequence->items[i], build, bytes, limit, commit);
            if (status != TURBOWASM_OK) return status;
        }
    } else if (variant != NULL && variant->payload != NULL)
        return sync_result_storage(variant->payload, build, bytes, limit, commit);
    return TURBOWASM_OK;
}

static turbowasm_status charge_sync_result(turbowasm_component_instance_public_impl *instance,
    turbowasm_component_host_value *value) {
    component_result_build build = {0};
    size_t bytes = 0u, committed = 0u;
    turbowasm_status status;
    if (!instance->host_budget_owned) return TURBOWASM_OK;
    build.instance = instance; build.budget = &instance->host_budget;
    status = sync_result_storage(value, &build, &bytes, build.budget->limit - build.budget->used, false);
    if (status == TURBOWASM_OK) status = retain_result_allocations(value, &build);
    if (status != TURBOWASM_OK) {
        while (build.allocation_refs != 0u) { --build.allocation_refs; component_instance_release(instance); }
        return status;
    }
    build.budget->used += bytes;
    /* The preflight validated this exclusive tree; installation invokes no
     * allocation or callbacks and cannot exceed the exact reservation. */
    (void)sync_result_storage(value, &build, &committed, bytes, true);
    return TURBOWASM_OK;
}

/* Staging owns only new wrappers/nodes. Strings and canonical obligations still
 * belong to the unchanged source until the complete tree is committed. */
static void discard_staged_result(turbowasm_component_host_value *value) {
    const turbowasm_component_host_sequence *sequence = public_sequence(value);
    const turbowasm_component_host_variant *variant = public_variant(value);
    size_t i;
    if (sequence != NULL) {
        for (i = 0u; i < sequence->count; ++i) discard_staged_result(&sequence->items[i]);
        turbowasm_rt_free(sequence->items);
    } else if (variant != NULL && variant->payload != NULL) {
        discard_staged_result(variant->payload); turbowasm_rt_free(variant->payload);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_FLAGS) {
        turbowasm_rt_free(value->as.flags.words);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_OWN && value->as.own != NULL) {
        turbowasm_component_instance_public_impl *instance = value->as.own->instance;
        turbowasm_rt_free(value->as.own); component_instance_release(instance);
    } else if (public_endpoint(value) != NULL && public_endpoint(value)->impl != NULL) {
        component_host_endpoint_impl *impl = public_endpoint(value)->impl;
        turbowasm_component_instance_public_impl *instance = impl->instance;
        /* Staging owns only this wrapper/ref; its canonical source and the
         * enclosing precharged byte reservation are still owned elsewhere. */
        turbowasm_rt_free(impl); component_instance_release(instance);
    }
    memset(value, 0, sizeof(*value));
}

static turbowasm_status stage_result_resource(component_result_build *build,
    const turbowasm_component_value *source, turbowasm_component_host_resource **out) {
    turbowasm_component_host_resource *owner;
    const turbowasm_component_resource_identity *identity =
        turbowasm_component_exec_resource_identity(&build->instance->exec, source->resource_identity);
    uint32_t count = build->instance->resource_count;
    if (identity == NULL) return TURBOWASM_TYPE_MISMATCH;
    for (owner = build->pending; owner != NULL; owner = owner->next) {
        if (owner->canonical.release_context == source->release_context) return TURBOWASM_INVALID_ARGUMENT;
        if (identity->provider != NULL && owner->external && owner->identity == source->resource_identity &&
            owner->rep.kind == source->as.resource_rep.kind &&
            (owner->rep.kind == TURBOWASM_VALUE_I32 ? owner->rep.as.i32 == source->as.resource_rep.as.i32
                : owner->rep.as.i64 == source->as.resource_rep.as.i64)) return TURBOWASM_TRAPPED;
        if (++count >= TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS) return TURBOWASM_OUT_OF_MEMORY;
    }
    if (count >= TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS) return TURBOWASM_OUT_OF_MEMORY;
    if (identity->provider != NULL) {
        for (owner = build->instance->resources; owner != NULL; owner = owner->next)
            if (owner->external && owner->identity == source->resource_identity &&
                owner->rep.kind == source->as.resource_rep.kind &&
                (owner->rep.kind == TURBOWASM_VALUE_I32
                    ? owner->rep.as.i32 == source->as.resource_rep.as.i32
                    : owner->rep.as.i64 == source->as.resource_rep.as.i64)) return TURBOWASM_TRAPPED;
    }
    owner = turbowasm_rt_calloc(1u, sizeof(*owner));
    if (owner == NULL) return TURBOWASM_OUT_OF_MEMORY;
    if (!component_instance_retain(build->instance)) {
        turbowasm_rt_free(owner); return TURBOWASM_INVALID_ARGUMENT;
    }
    owner->instance = build->instance; owner->identity = source->resource_identity;
    owner->rep = source->as.resource_rep; owner->external = identity->provider != NULL;
    owner->canonical = *source; owner->next = build->pending; build->pending = owner;
    *out = owner;
    return TURBOWASM_OK;
}

static turbowasm_status convert_result(
    turbowasm_component_value *source,
    turbowasm_component_host_value *out,
    uint32_t depth, component_result_build *build) {
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
            if (build == NULL) {
                source->as.string.data = NULL;
                source->as.string.size = 0u;
            }
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
                turbowasm_status status = convert_result(
                    &input->items[i], &sequence.items[i], depth + 1u, build);
                if (status != TURBOWASM_OK) {
                    if (build != NULL) discard_staged_result(out);
                    else turbowasm_component_host_value_destroy(out);
                    return status;
                }
            }
            if (build == NULL) {
                turbowasm_rt_free(input->items);
                input->items = NULL;
                input->count = 0u;
            }
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
                status = convert_result(input->payload,
                    variant.payload, depth + 1u, build);
                if (status != TURBOWASM_OK) {
                    if (build != NULL) discard_staged_result(variant.payload);
                    else turbowasm_component_host_value_destroy(variant.payload);
                    turbowasm_rt_free(variant.payload);
                    return status;
                }
                if (build == NULL) {
                    turbowasm_rt_free(input->payload);
                    input->payload = NULL;
                }
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
            if (build != NULL) {
                turbowasm_status status = stage_result_resource(build, source, &out->as.own);
                if (status != TURBOWASM_OK) return status;
                break;
            }
            if (source->release != resource_owner_release || source->release_context == NULL)
                return TURBOWASM_INVALID_ARGUMENT;
            out->as.own = source->release_context;
            break;

        case TURBOWASM_COMPONENT_TYPE_FUTURE:
        case TURBOWASM_COMPONENT_TYPE_STREAM: {
            component_host_endpoint_impl *impl, *pending;
            const turbowasm_component_endpoint *end = turbowasm_component_endpoint_value_get(source);
            if (build == NULL) return TURBOWASM_UNSUPPORTED;
            if (end == NULL || end->lower_scope != NULL || end->value_owner->publishing ||
                end->value_owner->host_admitted != NULL || end->value_owner->host_finish != NULL ||
                !turbowasm_component_endpoint_domain_pair_retained(end)) return TURBOWASM_INVALID_ARGUMENT;
            for (pending = build->pending_endpoints; pending != NULL; pending = pending->stage_next)
                if (pending->endpoint == end) return TURBOWASM_INVALID_ARGUMENT;
            if (!component_instance_retain(build->instance)) return TURBOWASM_INVALID_ARGUMENT;
            impl = turbowasm_rt_calloc(1u, sizeof(*impl));
            if (impl == NULL) { component_instance_release(build->instance); return TURBOWASM_OUT_OF_MEMORY; }
            impl->instance = build->instance; impl->budget = build->budget;
            impl->endpoint = (turbowasm_component_endpoint *)end; impl->driving = true;
            impl->stage_record = source->as.endpoint.owner;
            impl->stage_next = build->pending_endpoints; build->pending_endpoints = impl;
            build->endpoint_bytes += sizeof(*impl);
            public_endpoint(out)->impl = impl;
            break;
        }

        default:
            return TURBOWASM_UNSUPPORTED;
    }

    if (build == NULL) memset(source, 0, sizeof(*source));
    return TURBOWASM_OK;
}

static turbowasm_status internal_to_public(turbowasm_component_value *source,
    turbowasm_component_host_value *out, uint32_t depth) {
    return convert_result(source, out, depth, NULL);
}

typedef struct component_host_result_impl {
    turbowasm_component_instance_public_impl *instance;
    turbowasm_component_host_budget *budget;
    size_t bytes;
    turbowasm_component_host_value value;
    bool driving;
    turbowasm_component_host_registration registration;
} component_host_result_impl;

static turbowasm_status result_storage_size(turbowasm_component_instance_public_impl *instance,
    turbowasm_component_value *value, uint32_t depth, size_t *size, size_t limit, uint32_t *resources) {
    turbowasm_component_value_list *sequence;
    turbowasm_component_value_variant *variant;
    turbowasm_status status;
    uint64_t i;
    if (value == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (depth >= COMPONENT_PUBLIC_MAX_DEPTH) return TURBOWASM_TRAPPED;
    if (!public_kind_supported((turbowasm_component_host_value_kind)value->kind) ||
        value->kind == TURBOWASM_COMPONENT_TYPE_BORROW) return TURBOWASM_UNSUPPORTED;
    if (value->kind != TURBOWASM_COMPONENT_TYPE_OWN && value->kind != TURBOWASM_COMPONENT_TYPE_FUTURE &&
        value->kind != TURBOWASM_COMPONENT_TYPE_STREAM && value->release != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    sequence = internal_sequence(value); variant = internal_variant(value);
    if (sequence != NULL) {
        if ((sequence->count != 0u && sequence->items == NULL) || sequence->count > SIZE_MAX)
            return TURBOWASM_INVALID_ARGUMENT;
        status = argument_size_add(size, (size_t)sequence->count, sizeof(turbowasm_component_host_value), limit);
        if (status != TURBOWASM_OK) return status;
        for (i = 0u; i < sequence->count; ++i) {
            status = result_storage_size(instance, &sequence->items[i], depth + 1u, size, limit, resources);
            if (status != TURBOWASM_OK) return status;
        }
    } else if (variant != NULL && variant->payload != NULL) {
        status = argument_size_add(size, 1u, sizeof(turbowasm_component_host_value), limit);
        if (status != TURBOWASM_OK) return status;
        return result_storage_size(instance, variant->payload, depth + 1u, size, limit, resources);
    } else if (value->kind == TURBOWASM_COMPONENT_TYPE_STRING) {
        if (value->as.string.size > SIZE_MAX ||
            (value->as.string.size != 0u && value->as.string.data == NULL)) return TURBOWASM_INVALID_ARGUMENT;
        return argument_size_add(size, value->as.string.data != NULL && value->as.string.size == 0u
            ? 1u : (size_t)value->as.string.size, 1u, limit);
    } else if (value->kind == TURBOWASM_COMPONENT_TYPE_FLAGS) {
        return argument_size_add(size, COMPONENT_PUBLIC_FLAGS_WORDS, sizeof(uint32_t), limit);
    } else if (value->kind == TURBOWASM_COMPONENT_TYPE_OWN) {
        if (!turbowasm_component_exec_resource_value_owned(&instance->exec, value)) return TURBOWASM_INVALID_ARGUMENT;
        if (instance->resource_count >= TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
            *resources >= TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS - instance->resource_count)
            return TURBOWASM_OUT_OF_MEMORY;
        ++*resources;
        status = argument_size_add(size, 1u, sizeof(turbowasm_component_host_resource), limit);
        if (status == TURBOWASM_OK)
            status = argument_size_add(size, 1u, turbowasm_component_exec_resource_adopt_size(), limit);
        return status;
    } else if (value->kind == TURBOWASM_COMPONENT_TYPE_FUTURE || value->kind == TURBOWASM_COMPONENT_TYPE_STREAM) {
        const turbowasm_component_endpoint *end = turbowasm_component_endpoint_value_get(value);
        if (end == NULL || end->lower_scope != NULL || end->value_owner->publishing ||
            end->value_owner->host_admitted != NULL || end->value_owner->host_finish != NULL ||
            !turbowasm_component_endpoint_domain_pair_retained(end)) return TURBOWASM_INVALID_ARGUMENT;
        return argument_size_add(size, 1u, sizeof(component_host_endpoint_impl), limit);
    }
    return TURBOWASM_OK;
}

/* The staged tree has the same shape. No allocation or guest callback occurs
 * while source ownership and the public resource registry are committed. */
static void commit_staged_result(turbowasm_component_value *source,
    turbowasm_component_host_value *value, component_result_build *build, bool borrowed_root) {
    turbowasm_component_value_list *sequence = internal_sequence(source);
    turbowasm_component_value_variant *variant = internal_variant(source);
    const turbowasm_component_host_sequence *out_sequence = public_sequence(value);
    const turbowasm_component_host_variant *out_variant = public_variant(value);
    uint64_t i;
    if (sequence != NULL) {
        commit_result_allocation(build, out_sequence->items, out_sequence->count * sizeof(*out_sequence->items));
        for (i = 0u; i < sequence->count; ++i)
            commit_staged_result(&sequence->items[i], &out_sequence->items[i], build, false);
        if (!borrowed_root) turbowasm_rt_free(sequence->items);
    } else if (variant != NULL && variant->payload != NULL) {
        commit_result_allocation(build, out_variant->payload, sizeof(*out_variant->payload));
        commit_staged_result(variant->payload, out_variant->payload, build, false);
        turbowasm_rt_free(variant->payload);
    } else if (source->kind == TURBOWASM_COMPONENT_TYPE_OWN) {
        turbowasm_component_host_resource *owner = value->as.own;
        /* The public wrapper owns its canonical adopted resource as well.
         * Their joint reservation survives result delivery until both retire. */
        commit_result_allocation(build, owner, sizeof(*owner) + turbowasm_component_exec_resource_adopt_size());
        owner->next = owner->instance->resources;
        owner->instance->resources = owner; ++owner->instance->resource_count;
    } else if (public_endpoint(value) != NULL) {
        component_host_endpoint_impl *impl = public_endpoint(value)->impl;
        /* Staging authenticated this fresh source, held exclusive by the root
         * owner. Defer record deallocation until every leaf has transferred. */
        impl->stage_record->endpoint = NULL;
        impl->endpoint->value_owner = NULL;
        turbowasm_component_host_endpoint_register(impl);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_STRING) {
        commit_result_allocation(build, value->as.string.data,
            value->as.string.size == 0u ? 1u : value->as.string.size);
    } else if (value->kind == TURBOWASM_COMPONENT_HOST_FLAGS) {
        commit_result_allocation(build, value->as.flags.words, value->as.flags.word_count * sizeof(uint32_t));
    }
    memset(source, 0, sizeof(*source));
}

static turbowasm_status prepare_host_result(
    turbowasm_component_host_result *owner, turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    turbowasm_component_value *source, turbowasm_component_host_budget *budget, bool batch) {
    component_host_result_impl *impl;
    component_result_build build = {0};
    turbowasm_runtime_scope scope;
    size_t bytes = sizeof(*impl), available;
    uint32_t resources = 0u;
    turbowasm_status status;
    build.instance = instance; build.budget = budget;
    if (owner == NULL || owner->impl != NULL || instance == NULL || !instance->exec.initialized ||
        instance->shutdown_driving ||
        instance->exec.task_domain.table == NULL || graph == NULL || source == NULL ||
        !turbowasm_component_host_budget_valid(instance, budget))
        return TURBOWASM_INVALID_ARGUMENT;
    available = budget->limit - budget->used;
    if (bytes > available) return TURBOWASM_OUT_OF_MEMORY;
    status = result_storage_size(instance, source, 0u, &bytes, available, &resources);
    if (status != TURBOWASM_OK) return status;
    if (batch) {
        uint64_t i;
        for (i = 0u; i < source->as.list.count; ++i) {
            status = turbowasm_component_canonical_validate_value(graph, type, &source->as.list.items[i]);
            if (status != TURBOWASM_OK) break;
        }
    } else status = turbowasm_component_canonical_validate_value(graph, type, source);
    if (status != TURBOWASM_OK) return status;
    if (!component_instance_retain(instance)) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(instance, false)) {
        component_instance_release(instance); return TURBOWASM_INVALID_ARGUMENT;
    }
    budget->used += bytes;
    scope = turbowasm_runtime_scope_enter(&instance->exec.binary->config);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL) {
        budget->used -= bytes; turbowasm_runtime_scope_leave(scope);
        turbowasm_component_host_activity_leave(instance);
        component_instance_release(instance); return TURBOWASM_OUT_OF_MEMORY;
    }
    status = convert_result(source, &impl->value, 0u, &build);
    if (status == TURBOWASM_OK) status = retain_result_allocations(&impl->value, &build);
    if (status != TURBOWASM_OK) {
        discard_staged_result(&impl->value); turbowasm_rt_free(impl);
        while (build.allocation_refs != 0u) { --build.allocation_refs; component_instance_release(instance); }
        budget->used -= bytes;
        turbowasm_runtime_scope_leave(scope); turbowasm_component_host_activity_leave(instance);
        component_instance_release(instance); return status;
    }
    commit_staged_result(source, &impl->value, &build, batch);
    {
        component_host_endpoint_impl *endpoint, *next;
        for (endpoint = build.pending_endpoints; endpoint != NULL; endpoint = endpoint->stage_next) {
            turbowasm_rt_free(endpoint->stage_record); endpoint->stage_record = NULL;
        }
        /* Every source has moved and every pending cleanup has finished. */
        for (endpoint = build.pending_endpoints; endpoint != NULL; endpoint = next) {
            next = endpoint->stage_next; endpoint->stage_next = NULL; endpoint->driving = false;
        }
    }
    impl->instance = instance; impl->budget = budget;
    impl->bytes = bytes - build.endpoint_bytes - build.allocation_bytes;
    owner->impl = impl;
    turbowasm_component_host_register(instance, &impl->registration, impl, NULL, NULL);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_result_prepare(
    turbowasm_component_host_result *owner, turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    turbowasm_component_value *source, turbowasm_component_host_budget *budget) {
    return prepare_host_result(owner, instance, graph, type, source, budget, false);
}
turbowasm_status turbowasm_component_host_result_prepare_batch(
    turbowasm_component_host_result *owner, turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref element,
    turbowasm_component_value *values, uint32_t count, turbowasm_component_host_budget *budget) {
    turbowasm_component_value source = {.kind = TURBOWASM_COMPONENT_TYPE_LIST};
    source.as.list.items = values; source.as.list.count = count;
    return prepare_host_result(owner, instance, graph, element, &source, budget, true);
}

static void host_result_forget(turbowasm_component_host_result *owner) {
    component_host_result_impl *impl = owner->impl;
    turbowasm_component_instance_public_impl *instance = impl->instance;
    /* The caller holds activity through unpublication and allocator callbacks. */
    impl->budget->used -= impl->bytes;
    turbowasm_component_host_unregister(&impl->registration);
    owner->impl = NULL; turbowasm_rt_free(impl);
    turbowasm_component_host_activity_leave(instance); component_instance_release(instance);
}

turbowasm_status turbowasm_component_host_result_take(
    turbowasm_component_host_result *owner, turbowasm_component_host_value *out) {
    component_host_result_impl *impl = owner != NULL ? owner->impl : NULL;
    if (impl == NULL || impl->driving || impl->instance->shutdown_driving ||
        out == NULL || (int)out->kind != 0) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    *out = impl->value; host_result_forget(owner);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_host_result_destroy(turbowasm_component_host_result *owner) {
    component_host_result_impl *impl;
    turbowasm_status status;
    if (owner == NULL) return TURBOWASM_INVALID_ARGUMENT;
    impl = owner->impl;
    if (impl == NULL) return TURBOWASM_OK;
    if (impl->driving || !turbowasm_component_host_activity_enter(impl->instance, false))
        return TURBOWASM_INVALID_ARGUMENT;
    impl->driving = true;
    status = destroy_host_value(&impl->value, true);
    impl->driving = false;
    if ((int)impl->value.kind == 0) host_result_forget(owner);
    else turbowasm_component_host_activity_leave(impl->instance);
    return status;
}

static turbowasm_status check_destroy_owner(
    const turbowasm_component_host_value *value, void *context) {
    bool internal = context != NULL && *(const bool *)context;
    if (public_endpoint(value) != NULL) {
        const component_host_endpoint_impl *body = public_endpoint(value)->impl;
        if (!internal && body != NULL && body->instance->host_budget_owned && body->instance->host_activity != 0u)
            return TURBOWASM_INVALID_ARGUMENT;
        const turbowasm_component_endpoint *end = turbowasm_component_host_endpoint_view(public_endpoint(value));
        if (end == NULL || !end->readable ||
            end->waitable.state.endpoint.future != (value->kind == TURBOWASM_COMPONENT_HOST_FUTURE) ||
            !turbowasm_component_host_endpoint_destroy_ready(public_endpoint(value))) return TURBOWASM_INVALID_ARGUMENT;
    }
    if (value->kind == TURBOWASM_COMPONENT_HOST_OWN &&
        (value->as.own == NULL || value->as.own->loans != 0u || value->as.own->busy))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!internal && value->kind == TURBOWASM_COMPONENT_HOST_OWN &&
        value->as.own->instance->host_budget_owned && value->as.own->instance->host_activity != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_OK;
}

static turbowasm_status mark_destroy_owner(
    const turbowasm_component_host_value *value, void *context) {
    bool lock = *(const bool *)context;
    turbowasm_component_host_endpoint *endpoint = public_endpoint(value);
    if (endpoint != NULL) {
        component_host_endpoint_impl *impl = endpoint->impl;
        if (lock && impl->driving) return TURBOWASM_INVALID_ARGUMENT;
        impl->driving = lock;
    }
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
    } else if (public_endpoint(&owned) != NULL) {
        first = turbowasm_component_host_endpoint_destroy_locked(public_endpoint(&owned));
    }
    return first;
}

turbowasm_status turbowasm_component_host_value_destroy_preflight(const turbowasm_component_host_value *value) {
    return visit_public_resources(value, 0u, check_destroy_owner, NULL);
}
turbowasm_status turbowasm_component_host_value_destroy_lock(const turbowasm_component_host_value *value, bool lock) {
    turbowasm_status status = visit_public_resources(value, 0u, mark_destroy_owner, &lock);
    if (status != TURBOWASM_OK && lock) {
        lock = false; (void)visit_public_resources(value, 0u, mark_destroy_owner, &lock);
    }
    return status;
}
turbowasm_status turbowasm_component_host_value_destroy_locked(turbowasm_component_host_value *value) {
    return destroy_public_value(value);
}

static turbowasm_status destroy_host_value(turbowasm_component_host_value *value, bool internal) {
    bool lock = true;
    turbowasm_status status = visit_public_resources(value, 0u, check_destroy_owner, &internal);
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

turbowasm_status turbowasm_component_host_value_destroy(turbowasm_component_host_value *value) {
    return destroy_host_value(value, false);
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

static turbowasm_status component_load(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config, bool async_metadata) {
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

    status = async_metadata
        ? turbowasm_component_binary_decode_async_metadata(&impl->binary, bytes, size, &normalized)
        : turbowasm_component_binary_load_with_config(&impl->binary, bytes, size, &normalized);
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

static turbowasm_status component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits,
    const turbowasm_component_exec_imports *imports, size_t import_count,
    const turbowasm_component_async_options *options) {
    turbowasm_component_public_impl *component_state;
    turbowasm_component_instance_public_impl *impl;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (instance == NULL || component == NULL ||
        instance->impl != NULL || (import_count != 0u && imports == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    component_state = turbowasm_component_public_impl_get(component);
    if (component_state == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    /* Allocator callbacks may release the loader's public carrier. Retain the
     * immutable binary/config before any constructor allocation can reenter. */
    if (!turbowasm_component_public_impl_retain(component_state))
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(
        &component_state->binary.config);
    impl = (turbowasm_component_instance_public_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL) {
        turbowasm_component_public_impl_release(component_state);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    if (options != NULL) {
        impl->host_budget_owned = true;
        impl->host_budget.limit = options->host_bytes;
        impl->host_transfer_limit = options->transfers;
    }

    impl->component = component_state;

    status = limits != NULL
        ? turbowasm_component_exec_init_async_with_import_sets(&impl->exec,
              &component_state->binary, limits, imports, import_count)
        : turbowasm_component_exec_init(&impl->exec, &component_state->binary);
    if (status != TURBOWASM_OK) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        turbowasm_component_public_impl_release(component_state);
        return status;
    }

    impl->ref_count = 1u;
    if (limits != NULL) {
        if (options == NULL) impl->host_transfer_limit = limits->handles;
        impl->exec.task_domain.pair_owner = impl;
        impl->exec.task_domain.pair_retain = pair_instance_retain;
        impl->exec.task_domain.pair_release = pair_instance_release;
    }
    instance->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_load_borrowed_with_config(turbowasm_component *component,
    const uint8_t *bytes, size_t size, const turbowasm_runtime_config *config) {
    return component_load(component, bytes, size, config, false);
}

turbowasm_status turbowasm_component_load_async_private(turbowasm_component *component,
    const uint8_t *bytes, size_t size, const turbowasm_runtime_config *config) {
    return component_load(component, bytes, size, config, true);
}

turbowasm_status turbowasm_component_instance_create(
    turbowasm_component_instance *instance, const turbowasm_component *component) {
    return component_instance_create(instance, component, NULL, NULL, 0u, NULL);
}

turbowasm_status turbowasm_component_instance_create_async_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits) {
    if (limits == NULL) return TURBOWASM_INVALID_ARGUMENT;
    return component_instance_create(instance, component, limits, NULL, 0u, NULL);
}

turbowasm_status turbowasm_component_instance_create_async_with_import_sets_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits,
    const turbowasm_component_exec_imports *imports, size_t import_count) {
    if (limits == NULL) return TURBOWASM_INVALID_ARGUMENT;
    return component_instance_create(instance, component, limits, imports, import_count, NULL);
}

void turbowasm_component_async_options_init_private(turbowasm_component_async_options *options) {
    enum { DEFAULT_TASKS = 64, DEFAULT_HANDLES = 4096, DEFAULT_TRANSFERS = 64,
           DEFAULT_HOST_BYTES = 16 * 1024 * 1024 };
    if (options != NULL)
        *options = (turbowasm_component_async_options){DEFAULT_TASKS, DEFAULT_HANDLES,
            DEFAULT_TRANSFERS, DEFAULT_HOST_BYTES};
}

turbowasm_status turbowasm_component_instance_create_async_with_options_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_async_options *options,
    const turbowasm_component_exec_imports *imports, size_t import_count) {
    turbowasm_component_exec_async_limits limits;
    turbowasm_component_async_options snapshot;
    if (options == NULL) return TURBOWASM_INVALID_ARGUMENT;
    snapshot = *options;
    if (snapshot.tasks == 0u || snapshot.tasks == UINT32_MAX ||
        snapshot.handles == 0u || snapshot.handles > TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS ||
        snapshot.transfers == 0u || snapshot.transfers == UINT32_MAX ||
        snapshot.host_bytes == 0u || snapshot.host_bytes == SIZE_MAX)
        return TURBOWASM_INVALID_ARGUMENT;
    limits = (turbowasm_component_exec_async_limits){snapshot.tasks, snapshot.handles};
    return component_instance_create(instance, component, &limits, imports, import_count, &snapshot);
}

bool turbowasm_component_host_budget_valid(const turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_host_budget *budget) {
    return instance != NULL && budget != NULL && budget->limit != 0u && budget->limit != SIZE_MAX &&
        budget->used <= budget->limit && (!instance->host_budget_owned || budget == &instance->host_budget);
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
    /* Explicit shutdown keeps its public carrier available to drive remaining
     * guest cleanup and host completion until the drain reaches terminal state. */
    if (impl != NULL && impl->admission_closed && !impl->shutdown_complete) return;
    instance->impl = NULL;
    component_instance_release(impl);
}

bool turbowasm_component_instance_public_impl_retain(turbowasm_component_instance_public_impl *impl) {
    return component_instance_retain(impl);
}
void turbowasm_component_instance_public_impl_release(turbowasm_component_instance_public_impl *impl) {
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
    if (impl == NULL || impl->component == NULL || impl->admission_closed || impl->shutdown_driving ||
        (impl->host_budget_owned && impl->host_activity != 0u))
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
    if (!turbowasm_component_host_activity_enter(impl, true)) {
        component_instance_release(impl); return TURBOWASM_INVALID_ARGUMENT;
    }
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
        if (status == TURBOWASM_OK) status = charge_sync_result(impl, &public_result);
        if (status != TURBOWASM_OK)
            goto done;
        *result = public_result;
        memset(&public_result, 0, sizeof(public_result));
        *out_result_count = 1u;
    }

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i], false);
        turbowasm_rt_free(internal_arguments);
    }
    if (internal_result.kind != TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(&internal_result);

    destroy_host_value(&public_result, true);
    admission_destroy(&admission);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(impl);
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
    turbowasm_component_host_registration registration;
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
    if (instance_impl == NULL || instance_impl->component == NULL ||
        instance_impl->admission_closed || instance_impl->shutdown_driving ||
        (instance_impl->host_budget_owned && instance_impl->host_activity != 0u))
        return TURBOWASM_INVALID_ARGUMENT;

    if (!public_export_result_type(
            instance_impl, export_name, &has_result, &adapter))
        return TURBOWASM_UNSUPPORTED;

    if (!component_instance_retain(instance_impl))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(instance_impl, true)) {
        component_instance_release(instance_impl); return TURBOWASM_INVALID_ARGUMENT;
    }
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
    turbowasm_component_host_register(instance_impl, &call_impl->registration, call_impl, NULL, NULL);
    call_impl = NULL;

done:
    if (internal_arguments != NULL) {
        for (i = 0u; i < argument_count; ++i)
            internal_input_destroy(&internal_arguments[i], false);
        turbowasm_rt_free(internal_arguments);
    }
    if (call_impl != NULL) {
        turbowasm_component_exec_call_destroy(
            &call_impl->call);
        admission_destroy(&call_impl->admission);
        turbowasm_rt_free(call_impl);
    }
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance_impl);
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
    if (!turbowasm_component_host_activity_enter(instance, false)) return;
    call->impl = NULL;
    scope = turbowasm_runtime_scope_enter(
        &instance->component->binary.config);
    turbowasm_component_exec_call_destroy(&impl->call);
    admission_destroy(&impl->admission);
    turbowasm_component_host_unregister(&impl->registration);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(instance);
    component_instance_release(instance);
}

turbowasm_status turbowasm_component_call_resume(
    turbowasm_component_call *call,
    const turbowasm_execution_options *options) {
    turbowasm_component_call_public_impl *impl = component_call_public_impl_mut(call);
    turbowasm_status status;
    turbowasm_execution_state state;
    turbowasm_runtime_scope scope;
    if (impl == NULL || (impl->instance->host_budget_owned && impl->instance->host_activity != 0u))
        return TURBOWASM_INVALID_ARGUMENT;
    state = turbowasm_component_exec_call_state_get(&impl->call);
    if (state != TURBOWASM_EXECUTION_READY && state != TURBOWASM_EXECUTION_YIELDED)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(&impl->instance->component->binary.config);
    admission_start(&impl->admission);
    status = turbowasm_component_exec_call_resume(&impl->call, options);
    if (status != TURBOWASM_YIELDED)
        admission_destroy(&impl->admission);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(impl->instance);
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

    if (!turbowasm_component_host_activity_enter(impl->instance, false)) return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_component_exec_call_take_result(
        &impl->call, &internal_result);
    if (status != TURBOWASM_OK) {
        turbowasm_component_host_activity_leave(impl->instance);
        return status;
    }

    /* A repeated take must not erase an already-owned caller result. */
    memset(out_result, 0, sizeof(*out_result));
    scope = turbowasm_runtime_scope_enter(
        &impl->instance->component->binary.config);
    status = internal_to_public(
        &internal_result, out_result, 0u);
    if (status == TURBOWASM_OK) status = charge_sync_result(impl->instance, out_result);
    if (internal_result.kind !=
        TURBOWASM_COMPONENT_TYPE_UNDEFINED)
        turbowasm_component_value_destroy(
            &internal_result);
    if (status != TURBOWASM_OK)
        destroy_host_value(out_result, true);
    turbowasm_runtime_scope_leave(scope);
    turbowasm_component_host_activity_leave(impl->instance);
    return status;
}
