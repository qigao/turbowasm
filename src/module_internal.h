#ifndef TURBOWASM_MODULE_INTERNAL_H
#define TURBOWASM_MODULE_INTERNAL_H

#include <turbowasm/module.h>

#include "validation_context.h"

#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_module_impl {
    const uint8_t *bytes;
    size_t size;
    turbowasm_module_summary summary;
    turbowasm_validation_context validation;
} turbowasm_module_impl;

static inline turbowasm_module_impl *
turbowasm_module_impl_mut(turbowasm_module *module) {
    return module == NULL
        ? NULL
        : (turbowasm_module_impl *)module->impl;
}

static inline const turbowasm_module_impl *
turbowasm_module_impl_get(const turbowasm_module *module) {
    return module == NULL
        ? NULL
        : (const turbowasm_module_impl *)module->impl;
}

#endif /* TURBOWASM_MODULE_INTERNAL_H */
