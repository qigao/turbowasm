#include "component_exec.h"

#include "runtime_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool component_name_equal(
    turbowasm_component_name left,
    const uint8_t *right,
    uint32_t right_size) {
    return left.size == right_size &&
           (right_size == 0u ||
            (left.bytes != NULL && right != NULL &&
             memcmp(left.bytes, right, right_size) == 0));
}

static bool component_name_equal_core(
    turbowasm_component_name left,
    turbowasm_name right) {
    return left.size == right.size &&
           (left.size == 0u ||
            (left.bytes != NULL && right.bytes != NULL &&
             memcmp(left.bytes, right.bytes, left.size) == 0));
}

static const turbowasm_export_desc *find_core_function_export(
    const turbowasm_module *module,
    turbowasm_component_name name) {
    size_t i;
    size_t count;

    if (module == NULL)
        return NULL;

    count = turbowasm_module_export_count(module);
    for (i = 0u; i < count; ++i) {
        const turbowasm_export_desc *export_desc =
            turbowasm_module_export_at(module, i);
        if (export_desc != NULL &&
            export_desc->kind == TURBOWASM_EXTERN_FUNCTION &&
            component_name_equal_core(name, export_desc->name))
            return export_desc;
    }
    return NULL;
}

static void destroy_partial(
    turbowasm_component_exec *exec) {
    uint32_t i;

    if (exec == NULL)
        return;

    if (exec->functions != NULL) {
        for (i = 0u; i < exec->function_count; ++i)
            turbowasm_component_core_call_adapter_destroy(
                &exec->functions[i]);
    }

    if (exec->core_instances != NULL) {
        for (i = exec->core_instance_count; i != 0u; --i)
            turbowasm_instance_destroy(
                &exec->core_instances[i - 1u]);
    }

    if (exec->core_modules != NULL) {
        for (i = exec->core_module_count; i != 0u; --i)
            turbowasm_module_destroy(
                &exec->core_modules[i - 1u]);
    }

    turbowasm_rt_free(exec->functions);
    turbowasm_rt_free(exec->core_functions);
    turbowasm_rt_free(exec->core_instances);
    turbowasm_rt_free(exec->core_modules);
    memset(exec, 0, sizeof(*exec));
}

turbowasm_status turbowasm_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary) {
    turbowasm_runtime_scope scope;
    turbowasm_status status = TURBOWASM_OK;
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        exec->initialized || exec->binary != NULL ||
        binary->bytes == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * C5c1 has no Component import binding surface yet. Imports are retained
     * by C2b but executable composition begins with a closed Component.
     */
    if (binary->import_count != 0u)
        return TURBOWASM_UNSUPPORTED;

    scope = turbowasm_runtime_scope_enter(&binary->config);
    exec->binary = binary;

    if (binary->core_module_count != 0u) {
        if ((size_t)binary->core_module_count >
            SIZE_MAX / sizeof(*exec->core_modules)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_modules = (turbowasm_module *)turbowasm_rt_calloc(
            binary->core_module_count,
            sizeof(*exec->core_modules));
        if (exec->core_modules == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    for (i = 0u; i < binary->core_module_count; ++i) {
        const turbowasm_component_core_module *record =
            &binary->core_modules[i];
        status = turbowasm_module_load_borrowed_with_config(
            &exec->core_modules[i],
            record->bytes,
            record->size,
            &binary->config);
        if (status != TURBOWASM_OK)
            goto fail;
        ++exec->core_module_count;
    }

    if (binary->core_instance_count != 0u) {
        if ((size_t)binary->core_instance_count >
            SIZE_MAX / sizeof(*exec->core_instances)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_instances =
            (turbowasm_instance *)turbowasm_rt_calloc(
                binary->core_instance_count,
                sizeof(*exec->core_instances));
        if (exec->core_instances == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }

    for (i = 0u; i < binary->core_instance_count; ++i) {
        const turbowasm_component_core_instance_def *definition =
            &binary->core_instances[i];

        if (definition->module_index >= exec->core_module_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        status = turbowasm_instance_create(
            &exec->core_instances[i],
            &exec->core_modules[definition->module_index]);
        if (status != TURBOWASM_OK)
            goto fail;
        ++exec->core_instance_count;
    }

    if (binary->core_function_alias_count != 0u) {
        if ((size_t)binary->core_function_alias_count >
            SIZE_MAX / sizeof(*exec->core_functions)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_functions =
            (turbowasm_component_exec_core_function *)
                turbowasm_rt_calloc(
                    binary->core_function_alias_count,
                    sizeof(*exec->core_functions));
        if (exec->core_functions == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }
    exec->core_function_count =
        binary->core_function_alias_count;

    for (i = 0u; i < binary->core_function_alias_count; ++i) {
        const turbowasm_component_core_function_alias *alias =
            &binary->core_function_aliases[i];
        const turbowasm_export_desc *export_desc;
        const turbowasm_module *module;

        if (alias->core_function_index >= exec->core_function_count ||
            alias->instance_index >= exec->core_instance_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        module = turbowasm_instance_module(
            &exec->core_instances[alias->instance_index]);
        export_desc = find_core_function_export(module, alias->name);
        if (export_desc == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        exec->core_functions[alias->core_function_index].instance_index =
            alias->instance_index;
        exec->core_functions[alias->core_function_index].function_index =
            export_desc->item_index;
    }

    if (binary->canon_lift_count != 0u) {
        if ((size_t)binary->canon_lift_count >
            SIZE_MAX / sizeof(*exec->functions)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->functions =
            (turbowasm_component_core_call_adapter *)
                turbowasm_rt_calloc(
                    binary->canon_lift_count,
                    sizeof(*exec->functions));
        if (exec->functions == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }
    exec->function_count = binary->canon_lift_count;

    for (i = 0u; i < binary->canon_lift_count; ++i) {
        const turbowasm_component_canon_lift *lift =
            &binary->canon_lifts[i];
        const turbowasm_component_exec_core_function *core_function;

        if (lift->component_function_index >= exec->function_count ||
            lift->core_function_index >= exec->core_function_count ||
            lift->type_index >= binary->type_graph.count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        core_function =
            &exec->core_functions[lift->core_function_index];
        if (core_function->instance_index >= exec->core_instance_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        status = turbowasm_component_core_call_adapter_init(
            &exec->functions[lift->component_function_index],
            &binary->type_graph,
            lift->type_index,
            &exec->core_instances[core_function->instance_index],
            core_function->function_index,
            NULL);
        if (status != TURBOWASM_OK)
            goto fail;
    }

    for (i = 0u; i < binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &binary->exports[i];
        if (export_desc->kind !=
                TURBOWASM_COMPONENT_EXTERN_FUNCTION ||
            export_desc->item_index >= exec->function_count) {
            status = TURBOWASM_UNSUPPORTED;
            goto fail;
        }
        if (export_desc->has_ascribed_type) {
            const turbowasm_component_canon_lift *lift =
                &binary->canon_lifts[export_desc->item_index];
            if (export_desc->type_index != lift->type_index) {
                status = TURBOWASM_TYPE_MISMATCH;
                goto fail;
            }
        }
    }

    exec->initialized = true;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;

fail:
    destroy_partial(exec);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

void turbowasm_component_exec_destroy(
    turbowasm_component_exec *exec) {
    if (exec == NULL)
        return;
    destroy_partial(exec);
}

turbowasm_status turbowasm_component_exec_invoke_export(
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    uint32_t i;

    if (exec == NULL || !exec->initialized ||
        exec->binary == NULL ||
        (name_size != 0u && name == NULL) ||
        trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *export_desc =
            &exec->binary->exports[i];

        if (export_desc->kind ==
                TURBOWASM_COMPONENT_EXTERN_FUNCTION &&
            component_name_equal(
                export_desc->name, name, name_size)) {
            if (export_desc->item_index >= exec->function_count)
                return TURBOWASM_MALFORMED_MODULE;
            return turbowasm_component_core_call_invoke(
                &exec->functions[export_desc->item_index],
                arguments,
                argument_count,
                out_result,
                trap);
        }
    }

    return TURBOWASM_INVALID_ARGUMENT;
}
