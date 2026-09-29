#include "component_exec.h"

#include "runtime_alloc.h"

#include <turbowasm/link.h>
#include <turbowasm/value.h>

#include <cmeta/cmeta.h>

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

static const turbowasm_export_desc *find_core_memory_export(
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
            export_desc->kind == TURBOWASM_EXTERN_MEMORY &&
            component_name_equal_core(name, export_desc->name))
            return export_desc;
    }
    return NULL;
}

static turbowasm_value_kind pointer_value_kind(
    turbowasm_component_pointer_type pointer_type) {
    return pointer_type == TURBOWASM_COMPONENT_POINTER_I64
        ? TURBOWASM_VALUE_I64
        : TURBOWASM_VALUE_I32;
}

static bool core_function_has_pointer_signature(
    const turbowasm_instance *instance,
    uint32_t function_index,
    turbowasm_component_pointer_type pointer_type) {
    const turbowasm_module *module;
    turbowasm_function_signature signature;
    const cmeta_type_desc *expected;
    uint32_t i;

    if (instance == NULL)
        return false;
    module = turbowasm_instance_module(instance);
    if (module == NULL ||
        !turbowasm_module_function_signature_get(
            module, function_index, &signature) ||
        signature.param_count != 4u ||
        signature.result_count != 1u)
        return false;

    expected = turbowasm_value_type_descriptor(
        pointer_value_kind(pointer_type));
    if (expected == NULL)
        return false;

    for (i = 0u; i < 4u; ++i) {
        const cmeta_type_desc *actual =
            turbowasm_module_function_param_type(
                module, function_index, i);
        if (actual == NULL || !cmeta_type_equal(actual, expected))
            return false;
    }

    {
        const cmeta_type_desc *actual =
            turbowasm_module_function_result_type(
                module, function_index, 0u);
        return actual != NULL &&
               cmeta_type_equal(actual, expected);
    }
}

static turbowasm_status pointer_argument(
    turbowasm_component_pointer_type pointer_type,
    uint64_t value,
    turbowasm_value *out) {
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I32) {
        if (value > UINT32_MAX)
            return TURBOWASM_TRAPPED;
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)value;
        return TURBOWASM_OK;
    }
    if (pointer_type == TURBOWASM_COMPONENT_POINTER_I64) {
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)value;
        return TURBOWASM_OK;
    }
    return TURBOWASM_INVALID_ARGUMENT;
}

static turbowasm_status component_guest_realloc(
    void *context,
    uint64_t old_pointer,
    uint64_t old_size,
    uint64_t alignment,
    uint64_t new_size,
    uint64_t *out_pointer) {
    turbowasm_component_exec_realloc_context *realloc_context =
        (turbowasm_component_exec_realloc_context *)context;
    turbowasm_value arguments[4] = {{0}};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    if (realloc_context == NULL ||
        realloc_context->instance == NULL ||
        out_pointer == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = pointer_argument(
        realloc_context->pointer_type,
        old_pointer, &arguments[0]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        old_size, &arguments[1]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        alignment, &arguments[2]);
    if (status != TURBOWASM_OK)
        return status;
    status = pointer_argument(
        realloc_context->pointer_type,
        new_size, &arguments[3]);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_instance_invoke(
        realloc_context->instance,
        realloc_context->function_index,
        arguments, 4u,
        &result, 1u,
        &result_count, &trap);
    if (status != TURBOWASM_OK)
        return status;
    if (trap != TURBOWASM_TRAP_NONE)
        return TURBOWASM_TRAPPED;
    if (result_count != 1u ||
        result.kind !=
            pointer_value_kind(realloc_context->pointer_type))
        return TURBOWASM_TYPE_MISMATCH;

    *out_pointer =
        result.kind == TURBOWASM_VALUE_I64
            ? (uint64_t)result.as.i64
            : (uint64_t)(uint32_t)result.as.i32;
    return TURBOWASM_OK;
}

static turbowasm_status instantiate_core_instance(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    uint32_t index) {
    const turbowasm_component_core_instance_def *definition;
    turbowasm_linker linker = {0};
    bool has_linker = false;
    turbowasm_status status;
    uint32_t i;

    if (exec == NULL || binary == NULL ||
        index >= binary->core_instance_count)
        return TURBOWASM_INVALID_ARGUMENT;

    definition = &binary->core_instances[index];
    if (definition->module_index >= exec->core_module_count)
        return TURBOWASM_MALFORMED_MODULE;

    if (definition->argument_count == 0u)
        return turbowasm_instance_create(
            &exec->core_instances[index],
            &exec->core_modules[definition->module_index]);

    status = turbowasm_linker_init_with_config(
        &linker, &binary->config);
    if (status != TURBOWASM_OK)
        return status;
    has_linker = true;

    for (i = 0u; i < definition->argument_count; ++i) {
        const turbowasm_component_core_instantiate_arg *argument =
            &definition->arguments[i];
        turbowasm_name name;

        if (argument->instance_index >= index) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto done;
        }

        name.bytes = argument->name.bytes;
        name.size = argument->name.size;
        status = turbowasm_linker_define_instance(
            &linker,
            name,
            &exec->core_instances[argument->instance_index]);
        if (status != TURBOWASM_OK)
            goto done;
    }

    status = turbowasm_instance_create_linked(
        &exec->core_instances[index],
        &exec->core_modules[definition->module_index],
        &linker);

done:
    if (has_linker)
        turbowasm_linker_destroy(&linker);
    return status;
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
    turbowasm_rt_free(exec->realloc_contexts);
    turbowasm_rt_free(exec->core_memories);
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
        status = instantiate_core_instance(exec, binary, i);
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

    if (binary->core_memory_alias_count != 0u) {
        if ((size_t)binary->core_memory_alias_count >
            SIZE_MAX / sizeof(*exec->core_memories)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->core_memories =
            (turbowasm_component_exec_core_memory *)
                turbowasm_rt_calloc(
                    binary->core_memory_alias_count,
                    sizeof(*exec->core_memories));
        if (exec->core_memories == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }
    exec->core_memory_count = binary->core_memory_alias_count;

    for (i = 0u; i < binary->core_memory_alias_count; ++i) {
        const turbowasm_component_core_memory_alias *alias =
            &binary->core_memory_aliases[i];
        const turbowasm_export_desc *export_desc;
        const turbowasm_module *module;

        if (alias->core_memory_index >= exec->core_memory_count ||
            alias->instance_index >= exec->core_instance_count) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        module = turbowasm_instance_module(
            &exec->core_instances[alias->instance_index]);
        export_desc = find_core_memory_export(module, alias->name);
        if (export_desc == NULL) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        exec->core_memories[alias->core_memory_index].instance_index =
            alias->instance_index;
        exec->core_memories[alias->core_memory_index].memory_index =
            export_desc->item_index;
    }

    if (binary->canon_lift_count != 0u) {
        if ((size_t)binary->canon_lift_count >
                SIZE_MAX / sizeof(*exec->functions) ||
            (size_t)binary->canon_lift_count >
                SIZE_MAX / sizeof(*exec->realloc_contexts)) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
        exec->functions =
            (turbowasm_component_core_call_adapter *)
                turbowasm_rt_calloc(
                    binary->canon_lift_count,
                    sizeof(*exec->functions));
        exec->realloc_contexts =
            (turbowasm_component_exec_realloc_context *)
                turbowasm_rt_calloc(
                    binary->canon_lift_count,
                    sizeof(*exec->realloc_contexts));
        if (exec->functions == NULL ||
            exec->realloc_contexts == NULL) {
            status = TURBOWASM_OUT_OF_MEMORY;
            goto fail;
        }
    }
    exec->function_count = binary->canon_lift_count;

    for (i = 0u; i < binary->canon_lift_count; ++i) {
        const turbowasm_component_canon_lift *lift =
            &binary->canon_lifts[i];
        const turbowasm_component_exec_core_function *core_function;
        turbowasm_component_canonical_memory memory = {0};
        const turbowasm_component_canonical_memory *memory_option = NULL;

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

        if (lift->has_realloc && !lift->has_memory) {
            status = TURBOWASM_MALFORMED_MODULE;
            goto fail;
        }

        if (lift->has_memory) {
            const turbowasm_component_exec_core_memory *core_memory;
            const turbowasm_module *memory_module;
            turbowasm_memory_desc memory_desc;

            if (lift->memory_index >= exec->core_memory_count) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            core_memory = &exec->core_memories[lift->memory_index];
            if (core_memory->instance_index >= exec->core_instance_count) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }

            memory.instance =
                &exec->core_instances[core_memory->instance_index];
            memory.memory_index = core_memory->memory_index;
            memory.string_encoding = lift->string_encoding;

            memory_module = turbowasm_instance_module(memory.instance);
            if (memory_module == NULL ||
                !turbowasm_module_memory_at(
                    memory_module,
                    memory.memory_index,
                    &memory_desc)) {
                status = TURBOWASM_MALFORMED_MODULE;
                goto fail;
            }
            memory.pointer_type = memory_desc.memory64
                ? TURBOWASM_COMPONENT_POINTER_I64
                : TURBOWASM_COMPONENT_POINTER_I32;

            if (lift->has_realloc) {
                const turbowasm_component_exec_core_function *realloc_function;
                turbowasm_component_exec_realloc_context *context;

                if (lift->realloc_function_index >=
                    exec->core_function_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }
                realloc_function =
                    &exec->core_functions[
                        lift->realloc_function_index];
                if (realloc_function->instance_index >=
                    exec->core_instance_count) {
                    status = TURBOWASM_MALFORMED_MODULE;
                    goto fail;
                }

                context =
                    &exec->realloc_contexts[
                        lift->component_function_index];
                context->instance =
                    &exec->core_instances[
                        realloc_function->instance_index];
                context->function_index =
                    realloc_function->function_index;
                context->pointer_type = memory.pointer_type;

                if (!core_function_has_pointer_signature(
                        context->instance,
                        context->function_index,
                        context->pointer_type)) {
                    status = TURBOWASM_TYPE_MISMATCH;
                    goto fail;
                }

                memory.guest_realloc = component_guest_realloc;
                memory.realloc_context = context;
            }

            memory_option = &memory;
        }

        status = turbowasm_component_core_call_adapter_init(
            &exec->functions[lift->component_function_index],
            &binary->type_graph,
            lift->type_index,
            &exec->core_instances[core_function->instance_index],
            core_function->function_index,
            memory_option);
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
