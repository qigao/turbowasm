#include <turbowasm/link.h>
#include <turbowasm/instance.h>

#include "instance_internal.h"
#include "link_internal.h"
#include "module_internal.h"
#include "validation_context.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_linker_entry {
    uint8_t *module_name;
    uint32_t module_name_size;
    turbowasm_instance_impl *instance;
} turbowasm_linker_entry;

typedef struct turbowasm_linker_impl {
    turbowasm_linker_entry *entries;
    uint32_t count;
    uint32_t capacity;
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

    grown = (turbowasm_linker_entry *)realloc(
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

turbowasm_status turbowasm_linker_init(
    turbowasm_linker *linker) {
    turbowasm_linker_impl *impl;

    if (linker == NULL || linker->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_linker_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

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
        free(impl->entries[index].module_name);
    free(impl->entries);
    free(impl);
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

    if (module_name.size != 0u) {
        name_copy = (uint8_t *)malloc(module_name.size);
        if (name_copy == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
        memcpy(name_copy, module_name.bytes, module_name.size);
    }

    if (impl->count == UINT32_MAX ||
        !turbowasm_linker_reserve(
            impl, impl->count + 1u)) {
        free(name_copy);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    entry = &impl->entries[impl->count++];
    entry->module_name = name_copy;
    entry->module_name_size = module_name.size;
    entry->instance = (turbowasm_instance_impl *)instance->impl;
    return TURBOWASM_OK;
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
    turbowasm_status result = TURBOWASM_OK;
    uint32_t import_index;

    if (instance == NULL || module == NULL ||
        linker == NULL || linker->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    linker_impl = (const turbowasm_linker_impl *)linker->impl;

    if (module->summary.imported_function_count != 0u) {
        function_bindings = (turbowasm_linked_function *)calloc(
            (size_t)module->summary.imported_function_count,
            sizeof(*function_bindings));
        if (function_bindings == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }

    if (module->summary.imported_global_count != 0u) {
        global_bindings = (turbowasm_linked_global *)calloc(
            (size_t)module->summary.imported_global_count,
            sizeof(*global_bindings));
        if (global_bindings == NULL) {
            free(function_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (module->summary.imported_memory_count != 0u) {
        memory_bindings = (turbowasm_linked_memory *)calloc(
            (size_t)module->summary.imported_memory_count,
            sizeof(*memory_bindings));
        if (memory_bindings == NULL) {
            free(function_bindings);
            free(global_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    if (module->summary.imported_table_count != 0u) {
        table_bindings = (turbowasm_linked_table *)calloc(
            (size_t)module->summary.imported_table_count,
            sizeof(*table_bindings));
        if (table_bindings == NULL) {
            free(function_bindings);
            free(global_bindings);
            free(memory_bindings);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    for (import_index = 0u;
         import_index < module->validation.import_count;
         ++import_index) {
        const turbowasm_import_desc *import_desc =
            &module->validation.imports[import_index];
        const turbowasm_linker_entry *provider_entry;
        const turbowasm_module_impl *provider_module;
        const turbowasm_export_desc *export_desc;

        provider_entry = turbowasm_linker_find_module(
            linker_impl, import_desc->module_name);
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
            if (provider_function->imported &&
                (export_desc->item_index >=
                     provider_entry->instance->linked_function_count ||
                 provider_entry->instance
                         ->linked_functions[export_desc->item_index]
                         .provider == NULL)) {
                result = TURBOWASM_LINK_ERROR;
                goto fail;
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

                if (!turbowasm_link_limits_match(
                        expected_memory->limits,
                        actual_limits)) {
                    result = TURBOWASM_TYPE_MISMATCH;
                    goto fail;
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

            if (expected_global->value_type != actual_global->value_type ||
                expected_global->mutable_value !=
                    actual_global->mutable_value) {
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
    return TURBOWASM_OK;

fail:
    free(function_bindings);
    free(global_bindings);
    free(memory_bindings);
    free(table_bindings);
    return result;
}
