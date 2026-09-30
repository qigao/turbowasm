#ifndef TURBOWASM_COMPONENT_API_INTERNAL_H
#define TURBOWASM_COMPONENT_API_INTERNAL_H

#include <turbowasm/component.h>

#include "component_binary.h"
#include "component_exec.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct turbowasm_component_public_impl {
    turbowasm_component_binary binary;
    uint32_t ref_count;
} turbowasm_component_public_impl;

typedef void (*turbowasm_component_instance_owner_release_fn)(
    void *context);

typedef struct turbowasm_component_instance_public_impl {
    turbowasm_component_public_impl *component;
    turbowasm_component_exec exec;

    /*
     * Optional capability owner retained by a specialized public instance
     * constructor (for example WASI 0.2). The owner remains live through exec
     * destruction and is released immediately afterwards.
     */
    void *owner_context;
    turbowasm_component_instance_owner_release_fn owner_release;
} turbowasm_component_instance_public_impl;

turbowasm_component_public_impl *
turbowasm_component_public_impl_get(
    const turbowasm_component *component);

turbowasm_component_instance_public_impl *
turbowasm_component_instance_public_impl_get(
    const turbowasm_component_instance *instance);

bool turbowasm_component_public_impl_retain(
    turbowasm_component_public_impl *impl);

void turbowasm_component_public_impl_release(
    turbowasm_component_public_impl *impl);

#endif /* TURBOWASM_COMPONENT_API_INTERNAL_H */
