#include "component_runtime.h"

#include "runtime_alloc.h"

#include <stddef.h>
#include <string.h>

static bool names_equal(
    turbowasm_component_name component_name,
    turbowasm_name core_name) {
    return component_name.size == core_name.size &&
           (component_name.size == 0u ||
            memcmp(
                component_name.bytes,
                core_name.bytes,
                component_name.size) == 0);
}

static bool component_names_equal(
    turbowasm_component_name left,
    turbowasm_component_name right) {
    return left.size == right.size &&
           (left.size == 0u ||
            memcmp(left.bytes, right.bytes, left.size) == 0);
}

static turbowasm_status resolve_core_function_alias(
    turbowasm_component_runtime *runtime,
    uint32_t alias_index) {
    const turbowasm_component_core_function_alias *alias;
    turbowasm_instance *instance;
    const turbowasm_module *module;
    size_t export_count;
    size_t i;

    if (runtime == NULL || runtime->binary == NULL ||
        alias_index >= runtime->core_function_count)
        return TURBOWASM_INVALID_ARGUMENT;

    alias = turbowasm_component_binary_core_function_alias_at(
        runtime->binary, alias_index);
    if (alias == NULL ||
        alias->instance_index >= runtime->core_instance_count)
        return TURBOWASM_MALFORMED_MODULE;

    instance = &runtime->core_instances[alias->instance_index];
    module = turbowasm_instance_module(instance);
    if (module == NULL)
        return TURBOWASM_MALFORMED_MODULE;

    export_count = turbowasm_module_export_count(module);
    for (i = 0u; i < export_count; ++i) {
        const turbowasm_export_desc *desc =
            turbowasm_module_export_at(module, i);

        if (desc == NULL ||
            desc->kind != TURBOWASM_EXTERN_FUNCTION ||
            !names_equal(alias->name, desc->name))
            continue;

        runtime->core_functions[alias_index].instance = instance;
        runtime->core_functions[alias_index].function_index =
            desc->item_index;
        return TURBOWASM_OK;
    }

    return TURBOWASM_TYPE_MISMATCH;
}

static void runtime_dispose_partial(
    turbowasm_component_runtime *runtime) {
    uint32_t i;

    if (runtime == NULL)
        return;

    if (runtime->functions != NULL) {
        for (i = 0u; i < runtime->function_count; ++i)
            turbowasm_component_core_call_adapter_destroy(
                &runtime->functions[i]);
    }

    if (runtime->core_instances != NULL) {
        for (i = 0u; i < runtime->core_instance_count; ++i)
            turbowasm_instance_destroy(
                &runtime->core_instances[i]);
    }

    if (runtime->core_modules != NULL) {
        for (i = 0u; i < runtime->core_module_count; ++i)
            turbowasm_module_destroy(
                &runtime->core_modules[i]);
    }

    turbowasm_rt_free(runtime->functions);
    turbowasm_rt_free(runtime->core_functions);
    turbowasm_rt_free(runtime->core_instances);
    turbowasm_rt_free(runtime->core_modules);
    memset(runtime, 0, sizeof(*runtime));
}

turbowasm_status turbowasm_component_runtime_init(
    turbowasm_component_runtime *runtime,
    const turbowasm_component_binary *binary) {
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t i;

    if (runtime == NULL || binary == NULL ||
        runtime->initialized || runtime->binary != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * C5c1 has no Component host-import composition yet. Supporting imports
     * without explicit bindings would silently shift the Component function
     * index space, so fail closed.
     */
    if (binary->import_count != 0u)
        return TURBOWASM_UNSUPPORTED;

    scope = turbowasm_runtime_scope_enter(&binary->config);
    memset(runtime, 0, sizeof(*runtime));
    runtime->binary = binary;

    runtime->core_module_count = binary->core_module_count;
    runtime->core_instance_count = binary->core_instance_count;
    runtime->core_function_count =
        binary->core_function_alias_count;
    runtime->function_count = binary->canon_lift_count;

    if (runtime->core_module_count != 0u) {
        runtime->core_modules = (turbowasm_module *)
            turbowasm_rt_calloc(
                runtime->core_module_count,
                sizeof(*runtime->core_modules));
        if (runtime->core_modules == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    if (runtime->core_instance_count != 0u) {
        runtime->core_instances = (turbowasm_instance *)
            turbowasm_rt_calloc(
                runtime->core_instance_count,
                sizeof(*runtime->core_instances));
        if (runtime->core_instances == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    if (runtime->core_function_count != 0u) {
        runtime->core_functions =
            (turbowasm_component_core_function_ref *)
            turbowasm_rt_calloc(
                runtime->core_function_count,
                sizeof(*runtime->core_functions));
        if (runtime->core_functions == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    if (runtime->function_count != 0u) {
        runtime->functions =
            (turbowasm_component_core_call_adapter *)
            turbowasm_rt_calloc(
                runtime->function_count,
                sizeof(*runtime->functions));
        if (runtime->functions == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    for (i = 0u; i < runtime->core_module_count; ++i) {
        const turbowasm_component_core_module *record =
            turbowasm_component_binary_core_module_at(binary, i);
        if (record == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        status = turbowasm_module_load_borrowed_with_config(
            &runtime->core_modules[i],
            record->bytes,
            record->size,
            &binary->config);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    for (i = 0u; i < runtime->core_instance_count; ++i) {
        const turbowasm_component_core_instance *record =
            turbowasm_component_binary_core_instance_at(binary, i);
        if (record == NULL ||
            record->module_index >= runtime->core_module_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        status = turbowasm_instance_create(
            &runtime->core_instances[i],
            &runtime->core_modules[record->module_index]);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    for (i = 0u; i < runtime->core_function_count; ++i) {
        status = resolve_core_function_alias(runtime, i);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    for (i = 0u; i < runtime->function_count; ++i) {
        const turbowasm_component_canon_lift *record =
            turbowasm_component_binary_canon_lift_at(binary, i);
        const turbowasm_component_core_function_ref *core;

        if (record == NULL ||
            record->core_function_index >=
                runtime->core_function_count ||
            record->type_index >= binary->type_graph.count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        core = &runtime->core_functions[
            record->core_function_index];
        if (core->instance == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        status = turbowasm_component_core_call_adapter_init(
            &runtime->functions[i],
            &binary->type_graph,
            record->type_index,
            core->instance,
            core->function_index,
            NULL);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    /*
     * With no Component imports, the function index space is exactly the
     * canon-lift sequence. Validate every retained function export now.
     */
    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            turbowasm_component_binary_export_at(binary, i);
        if (export_desc == NULL ||
            export_desc->kind != TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->item_index >= runtime->function_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }
        if (export_desc->has_ascribed_type) {
            const turbowasm_component_canon_lift *lift =
                turbowasm_component_binary_canon_lift_at(
                    binary, export_desc->item_index);
            if (lift == NULL ||
                lift->type_index != export_desc->type_index) {
                status = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }
        }
    }

    runtime->initialized = true;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

fail:
    runtime_dispose_partial(runtime);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

void turbowasm_component_runtime_destroy(
    turbowasm_component_runtime *runtime) {
    turbowasm_runtime_scope scope;

    if (runtime == NULL)
        return;
    if (runtime->binary == NULL) {
        memset(runtime, 0, sizeof(*runtime));
        return;
    }

    scope = turbowasm_runtime_scope_enter(
        &runtime->binary->config);
    runtime_dispose_partial(runtime);
    turbowasm_runtime_scope_leave(scope);
}

turbowasm_status turbowasm_component_runtime_invoke_export(
    turbowasm_component_runtime *runtime,
    turbowasm_component_name export_name,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    uint32_t i;

    if (runtime == NULL || !runtime->initialized ||
        runtime->binary == NULL || trap == NULL ||
        (export_name.size != 0u && export_name.bytes == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < runtime->binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            turbowasm_component_binary_export_at(
                runtime->binary, i);
        if (export_desc == NULL ||
            export_desc->kind != TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            !component_names_equal(export_desc->name, export_name))
            continue;
        if (export_desc->item_index >= runtime->function_count)
            return TURBOWASM_MALFORMED_MODULE;

        return turbowasm_component_core_call_invoke(
            &runtime->functions[export_desc->item_index],
            arguments,
            argument_count,
            out_result,
            trap);
    }

    return TURBOWASM_INVALID_ARGUMENT;
}
