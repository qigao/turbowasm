#include <turbowasm/module.h>

#include "module_internal.h"
#include "reader.h"
#include "validate.h"

#include <stdlib.h>

enum {
    TURBOWASM_MAGIC = 0x6d736100u,
    TURBOWASM_BINARY_VERSION = 1u
};

turbowasm_status turbowasm_module_load_borrowed(turbowasm_module *module,
                                                const uint8_t *bytes,
                                                size_t size) {
    turbowasm_reader reader;
    uint32_t magic;
    uint32_t version;
    turbowasm_module_impl *impl;
    turbowasm_module_summary summary = {0};
    turbowasm_validation_context validation = {0};
    turbowasm_status status;

    if (module == NULL || bytes == NULL || module->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_reader_init(&reader, bytes, size);
    if (!turbowasm_reader_u32le(&reader, &magic) ||
        !turbowasm_reader_u32le(&reader, &version))
        return TURBOWASM_MALFORMED_MODULE;
    if (magic != TURBOWASM_MAGIC || version != TURBOWASM_BINARY_VERSION)
        return TURBOWASM_MALFORMED_MODULE;

    status = turbowasm_validate_sections(&reader, &summary, &validation);
    if (status != TURBOWASM_OK) {
        turbowasm_validation_context_destroy(&validation);
        return status;
    }

    impl = (turbowasm_module_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) {
        turbowasm_validation_context_destroy(&validation);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->bytes = bytes;
    impl->size = size;
    impl->summary = summary;
    impl->validation = validation;
    module->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_module_destroy(turbowasm_module *module) {
    turbowasm_module_impl *impl;
    if (module == NULL || module->impl == NULL) return;
    impl = (turbowasm_module_impl *)module->impl;
    turbowasm_validation_context_destroy(&impl->validation);
    free(impl);
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
