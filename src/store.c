#include "store_internal.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "validate_type.h"

#include <string.h>

enum {
    TW_STORE_DEFAULT_OBJECTS = 65536u,
    TW_STORE_DEFAULT_ROOTS = 4096u,
    TW_STORE_DEFAULT_BYTES = 64u * 1024u * 1024u,
    TW_GC_GENERATION_MAX = 0x7fffffffu
};
#define TW_GC_I31_TAG (UINT64_C(1) << 63u)

typedef struct tw_gc_slot {
    turbowasm_gc_object *object;
    uint32_t generation;
    uint32_t next_free;
    bool marked;
} tw_gc_slot;

typedef struct tw_root {
    struct tw_root *next;
    turbowasm_store_impl *store;
    turbowasm_value value;
} tw_root;

typedef struct tw_store_types {
    struct tw_store_types *next;
    turbowasm_validation_context types;
} tw_store_types;

struct turbowasm_store_impl {
    turbowasm_store_config config;
    turbowasm_store_stats stats;
    const void *owner_thread;
    tw_gc_slot *slots;
    uint32_t *worklist;
    uint32_t work_count;
    uint32_t free_head;
    tw_root *roots;
    tw_store_types *types;
    turbowasm_instance_impl *instances;
    turbowasm_gc_source *sources;
    uint32_t source_count;
};

bool turbowasm_store_is_owner(const turbowasm_store_impl *store) {
    return store != NULL && store->owner_thread == cmeta_thread_current_token();
}

void turbowasm_store_config_init(turbowasm_store_config *config) {
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->max_bytes = TW_STORE_DEFAULT_BYTES;
        config->max_objects = TW_STORE_DEFAULT_OBJECTS;
        config->max_roots = TW_STORE_DEFAULT_ROOTS;
    }
}

turbowasm_status turbowasm_store_create(turbowasm_store *store,
                                        const turbowasm_store_config *config) {
    turbowasm_store_config settings;
    turbowasm_store_impl *impl;
    turbowasm_runtime_scope scope;
    uint64_t overhead;
    uint32_t i;
    if (store == NULL || store->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_store_config_init(&settings);
    if (config != NULL) {
        settings = *config;
        if (settings.max_bytes == 0u)
            settings.max_bytes = TW_STORE_DEFAULT_BYTES;
        if (settings.max_objects == 0u)
            settings.max_objects = TW_STORE_DEFAULT_OBJECTS;
        if (settings.max_roots == 0u)
            settings.max_roots = TW_STORE_DEFAULT_ROOTS;
    }
    if (!turbowasm_runtime_config_normalize(config == NULL ? NULL : &config->runtime,
                                            &settings.runtime))
        return TURBOWASM_INVALID_ARGUMENT;
    overhead =
        sizeof(*impl) + (uint64_t)settings.max_objects * (sizeof(tw_gc_slot) + sizeof(uint32_t));
    if (overhead > settings.max_bytes || overhead > SIZE_MAX || settings.max_objects == UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    scope = turbowasm_runtime_scope_enter(&settings.runtime);
    impl = turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl != NULL) {
        impl->slots = turbowasm_rt_calloc(settings.max_objects, sizeof(*impl->slots));
        impl->worklist = turbowasm_rt_calloc(settings.max_objects, sizeof(*impl->worklist));
    }
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    if (impl->slots == NULL || impl->worklist == NULL) {
        turbowasm_rt_free(impl->slots);
        turbowasm_rt_free(impl->worklist);
        turbowasm_rt_free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    impl->config = settings;
    impl->stats.bytes = (size_t)overhead;
    impl->owner_thread = cmeta_thread_current_token();
    for (i = 0u; i < settings.max_objects; ++i) {
        impl->slots[i].generation = 1u;
        impl->slots[i].next_free = i + 1u;
    }
    impl->slots[settings.max_objects - 1u].next_free = UINT32_MAX;
    store->impl = impl;
    return TURBOWASM_OK;
}

bool turbowasm_gc_is_i31(turbowasm_gcref ref) {
    return (ref.handle & TW_GC_I31_TAG) != 0u;
}

turbowasm_value turbowasm_gc_i31(uint32_t bits) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_GCREF;
    value.as.gcref.handle = TW_GC_I31_TAG | (bits & TW_GC_GENERATION_MAX);
    return value;
}

uint32_t turbowasm_gc_i31_bits(turbowasm_gcref ref) {
    return (uint32_t)ref.handle & TW_GC_GENERATION_MAX;
}

turbowasm_gc_object *turbowasm_gc_resolve(turbowasm_store_impl *store, turbowasm_gcref ref) {
    uint32_t index = (uint32_t)ref.handle;
    if (store == NULL || ref.store != store || index == 0u || index > store->config.max_objects ||
        turbowasm_gc_is_i31(ref))
        return NULL;
    --index;
    return store->slots[index].generation == (uint32_t)(ref.handle >> 32u)
               ? store->slots[index].object
               : NULL;
}

bool turbowasm_gc_value_valid(turbowasm_store_impl *store, const turbowasm_value *value) {
    if (value == NULL)
        return false;
    if (value->kind != TURBOWASM_VALUE_GCREF && value->kind != TURBOWASM_VALUE_MANAGED_EXTERNREF)
        return true;
    return value->as.gcref.handle == 0u ||
           (turbowasm_gc_is_i31(value->as.gcref) && value->as.gcref.store == NULL &&
            (value->as.gcref.handle & ~(TW_GC_I31_TAG | TW_GC_GENERATION_MAX)) == 0u) ||
           turbowasm_gc_resolve(store, value->as.gcref) != NULL;
}

void turbowasm_gc_mark_values(turbowasm_store_impl *store, const turbowasm_value *values,
                              size_t count) {
    size_t i;
    for (i = 0u; i < count; ++i) {
        uint32_t index;
        if ((values[i].kind != TURBOWASM_VALUE_GCREF &&
             values[i].kind != TURBOWASM_VALUE_MANAGED_EXTERNREF) ||
            turbowasm_gc_resolve(store, values[i].as.gcref) == NULL)
            continue;
        index = (uint32_t)values[i].as.gcref.handle - 1u;
        if (!store->slots[index].marked) {
            store->slots[index].marked = true;
            store->worklist[store->work_count++] = index;
        }
    }
}

turbowasm_status turbowasm_gc_source_add(turbowasm_store_impl *store, turbowasm_gc_source *source) {
    if (store != NULL) {
        if (!turbowasm_store_is_owner(store))
            return TURBOWASM_INVALID_ARGUMENT;
        if (store->source_count >= store->config.max_roots - store->stats.roots ||
            sizeof(*source) > store->config.max_bytes - store->stats.bytes)
            return TURBOWASM_OUT_OF_MEMORY;
        source->next = store->sources;
        store->sources = source;
        ++store->source_count;
        store->stats.bytes += sizeof(*source);
    }
    return TURBOWASM_OK;
}

void turbowasm_gc_source_remove(turbowasm_store_impl *store, turbowasm_gc_source *source) {
    turbowasm_gc_source **link;
    if (store == NULL)
        return;
    for (link = &store->sources; *link != NULL; link = &(*link)->next)
        if (*link == source) {
            *link = source->next;
            source->next = NULL;
            --store->source_count;
            store->stats.bytes -= sizeof(*source);
            return;
        }
}

void turbowasm_gc_sources_remove_owner(turbowasm_store_impl *store, const void *owner) {
    turbowasm_gc_source **link;
    if (store == NULL)
        return;
    for (link = &store->sources; *link != NULL;)
        if ((*link)->owner == owner) {
            *link = (*link)->next;
            --store->source_count;
            store->stats.bytes -= sizeof(turbowasm_gc_source);
        } else
            link = &(*link)->next;
}

static void collect(turbowasm_store_impl *store) {
    tw_root *root;
    turbowasm_instance_impl *instance;
    turbowasm_gc_source *source;
    uint32_t i, j;
    store->work_count = 0u;
    for (root = store->roots; root != NULL; root = root->next)
        turbowasm_gc_mark_values(store, &root->value, 1u);
    for (source = store->sources; source != NULL; source = source->next)
        source->trace(store, source->context);
    for (instance = store->instances; instance != NULL; instance = instance->store_next) {
        turbowasm_exception *exception;
        const turbowasm_validation_context *validation =
            &turbowasm_module_impl_get(instance->module)->validation;
        turbowasm_gc_mark_values(store, instance->globals, instance->global_count);
        for (i = 0u; i < instance->table_count; ++i)
            if (instance->tables[i].entries != NULL)
                for (j = 0u; j < instance->tables[i].size; ++j)
                    turbowasm_gc_mark_values(store, &instance->tables[i].entries[j].value, 1u);
        for (exception = instance->exceptions; exception != NULL; exception = exception->next)
            turbowasm_gc_mark_values(store, exception->payload, exception->payload_count);
        if (instance->element_values != NULL)
            for (i = 0u; i < instance->element_segment_count; ++i)
                if (instance->element_values[i] != NULL)
                    turbowasm_gc_mark_values(store, instance->element_values[i],
                                             validation->element_segments[i].item_count);
    }
    while (store->work_count != 0u) {
        turbowasm_gc_object *object = store->slots[store->worklist[--store->work_count]].object;
        turbowasm_gc_mark_values(store, object->values, object->count);
    }
    /* Each reachable object enters the fixed worklist once: O(slots + live
     * fields + roots) time and O(max_objects) preallocated auxiliary space. */
    for (i = 0u; i < store->config.max_objects; ++i) {
        tw_gc_slot *slot = &store->slots[i];
        if (slot->object != NULL && !slot->marked) {
            store->stats.bytes -= slot->object->bytes;
            --store->stats.objects;
            turbowasm_rt_free(slot->object);
            slot->object = NULL;
            if (slot->generation < TW_GC_GENERATION_MAX) {
                ++slot->generation;
                slot->next_free = store->free_head;
                store->free_head = i;
            }
        }
        slot->marked = false;
    }
    ++store->stats.collections;
}

turbowasm_status turbowasm_store_collect(turbowasm_store *store) {
    turbowasm_store_impl *impl = store == NULL ? NULL : store->impl;
    if (!turbowasm_store_is_owner(impl))
        return TURBOWASM_INVALID_ARGUMENT;
    collect(impl);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_store_get_stats(const turbowasm_store *store,
                                           turbowasm_store_stats *out) {
    turbowasm_store_impl *impl = store == NULL ? NULL : store->impl;
    if (out == NULL || !turbowasm_store_is_owner(impl))
        return TURBOWASM_INVALID_ARGUMENT;
    *out = impl->stats;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_gc_allocate(turbowasm_store_impl *store,
                                       const turbowasm_validation_func_type *type, uint32_t count,
                                       turbowasm_value *out) {
    const size_t alignment = _Alignof(turbowasm_value);
    const size_t offset = (sizeof(turbowasm_gc_object) + alignment - 1u) & ~(alignment - 1u);
    uint64_t bytes = offset + (uint64_t)count * sizeof(turbowasm_value);
    turbowasm_gc_object *object;
    turbowasm_runtime_scope scope;
    uint32_t index;
    if (out == NULL || !turbowasm_store_is_owner(store))
        return TURBOWASM_INVALID_ARGUMENT;
    if (bytes > store->config.max_bytes || bytes > SIZE_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if (store->free_head == UINT32_MAX || bytes > store->config.max_bytes - store->stats.bytes)
        collect(store);
    if (store->free_head == UINT32_MAX || bytes > store->config.max_bytes - store->stats.bytes)
        return TURBOWASM_OUT_OF_MEMORY;
    scope = turbowasm_runtime_scope_enter(&store->config.runtime);
    object = turbowasm_rt_calloc(1u, (size_t)bytes);
    turbowasm_runtime_scope_leave(scope);
    if (object == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    object->type = type;
    object->values = (turbowasm_value *)((uint8_t *)object + offset);
    object->count = count;
    object->bytes = (size_t)bytes;
    index = store->free_head;
    store->free_head = store->slots[index].next_free;
    store->slots[index].object = object;
    store->stats.bytes += (size_t)bytes;
    ++store->stats.objects;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_VALUE_GCREF;
    out->as.gcref.store = store;
    out->as.gcref.handle = ((uint64_t)store->slots[index].generation << 32u) | (index + 1u);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_root_retain(turbowasm_store *store, const turbowasm_value *value,
                                       turbowasm_root *root) {
    turbowasm_store_impl *impl = store == NULL ? NULL : store->impl;
    turbowasm_runtime_scope scope;
    tw_root *retained;
    if (root == NULL || root->impl != NULL || value == NULL || !turbowasm_store_is_owner(impl))
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_gc_value_valid(impl, value))
        return TURBOWASM_TYPE_MISMATCH;
    if (impl->stats.roots >= impl->config.max_roots - impl->source_count ||
        sizeof(*retained) > impl->config.max_bytes - impl->stats.bytes)
        return TURBOWASM_OUT_OF_MEMORY;
    scope = turbowasm_runtime_scope_enter(&impl->config.runtime);
    retained = turbowasm_rt_malloc(sizeof(*retained));
    turbowasm_runtime_scope_leave(scope);
    if (retained == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    retained->value = *value;
    retained->store = impl;
    retained->next = impl->roots;
    impl->roots = retained;
    ++impl->stats.roots;
    impl->stats.bytes += sizeof(*retained);
    root->impl = retained;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_root_get(const turbowasm_root *root, turbowasm_value *out) {
    const tw_root *impl = root == NULL ? NULL : root->impl;
    if (impl == NULL || out == NULL || !turbowasm_store_is_owner(impl->store))
        return TURBOWASM_INVALID_ARGUMENT;
    *out = impl->value;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_root_release(turbowasm_root *root) {
    tw_root *impl = root == NULL ? NULL : root->impl;
    tw_root **link;
    if (impl == NULL || !turbowasm_store_is_owner(impl->store))
        return TURBOWASM_INVALID_ARGUMENT;
    for (link = &impl->store->roots; *link != NULL; link = &(*link)->next)
        if (*link == impl) {
            *link = impl->next;
            --impl->store->stats.roots;
            impl->store->stats.bytes -= sizeof(*impl);
            turbowasm_rt_free(impl);
            root->impl = NULL;
            return TURBOWASM_OK;
        }
    return TURBOWASM_INVALID_ARGUMENT;
}

turbowasm_status turbowasm_store_attach(turbowasm_store_impl *store,
                                        turbowasm_instance_impl *instance) {
    const turbowasm_validation_context *source =
        &turbowasm_module_impl_get(instance->module)->validation;
    tw_store_types *entry;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    size_t bytes;
    uint32_t i;
    if (!turbowasm_store_is_owner(store))
        return TURBOWASM_INVALID_ARGUMENT;
    /* Repeated instantiation shares immutable type metadata, including across
     * separately loaded modules with equivalent recursion-group projections. */
    for (entry = store->types; entry != NULL; entry = entry->next) {
        if (entry->types.type_count != source->type_count)
            continue;
        for (i = 0u; i < source->type_count; ++i)
            if (!turbowasm_validation_defined_type_equal(&source->types[i], &entry->types.types[i]))
                break;
        if (i == source->type_count)
            goto attach;
    }
    if (!turbowasm_validation_types_size(source, &bytes) || bytes > SIZE_MAX - sizeof(*entry) ||
        bytes + sizeof(*entry) > store->config.max_bytes - store->stats.bytes)
        return TURBOWASM_OUT_OF_MEMORY;
    scope = turbowasm_runtime_scope_enter(&store->config.runtime);
    entry = turbowasm_rt_calloc(1u, sizeof(*entry));
    status = entry == NULL ? TURBOWASM_OUT_OF_MEMORY
                           : turbowasm_validation_clone_types(source, &entry->types);
    turbowasm_runtime_scope_leave(scope);
    if (status != TURBOWASM_OK) {
        turbowasm_rt_free(entry);
        return status;
    }
    entry->next = store->types;
    store->types = entry;
    store->stats.bytes += bytes + sizeof(*entry);
    for (i = 0u; i < entry->types.type_count;) {
        tw_store_types *prior;
        uint32_t j, k, count = entry->types.types[i].group_count;
        for (prior = entry->next; prior != NULL; prior = prior->next) {
            for (j = 0u; j < prior->types.type_count; j += prior->types.types[j].group_count)
                if (turbowasm_validation_defined_type_equal(&entry->types.types[i],
                                                            &prior->types.types[j])) {
                    for (k = 0u; k < count; ++k)
                        entry->types.types[i + k].canonical = prior->types.types[j + k].canonical;
                    break;
                }
            if (j < prior->types.type_count)
                break;
        }
        i += count;
    }
attach:
    instance->store_types = &entry->types;
    instance->store = store;
    instance->store_next = store->instances;
    store->instances = instance;
    return TURBOWASM_OK;
}

void turbowasm_store_detach(turbowasm_instance_impl *instance) {
    turbowasm_instance_impl **link;
    if (instance->store == NULL)
        return;
    for (link = &instance->store->instances; *link != NULL; link = &(*link)->store_next)
        if (*link == instance) {
            *link = instance->store_next;
            instance->store = NULL;
            instance->store_next = NULL;
            instance->store_types = NULL;
            return;
        }
}

turbowasm_status turbowasm_store_destroy(turbowasm_store *store) {
    turbowasm_store_impl *impl = store == NULL ? NULL : store->impl;
    uint32_t i;
    if (!turbowasm_store_is_owner(impl) || impl->instances != NULL || impl->roots != NULL ||
        impl->sources != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0u; i < impl->config.max_objects; ++i)
        turbowasm_rt_free(impl->slots[i].object);
    while (impl->types != NULL) {
        tw_store_types *entry = impl->types;
        impl->types = entry->next;
        turbowasm_validation_context_destroy(&entry->types);
        turbowasm_rt_free(entry);
    }
    turbowasm_rt_free(impl->slots);
    turbowasm_rt_free(impl->worklist);
    turbowasm_rt_free(impl);
    store->impl = NULL;
    return TURBOWASM_OK;
}
