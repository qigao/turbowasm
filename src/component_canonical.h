#ifndef TURBOWASM_COMPONENT_CANONICAL_H
#define TURBOWASM_COMPONENT_CANONICAL_H

#include "component_type_graph.h"

#include <turbowasm/status.h>

#include <stdint.h>

enum {
    TURBOWASM_COMPONENT_MAX_FLAT_PARAMS = 16u,
    TURBOWASM_COMPONENT_MAX_FLAT_RESULTS = 1u,
    TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS = 17u
};

typedef enum turbowasm_component_pointer_type {
    TURBOWASM_COMPONENT_POINTER_I32 = 0,
    TURBOWASM_COMPONENT_POINTER_I64
} turbowasm_component_pointer_type;

typedef enum turbowasm_component_flat_type {
    TURBOWASM_COMPONENT_FLAT_I32 = 0,
    TURBOWASM_COMPONENT_FLAT_I64,
    TURBOWASM_COMPONENT_FLAT_F32,
    TURBOWASM_COMPONENT_FLAT_F64
} turbowasm_component_flat_type;

typedef enum turbowasm_component_canonical_context {
    TURBOWASM_COMPONENT_CANONICAL_LIFT = 0,
    TURBOWASM_COMPONENT_CANONICAL_LOWER
} turbowasm_component_canonical_context;

typedef struct turbowasm_component_layout {
    uint64_t alignment;
    uint64_t size;
} turbowasm_component_layout;

typedef struct turbowasm_component_flat_type_list {
    turbowasm_component_flat_type types[2];
    uint32_t count;
} turbowasm_component_flat_type_list;

typedef struct turbowasm_component_flat_signature {
    turbowasm_component_flat_type
        params[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS];
    uint32_t param_count;

    turbowasm_component_flat_type
        results[TURBOWASM_COMPONENT_MAX_FLAT_RESULTS];
    uint32_t result_count;

    bool params_indirect;
    bool results_indirect;
} turbowasm_component_flat_signature;

turbowasm_status turbowasm_component_canonical_layout(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_layout *out);

turbowasm_status turbowasm_component_canonical_flatten_type(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_flat_type_list *out);

turbowasm_status turbowasm_component_canonical_flatten_function(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_canonical_context context,
    turbowasm_component_flat_signature *out);

#endif /* TURBOWASM_COMPONENT_CANONICAL_H */
