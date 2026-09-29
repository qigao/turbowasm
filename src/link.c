#include <turbowasm/link.h>
#include <turbowasm/instance.h>

#include "instance_internal.h"
#include "link_internal.h"
#include "module_internal.h"
#include "validation_context.h"
#include "runtime_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_linker_entry {
    uint8_t *module_name;
    uint32_t module_name_size;
    turbowasm_instance_impl *instance;
} turbowasm_linker_entry;

typedef struct turbowasm_linker_host_function {
    uint8_t *module_name;
    uint32_t module_name_size;
    uint8_t *name;
    uint32_t name_size;
    turbowasm_value_kind *params;
    uint32_t param_count;
    turbowasm_value_kind *results;
    uint32_t result_count;
    turbowasm_host_function_fn function;
    void *context;
} turbowasm_linker_host_function;

typedef struct turbowasm_linker_impl {
    turbowasm_runtime_config config;
    turbowasm_linker_entry *entries;
    uint32_t count;
    uint32_t capacity;

    turbowasm_linker_host_function *host_functions;
    uint32_t host_function_count;
    uint32_t host_function_capacity;
} turbowasm_linker_impl;

static bool turbowasm_link_name_equal(
    const uint8_t *left,
    uint32_t left_size,
    turbowasm_name right) {
    if (left_size != right.size)
        return false;
    if (left_size == 0u)
        return true;
    return left != NULL && right.bytes != NULL &&
           memcmp(left, right.bytes, left_size) == 0;
}

static bool turbowasm_link_span_equal(
    turbowasm_name left,
    turbowasm_name right) {
    return turbowasm_link_name_equal(
        left.bytes, left.size, right);
}

static bool turbowasm_linker_reserve(
    turbowasm_linker_impl *impl,
    uint32_t required) {
    uint32_t next;
    turbowasm_linker_entry *grown;

    if (impl == NULL)
        return false;
    if (required <= impl->capacity)
        return true;

    next = impl->capacity == 0u ? 4u : impl->capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * (uint64_t)sizeof(*grown) >
        (uint64_t)SIZE_MAX)
        return false;

    grown = (turbowasm_linker_entry *)turbowasm_rt_realloc(
        impl->entries, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    if (next > impl->capacity) {
        memset(grown + impl->capacity, 0,
               (size_t)(next - impl->capacity) *
                   sizeof(*grown));
    }

    impl->entries = grown;
    impl->capacity = next;
    return true;
}

static bool turbowasm_linker_host_reserve(
    turbowasm_linker_impl *impl,
    uint32_t required) {
    uint32_t next;
    turbowasm_linker_host_function *grown;

    if (impl == NULL)
        return false;
    if (required <= impl->host_function_capacity)
        return true;

    next = impl->host_function_capacity == 0u
        ? 4u
        : impl->host_function_capacity;
    while (next < required) {
        if (next > UINT32_MAX / 2u) {
            next = required;
            break;
        }
        next *= 2u;
    }

    if ((uint64_t)next * (uint64_t)sizeof(*grown) >
        (uint64_t)SIZE_MAX)
        return false;

    grown = (turbowasm_linker_host_function *)turbowasm_rt_realloc(
        impl->host_functions, (size_t)next * sizeof(*grown));
    if (grown == NULL)
        return false;

    if (next > impl->host_function_capacity) {
        memset(grown + impl->host_function_capacity, 0,
               (size_t)(next - impl->host_function_capacity) *
                   sizeof(*grown));
    }

    impl->host_functions = grown;
    impl->host_function_capacity = next;
    return true;
}

static bool turbowasm_host_value_kind_valid(
    turbowasm_value_kind kind) {
    switch (kind) {
        case TURBOWASM_VALUE_I32:
        case TURBOWASM_VALUE_I64:
        case TURBOWASM_VALUE_F32:
        case TURBOWASM_VALUE_F64:
        case TURBOWASM_VALUE_V128:
        case TURBOWASM_VALUE_FUNCREF:
        case TURBOWASM_VALUE_EXTERNREF:
        case TURBOWASM_VALUE_EXNREF:
            return true;
        default:
            return false;
    }
}

turbowasm_status turbowasm_linker_init(
    turbowasm_linker *linker) {
    return turbowasm_linker_init_with_config(linker, NULL);
}

turbowasm_status turbowasm_linker_init_with_config(
    turbowasm_linker *linker,
    const turbowasm_runtime_config *config) {
    turbowasm_linker_impl *impl;
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;

    if (linker == NULL || linker->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(&normalized);
    impl = (turbowasm_linker_impl *)turbowasm_rt_calloc(
        1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->config = normalized;
    linker->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_linker_destroy(
    turbowasm_linker *linker) {
    turbowasm_linker_impl *impl;
    uint32_t index;

    if (linker == NULL || linker->impl == NULL)
        return;

    impl = (turbowasm_linker_impl *)linker->impl;
    for (index = 0u; index < impl->count; ++index)
        turbowasm_rt_free(impl->entries[index].module_name);
    for (index = 0u; index < impl->host_function_count; ++index) {
        turbowasm_rt_free(impl->host_functions[index].module_name);
        turbowasm_rt_free(impl->host_functions[index].name);
        turbowasm_rt_free(impl->host_functions[index].params);
        turbowasm_rt_free(impl->host_functions[index].results);
    }
    turbowasm_rt_free(impl->host_functions);
    turbowasm_rt_free(impl->entries);
    turbowasm_rt_free(impl);
    linker->impl = NULL;
}

turbowasm_status turbowasm_linker_define_instance(
    turbowasm_linker *linker,
    turbowasm_name module_name,
    struct turbowasm_instance *instance) {
    turbowasm_linker_impl *impl;
    turbowasm_linker_entry *entry;
    uint8_t *name_copy = NULL;
    uint32_t index;

    if (linker == NULL || linker->impl == NULL ||
        instance == NULL || instance->impl == NULL ||
        (module_name.size != 0u && module_name.bytes == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_linker_impl *)linker->impl;
    for (index = 0u; index < impl->count; ++index) {
        if (turbowasm_link_name_equal(
                impl->entries[index].module_name,
                impl->entries[index].module_name_size,
                module_name))
            return TURBOWASM_LINK_ERROR;
    }

    for (index = 0u; index < impl->host_function_count; ++index) {
        if (turbowasm_link_name_equal(
                impl->host_functions[index].module_name,
                impl->host_functions[index].module_name_size,
                module_name))
            return TURBOWASM_LINK_ERROR;
    }

    {
        turbowasm_runtime_scope scope =
            turbowasm_runtime_scope_enter(&impl->config);

        if (module_name.size != 0u) {
            name_copy = (uint8_t *)turbowasm_rt_malloc(
                module_name.size);
            if (name_copy == NULL) {
                turbowasm_runtime_scope_leave(scope);
                return TURBOWASM_OUT_OF_MEMORY;
            }
            memcpy(name_copy, module_name.bytes, module_name.size);
        }

        if (impl->count == UINT32_MAX ||
            !turbowasm_linker_reserve(
                impl, impl->count + 1u)) {
            turbowasm_rt_free(name_copy);
            turbowasm_runtime_scope_leave(scope);
            return TURBOWASM_OUT_OF_MEMORY;
        }
        turbowasm_runtime_scope_leave(scope);
    }

    entry = &impl->entries[impl->count++];
    entry->module_name = name_copy;
    entry->module_name_size = module_name.size;
    entry->instance = (turbowasm_instance_impl *)instance->impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_linker_define_host_function(
    turbowasm_linker *linker,
    turbowasm_name module_name,
    turbowasm_name name,
    const turbowasm_host_function_type *type,
    turbowasm_host_function_fn function,
    void *context) {
    turbowasm_linker_impl *impl;
    turbowasm_linker_host_function *entry;
    uint8_t *module_copy = NULL;
    uint8_t *name_copy = NULL;
    turbowasm_value_kind *params = NULL;
    turbowasm_value_kind *results = NULL;
    uint32_t index;
    turbowasm_runtime_scope scope;

    if (linker == NULL || linker->impl == NULL ||
        type == NULL || function == NULL ||
        (module_name.size != 0u && module_name.bytes == NULL) ||
        (name.size != 0u && name.bytes == NULL) ||
        (type->param_count != 0u && type->params == NULL) ||
        (type->result_count != 0u && type->results == NULL) ||
        type->param_count > UINT32_MAX ||
        type->result_count > UINT32_MAX)
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < (uint32_t)type->param_count; ++index) {
        if (!turbowasm_host_value_kind_valid(type->params[index]))
            return TURBOWASM_INVALID_ARGUMENT;
    }
    for (index = 0u; index < (uint32_t)type->result_count; ++index) {
        if (!turbowasm_host_value_kind_valid(type->results[index]))
            return TURBOWASM_INVALID_ARGUMENT;
    }

    impl = (turbowasm_linker_impl *)linker->impl;
    for (index = 0u; index < impl->count; ++index) {
        if (turbowasm_link_name_equal(
                impl->entries[index].module_name,
                impl->entries[index].module_name_size,
                module_name))
            return TURBOWASM_LINK_ERROR;
    }
    for (index = 0u; index < impl->host_function_count; ++index) {
        turbowasm_linker_host_function *existing =
            &impl->host_functions[index];
        if (turbowasm_link_name_equal(
                existing->module_name,
                existing->module_name_size,
                module_name) &&
            turbowasm_link_name_equal(
                existing->name,
                existing->name_size,
                name))
            return TURBOWASM_LINK_ERROR;
    }

    scope = turbowasm_runtime_scope_enter(&impl->config);
    if (module_name.size != 0u) {
        module_copy = (uint8_t *)turbowasm_rt_malloc(module_name.size);
        if (module_copy == NULL)
            goto out_of_memory;
        memcpy(module_copy, module_name.bytes, module_name.size);
    }
    if (name.size != 0u) {
        name_copy = (uint8_t *)turbowasm_rt_malloc(name.size);
        if (name_copy == NULL)
            goto out_of_memory;
        memcpy(name_copy, name.bytes, name.size);
    }
    if (type->param_count != 0u) {
        params = (turbowasm_value_kind *)turbowasm_rt_malloc(
            type->param_count * sizeof(*params));
        if (params == NULL)
            goto out_of_memory;
        memcpy(params, type->params, type->param_count * sizeof(*params));
    }
    if (type->result_count != 0u) {
        results = (turbowasm_value_kind *)turbowasm_rt_malloc(
            type->result_count * sizeof(*results));
        if (results == NULL)
            goto out_of_memory;
        memcpy(results, type->results, type->result_count * sizeof(*results));
    }
    if (impl->host_function_count == UINT32_MAX ||
        !turbowasm_linker_host_reserve(
            impl, impl->host_function_count + 1u))
        goto out_of_memory;

    entry = &impl->host_functions[impl->host_function_count++];
    entry->module_name = module_copy;
    entry->module_name_size = module_name.size;
    entry->name = name_copy;
    entry->name_size = name.size;
    entry->params = params;
    entry->param_count = (uint32_t)type->param_count;
    entry->results = results;
    entry->result_count = (uint32_t)type->result_count;
    entry->function = function;
    entry->context = context;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

out_of_memory:
    turbowasm_rt_free(results);
    turbowasm_rt_free(params);
    turbowasm_rt_free(name_copy);
    turbowasm_rt_free(module_copy);
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OUT_OF_MEMORY;
}

static const turbowasm_linker_host_function *
turbowasm_linker_find_host_function(
    const turbowasm_linker_impl *impl,
    turbowasm_name module_name,
    turbowasm_name name) {
    uint32_t index;

    if (impl == NULL)
        return NULL;

    for (index = 0u; index < impl->host_function_count; ++index) {
        const turbowasm_linker_host_function *entry =
            &impl->host_functions[index];
        if (turbowasm_link_name_equal(
                entry->module_name,
                entry->module_name_size,
                module_name) &&
            turbowasm_link_name_equal(
                entry->name,
                entry->name_size,
                name))
            return entry;
    }
    return NULL;
}

static const turbowasm_linker_entry *
turbowasm_linker_find_module(
    const turbowasm_linker_impl *impl,
    turbowasm_name module_name) {
    uint32_t index;

    if (impl == NULL)
        return NULL;

    for (index = 0u; index < impl->count; ++index) {
        if (turbowasm_link_name_equal(
                impl->entries[index].module_name,
                impl->entries[index].module_name_size,
                module_name))
            return &impl->entries[index];
    }
    return NULL;
}

static bool turbowasm_link_limits_match(
    turbowasm_validation_limits expected,
    turbowasm_instance_limits actual) {
    if (actual.minimum < expected.minimum)
        return false;
    if (expected.has_maximum) {
        if (!actual.has_maximum)
            return false;
        if (actual.maximum > expected.maximum)
            return false;
    }
    return true;
}

static bool turbowasm_link_host_type_matches(
    const turbowasm_validation_func_type *expected,
    const turbowasm_linker_host_function *host) {
    uint32_t index;

    if (expected == NULL || !expected->defined || host == NULL ||
        expected->param_count != host->param_count ||
        expected->result_count != host->result_count)
        return false;

    for (index = 0u; index < expected->param_count; ++index) {
        if (expected->params[index] != (uint8_t)host->params[index])
            return false;
    }
    for (index = 0u; index < expected->result_count; ++index) {
        if (expected->results[index] != (uint8_t)host->results[index])
            return false;
    }
    return true;
}

static const turbowasm_export_desc *
turbowasm_linker_find_export(
    const turbowasm_module *module,
    turbowasm_name name) {
    size_t index;
    size_t count = turbowasm_module_export_count(module);

    for (index = 0u; index < count; ++index) {
        const turbowasm_export_desc *export_desc =
            turbowasm_module_export_at(module, index);
        if (export_desc != NULL &&
            turbowasm_link_span_equal(export_desc->name, name))
            return export_desc;
    }
    return NULL;
}

turbowasm_status turbowasm_linker_bind_instance(
    turbowasm_instance_impl *instance,
    const turbowasm_module_impl *module,
    const turbowasm_linker *linker) {
    const turbowasm_linker_impl *linker_impl;
    turbowasm_linked_function *function_bindings = NULL;
    turbowasm_linked_global *global_bindings = NULL;
    turbowasm_linked_memory *memory_bindings = NULL;
    turbowasm_linked_table *table_bindings = NULL;
    turbowasm_linked_tag *tag_bindings = NULL;
    turbowasm_status result = TURBOWASM_OK;
    uint32_t import_index;

    if (instance == NULL || module == NULL ||
        linker == NULL || linker->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    linker_impl = (const turbowasm_linker_impl *)linker->impl;

    if (module->summary.imported_function_count != 0u) {
        function_bindings = (turbowasm_linked_function *)turbowasm_rt_calloc(
            (size_t)module->summary.imported_function_count,
            sizeof(*function_bindings));
        if (function_bindings == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (module->summary.imported_global_count != 0u) {
        global_bindings = (turbowasm_linked_global *)turbowasm_rt_calloc(
            (size_t)module->summary.imported_global_count,
            sizeof(*global_bindings));
        if (global_bindings == NULL) {
            turbowasm_rt_free(function_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (module->summary.imported_memory_count != 0u) {
        memory_bindings = (turbowasm_linked_memory *)turbowasm_rt_calloc(
            (size_t)module->summary.imported_memory_count,
            sizeof(*memory_bindings));
        if (memory_bindings == NULL) {
            turbowasm_rt_free(function_bindings);
            turbowasm_rt_free(global_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (module->summary.imported_table_count != 0u) {
        table_bindings = (turbowasm_linked_table *)turbowasm_rt_calloc(
            (size_t)module->summary.imported_table_count,
            sizeof(*table_bindings));
        if (table_bindings == NULL) {
            turbowasm_rt_free(function_bindings);
            turbowasm_rt_free(global_bindings);
            turbowasm_rt_free(memory_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (module->summary.imported_tag_count != 0u) {
        tag_bindings = (turbowasm_linked_tag *)turbowasm_rt_calloc(
            (size_t)module->summary.imported_tag_count,
            sizeof(*tag_bindings));
        if (tag_bindings == NULL) {
            turbowasm_rt_free(function_bindings);
            turbowasm_rt_free(global_bindings);
            turbowasm_rt_free(memory_bindings);
            turbowasm_rt_free(table_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    for (import_index = 0u;
         import_index < module->validation.import_count;
         ++import_index) {
        const turbowasm_import_desc *import_desc =
            &module->validation.imports[import_index];
        const turbowasm_linker_entry *provider_entry;
        const turbowasm_linker_host_function *host_function = NULL;
        const turbowasm_module_impl *provider_module;
        const turbowasm_export_desc *export_desc;

        provider_entry = turbowasm_linker_find_module(
            linker_impl, import_desc->module_name);

        if (import_desc->kind == TURBOWASM_EXTERN_FUNCTION) {
            host_function = turbowasm_linker_find_host_function(
                linker_impl,
                import_desc->module_name,
                import_desc->name);

            if (provider_entry == NULL && host_function != NULL) {
                const turbowasm_validation_func_type *expected_type;

                if (import_desc->item_index >=
                    module->summary.imported_function_count) {
                    result = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }

                expected_type =
                    turbowasm_validation_context_function_type(
                        &module->validation,
                        import_desc->item_index);
                if (!turbowasm_link_host_type_matches(
                        expected_type, host_function)) {
                    result = TURBOWASM_TYPE_MISMATCH;
                    goto fail;
                }

                function_bindings[import_desc->item_index].host_function =
                    host_function->function;
                function_bindings[import_desc->item_index].host_context =
                    host_function->context;
                continue;
            }
        }

        if (provider_entry == NULL ||
            provider_entry->instance == NULL) {
            result = TURBOWASM_LINK_ERROR;
            goto fail;
        }

        provider_module = turbowasm_module_impl_get(
            provider_entry->instance->module);
        if (provider_module == NULL) {
            result = TURBOWASM_LINK_ERROR;
            goto fail;
        }

        export_desc = turbowasm_linker_find_export(
            provider_entry->instance->module,
            import_desc->name);
        if (export_desc == NULL) {
            result = TURBOWASM_LINK_ERROR;
            goto fail;
        }
        if (export_desc->kind != import_desc->kind) {
            result = TURBOWASM_TYPE_MISMATCH;
            goto fail;
        }

        if (import_desc->kind == TURBOWASM_EXTERN_FUNCTION) {
            const turbowasm_validation_func_type *expected_type;
            const turbowasm_validation_func_type *actual_type;
            const turbowasm_validation_function *provider_function;

            if (import_desc->item_index >=
                module->summary.imported_function_count) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_type =
                turbowasm_validation_context_function_type(
                    &module->validation,
                    import_desc->item_index);
            actual_type =
                turbowasm_validation_context_function_type(
                    &provider_module->validation,
                    export_desc->item_index);
            if (!turbowasm_validation_func_type_equal(
                    expected_type, actual_type)) {
                result = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }

            provider_function =
                turbowasm_validation_context_function(
                    &provider_module->validation,
                    export_desc->item_index);
            if (provider_function == NULL) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
            }
            if (provider_function->imported) {
                const turbowasm_linked_function *linked;

                if (export_desc->item_index >=
                    provider_entry->instance->linked_function_count) {
                    result = TURBOWASM_LINK_ERROR;
                    goto fail;
                }

                linked = &provider_entry->instance
                    ->linked_functions[export_desc->item_index];
                if (linked->provider == NULL &&
                    linked->host_function == NULL) {
                    result = TURBOWASM_LINK_ERROR;
                    goto fail;
                }
            }

            function_bindings[import_desc->item_index].provider =
                provider_entry->instance;
            function_bindings[import_desc->item_index].function_index =
                export_desc->item_index;
            continue;
        }

        if (import_desc->kind == TURBOWASM_EXTERN_MEMORY) {
            const turbowasm_validation_memory *expected_memory;
            const turbowasm_validation_memory *actual_memory;

            if (import_desc->item_index >=
                    module->summary.imported_memory_count ||
                export_desc->item_index >=
                    provider_module->validation.memory_count) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_memory =
                &module->validation.memories[
                    import_desc->item_index];
            actual_memory =
                &provider_module->validation.memories[
                    export_desc->item_index];

            if (actual_memory->imported &&
                (export_desc->item_index >=
                     provider_entry->instance->linked_memory_count ||
                 provider_entry->instance
                         ->linked_memories[export_desc->item_index]
                         .provider == NULL)) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
            }

            {
                turbowasm_instance_limits actual_limits;
                result = turbowasm_instance_memory_limits(
                    provider_entry->instance,
                    export_desc->item_index,
                    &actual_limits);
                if (result != TURBOWASM_OK) {
                    result = TURBOWASM_LINK_ERROR;
                    goto fail;
                }

                if (expected_memory->memory64 !=
                        actual_memory->memory64 ||
                    expected_memory->shared !=
                        actual_memory->shared ||
                    expected_memory->page_size !=
                        actual_memory->page_size) {
                    result = TURBOWASM_TYPE_MISMATCH;
                    goto fail;
                }
                if (expected_memory->memory64) {
                    result = TURBOWASM_UNSUPPORTED;
                    goto fail;
                }
                {
                    turbowasm_validation_limits expected_limits = {
                        (uint32_t)expected_memory->limits.minimum,
                        (uint32_t)expected_memory->limits.maximum,
                        expected_memory->limits.has_maximum
                    };
                    if (!turbowasm_link_limits_match(
                            expected_limits, actual_limits)) {
                        result = TURBOWASM_TYPE_MISMATCH;
                        goto fail;
                    }
                }
            }

            memory_bindings[import_desc->item_index].provider =
                provider_entry->instance;
            memory_bindings[import_desc->item_index].memory_index =
                export_desc->item_index;
            continue;
        }

        if (import_desc->kind == TURBOWASM_EXTERN_TABLE) {
            const turbowasm_validation_table *expected_table;
            const turbowasm_validation_table *actual_table;

            if (import_desc->item_index >=
                    module->summary.imported_table_count ||
                export_desc->item_index >=
                    provider_module->validation.table_count) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_table =
                &module->validation.tables[import_desc->item_index];
            actual_table =
                &provider_module->validation.tables[
                    export_desc->item_index];

            if ((expected_table->reference_type != 0x70u &&
                 expected_table->reference_type != 0x6fu) ||
                (actual_table->reference_type != 0x70u &&
                 actual_table->reference_type != 0x6fu)) {
                result = TURBOWASM_UNSUPPORTED;
                goto fail;
            }

            /*
             * Tables are mutable, so imported element types are invariant.
             * Preserve typed-funcref semantic identity instead of comparing
             * only the erased carrier byte.
             */
            if (!turbowasm_validation_value_type_equal(
                    &expected_table->semantic_type,
                    &actual_table->semantic_type)) {
                result = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }

            if (actual_table->imported &&
                (export_desc->item_index >=
                     provider_entry->instance->linked_table_count ||
                 provider_entry->instance
                         ->linked_tables[export_desc->item_index]
                         .provider == NULL)) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
            }

            {
                turbowasm_instance_limits actual_limits;
                result = turbowasm_instance_table_limits(
                    provider_entry->instance,
                    export_desc->item_index,
                    &actual_limits);
                if (result != TURBOWASM_OK) {
                    result = TURBOWASM_LINK_ERROR;
                    goto fail;
                }

                if (!turbowasm_link_limits_match(
                        expected_table->limits,
                        actual_limits)) {
                    result = TURBOWASM_TYPE_MISMATCH;
                    goto fail;
                }
            }

            table_bindings[import_desc->item_index].provider =
                provider_entry->instance;
            table_bindings[import_desc->item_index].table_index =
                export_desc->item_index;
            continue;
        }

        if (import_desc->kind == TURBOWASM_EXTERN_GLOBAL) {
            const turbowasm_validation_global *expected_global;
            const turbowasm_validation_global *actual_global;

            if (import_desc->item_index >=
                    module->summary.imported_global_count ||
                export_desc->item_index >=
                    provider_module->validation.global_count) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_global =
                &module->validation.globals[import_desc->item_index];
            actual_global =
                &provider_module->validation.globals[
                    export_desc->item_index];

            if (expected_global->mutable_value !=
                    actual_global->mutable_value ||
                !turbowasm_validation_value_type_equal(
                    &expected_global->semantic_type,
                    &actual_global->semantic_type)) {
                result = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }

            if (actual_global->imported &&
                (export_desc->item_index >=
                     provider_entry->instance->linked_global_count ||
                 provider_entry->instance
                         ->linked_globals[export_desc->item_index]
                         .provider == NULL)) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
            }

            global_bindings[import_desc->item_index].provider =
                provider_entry->instance;
            global_bindings[import_desc->item_index].global_index =
                export_desc->item_index;
            continue;
        }

        if (import_desc->kind == TURBOWASM_EXTERN_TAG) {
            const turbowasm_validation_tag *expected_tag;
            const turbowasm_validation_tag *actual_tag;
            const turbowasm_validation_func_type *expected_type;
            const turbowasm_validation_func_type *actual_type;
            turbowasm_tag_identity identity;

            if (import_desc->item_index >=
                    module->summary.imported_tag_count ||
                export_desc->item_index >=
                    provider_module->validation.tag_count) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_tag = turbowasm_validation_context_tag(
                &module->validation, import_desc->item_index);
            actual_tag = turbowasm_validation_context_tag(
                &provider_module->validation,
                export_desc->item_index);
            if (expected_tag == NULL || actual_tag == NULL) {
                result = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            expected_type = turbowasm_validation_context_type(
                &module->validation, expected_tag->type_index);
            actual_type = turbowasm_validation_context_type(
                &provider_module->validation, actual_tag->type_index);
            if (!turbowasm_validation_func_type_equal(
                    expected_type, actual_type)) {
                result = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }

            result = turbowasm_instance_tag_identity(
                provider_entry->instance,
                export_desc->item_index,
                &identity);
            if (result != TURBOWASM_OK) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
            }

            tag_bindings[import_desc->item_index].provider =
                identity.owner;
            tag_bindings[import_desc->item_index].tag_index =
                identity.tag_index;
            continue;
        }

        result = TURBOWASM_UNSUPPORTED;
        goto fail;
    }

    instance->linked_functions = function_bindings;
    instance->linked_function_count =
        module->summary.imported_function_count;
    instance->linked_globals = global_bindings;
    instance->linked_global_count =
        module->summary.imported_global_count;
    instance->linked_memories = memory_bindings;
    instance->linked_memory_count =
        module->summary.imported_memory_count;
    instance->linked_tables = table_bindings;
    instance->linked_table_count =
        module->summary.imported_table_count;
    instance->linked_tags = tag_bindings;
    instance->linked_tag_count =
        module->summary.imported_tag_count;
    return TURBOWASM_OK;

fail:
    turbowasm_rt_free(function_bindings);
    turbowasm_rt_free(global_bindings);
    turbowasm_rt_free(memory_bindings);
    turbowasm_rt_free(table_bindings);
    turbowasm_rt_free(tag_bindings);
    return result;
}
