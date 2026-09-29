#ifndef TURBOWASM_COMPONENT_CANONICAL_H
#define TURBOWASM_COMPONENT_CANONICAL_H

#include "component_type_graph.h"

#include <turbowasm/instance.h>
#include <turbowasm/status.h>

#include <stddef.h>
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

typedef enum turbowasm_component_string_encoding {
    TURBOWASM_COMPONENT_STRING_UTF8 = 0
} turbowasm_component_string_encoding;

typedef struct turbowasm_component_value turbowasm_component_value;

typedef struct turbowasm_component_owned_bytes {
    uint8_t *data;
    size_t size;
} turbowasm_component_owned_bytes;

typedef struct turbowasm_component_value_list {
    turbowasm_component_value *items;
    uint64_t count;
} turbowasm_component_value_list;

struct turbowasm_component_value {
    turbowasm_component_type_kind kind;
    union {
        bool boolean;
        int8_t s8;
        uint8_t u8;
        int16_t s16;
        uint16_t u16;
        int32_t s32;
        uint32_t u32;
        int64_t s64;
        uint64_t u64;
        float f32;
        double f64;
        uint32_t character;
        turbowasm_component_owned_bytes string;
        turbowasm_component_value_list list;
    } as;
};

typedef turbowasm_status (*turbowasm_component_realloc_fn)(
    void *context,
    uint64_t old_pointer,
    uint64_t old_size,
    uint64_t alignment,
    uint64_t new_size,
    uint64_t *out_pointer);

typedef struct turbowasm_component_canonical_memory {
    turbowasm_instance *instance;
    uint32_t memory_index;
    turbowasm_component_pointer_type pointer_type;
    turbowasm_component_string_encoding string_encoding;
    turbowasm_component_realloc_fn guest_realloc;
    void *realloc_context;
} turbowasm_component_canonical_memory;

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

/*
 * Lift/lower the canonical in-memory representation of the retained
 * scalar/string/list subset. String lowering currently implements the pinned
 * UTF-8 canonical option. Resource handles remain C4.
 */
turbowasm_status turbowasm_component_canonical_lift_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    turbowasm_component_value *out);

turbowasm_status turbowasm_component_canonical_lower_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address,
    const turbowasm_component_value *value);

void turbowasm_component_value_destroy(
    turbowasm_component_value *value);

#endif /* TURBOWASM_COMPONENT_CANONICAL_H */
