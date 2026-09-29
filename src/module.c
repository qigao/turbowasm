#include <turbowasm/module.h>
#include <turbowasm/value.h>

#include "module_internal.h"
#include "reader.h"
#include "validate.h"
#include "runtime_alloc.h"

#include <stdlib.h>

enum {
    TURBOWASM_MAGIC = 0x6d736100u,
    TURBOWASM_BINARY_VERSION = 1u
};

turbowasm_status turbowasm_module_load_borrowed(
    turbowasm_module *module,
    const uint8_t *bytes,
    size_t size) {
    return turbowasm_module_load_borrowed_with_config(
        module, bytes, size, NULL);
}

turbowasm_status turbowasm_module_load_borrowed_with_config(
    turbowasm_module *module,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config) {
    turbowasm_reader reader;
    uint32_t magic;
    uint32_t version;
    turbowasm_module_impl *impl;
    turbowasm_module_summary summary = {0};
    turbowasm_validation_context validation = {0};
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (module == NULL || bytes == NULL || module->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;
    if (normalized.limits.max_module_bytes != 0u &&
        size > normalized.limits.max_module_bytes)
        return TURBOWASM_OUT_OF_MEMORY;

    turbowasm_reader_init(&reader, bytes, size);
    if (!turbowasm_reader_u32le(&reader, &magic) ||
        !turbowasm_reader_u32le(&reader, &version))
        return TURBOWASM_MALFORMED_MODULE;
    if (magic != TURBOWASM_MAGIC || version != TURBOWASM_BINARY_VERSION)
        return TURBOWASM_MALFORMED_MODULE;

    scope = turbowasm_runtime_scope_enter(&normalized);
    status = turbowasm_validate_sections(&reader, &summary, &validation);
    if (status != TURBOWASM_OK) {
        turbowasm_validation_context_destroy(&validation);
        turbowasm_runtime_scope_leave(scope);
        return status;
    }

    impl = (turbowasm_module_impl *)turbowasm_rt_calloc(
        1u, sizeof(*impl));
    if (impl == NULL) {
        turbowasm_validation_context_destroy(&validation);
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->bytes = bytes;
    impl->size = size;
    impl->summary = summary;
    impl->config = normalized;
    impl->validation = validation;
    module->impl = impl;
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;
}

void turbowasm_module_destroy(turbowasm_module *module) {
    turbowasm_module_impl *impl;
    if (module == NULL || module->impl == NULL) return;
    impl = (turbowasm_module_impl *)module->impl;
    turbowasm_validation_context_destroy(&impl->validation);
    turbowasm_rt_free(impl);
    module->impl = NULL;
}

const uint8_t *turbowasm_module_bytes(const turbowasm_module *module) {
    const turbowasm_module_impl *impl;
    if (module == NULL || module->impl == NULL) return NULL;
    impl = (const turbowasm_module_impl *)module->impl;
    return impl->bytes;
}

size_t turbowasm_module_size(const turbowasm_module *module) {
    const turbowasm_module_impl *impl;
    if (module == NULL || module->impl == NULL) return 0u;
    impl = (const turbowasm_module_impl *)module->impl;
    return impl->size;
}

bool turbowasm_module_summary_get(const turbowasm_module *module,
                                  turbowasm_module_summary *out) {
    const turbowasm_module_impl *impl;
    if (module == NULL || module->impl == NULL || out == NULL)
        return false;
    impl = (const turbowasm_module_impl *)module->impl;
    *out = impl->summary;
    return true;
}


size_t turbowasm_module_import_count(
    const turbowasm_module *module) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    return impl == NULL ? 0u : (size_t)impl->validation.import_count;
}

const turbowasm_import_desc *turbowasm_module_import_at(
    const turbowasm_module *module,
    size_t index) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);

    if (impl == NULL ||
        index >= (size_t)impl->validation.import_count)
        return NULL;
    return &impl->validation.imports[index];
}

size_t turbowasm_module_export_count(
    const turbowasm_module *module) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    return impl == NULL ? 0u : (size_t)impl->validation.export_count;
}

const turbowasm_export_desc *turbowasm_module_export_at(
    const turbowasm_module *module,
    size_t index) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);

    if (impl == NULL ||
        index >= (size_t)impl->validation.export_count)
        return NULL;
    return &impl->validation.exports[index];
}

size_t turbowasm_module_memory_count(
    const turbowasm_module *module) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    return impl == NULL
        ? 0u
        : (size_t)impl->validation.memory_count;
}

bool turbowasm_module_memory_at(
    const turbowasm_module *module,
    size_t index,
    turbowasm_memory_desc *out) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_memory *memory;

    if (impl == NULL || out == NULL ||
        index >= (size_t)impl->validation.memory_count)
        return false;

    memory = &impl->validation.memories[index];
    out->minimum = memory->limits.minimum;
    out->maximum = memory->limits.maximum;
    out->page_size = memory->page_size;
    out->has_maximum = memory->limits.has_maximum;
    out->shared = memory->shared;
    out->imported = memory->imported;
    return true;
}


static const turbowasm_validation_func_type *
turbowasm_module_reflected_function_type(
    const turbowasm_module *module,
    uint32_t function_index) {
    const turbowasm_module_impl *impl = turbowasm_module_impl_get(module);

    if (impl == NULL)
        return NULL;
    return turbowasm_validation_context_function_type(
        &impl->validation, function_index);
}

bool turbowasm_module_function_signature_get(
    const turbowasm_module *module,
    uint32_t function_index,
    turbowasm_function_signature *out) {
    const turbowasm_validation_func_type *type;

    if (out == NULL)
        return false;

    type = turbowasm_module_reflected_function_type(module, function_index);
    if (type == NULL || !type->defined)
        return false;

    out->param_count = type->param_count;
    out->result_count = type->result_count;
    return true;
}

const cmeta_type_desc *turbowasm_module_function_param_type(
    const turbowasm_module *module,
    uint32_t function_index,
    uint32_t param_index) {
    const turbowasm_validation_func_type *type =
        turbowasm_module_reflected_function_type(module, function_index);

    if (type == NULL || !type->defined || param_index >= type->param_count)
        return NULL;
    return turbowasm_value_type_descriptor(
        (turbowasm_value_kind)type->params[param_index]);
}

const cmeta_type_desc *turbowasm_module_function_result_type(
    const turbowasm_module *module,
    uint32_t function_index,
    uint32_t result_index) {
    const turbowasm_validation_func_type *type =
        turbowasm_module_reflected_function_type(module, function_index);

    if (type == NULL || !type->defined || result_index >= type->result_count)
        return NULL;
    return turbowasm_value_type_descriptor(
        (turbowasm_value_kind)type->results[result_index]);
}
