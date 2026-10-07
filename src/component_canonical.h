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
    /*
     * Internal flatten scratch capacity. The synchronous ABI switches to
     * indirect parameters above 16 flat carriers; one extra slot preserves
     * that over-limit distinction for retained composites.
     */
    turbowasm_component_flat_type
        types[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS];
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
    TURBOWASM_COMPONENT_STRING_UTF8 = 0,
    TURBOWASM_COMPONENT_STRING_UTF16,
    TURBOWASM_COMPONENT_STRING_LATIN1_UTF16
} turbowasm_component_string_encoding;

typedef struct turbowasm_component_value turbowasm_component_value;

typedef enum turbowasm_component_string_origin {
    TURBOWASM_COMPONENT_STRING_ORIGIN_UTF8 = 0,
    TURBOWASM_COMPONENT_STRING_ORIGIN_UTF16,
    TURBOWASM_COMPONENT_STRING_ORIGIN_LATIN1,
    TURBOWASM_COMPONENT_STRING_ORIGIN_COMPACT_UTF16
} turbowasm_component_string_origin;

typedef struct turbowasm_component_owned_string {
    uint8_t *data;
    size_t size;
    /* Bytes are always UTF-8; origin preserves the canonical realloc protocol. */
    turbowasm_component_string_origin origin;
} turbowasm_component_owned_string;

typedef struct turbowasm_component_value_list {
    turbowasm_component_value *items;
    uint64_t count;
} turbowasm_component_value_list;

typedef struct turbowasm_component_value_variant {
    uint32_t case_index;
    turbowasm_component_value *payload;
} turbowasm_component_value_variant;

typedef turbowasm_status (*turbowasm_component_value_release_fn)(void *context);

/* One record per private endpoint value obligation. The adapter owns allocation
 * and cleanup. NULL endpoint means ownership was committed to a guest; the old
 * carrier can still be destroyed without touching a subsequent endpoint owner. */
typedef struct turbowasm_component_endpoint_value_owner {
    void *endpoint;
} turbowasm_component_endpoint_value_owner;

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
        turbowasm_component_owned_string string;
        turbowasm_component_value_list list;
        turbowasm_component_value_list record;
        turbowasm_component_value_list tuple;
        turbowasm_component_value_variant variant;
        turbowasm_component_value_variant option;
        turbowasm_component_value_variant result;
        uint32_t enum_index;
        uint32_t flags;
        /*
         * Abstract Component resource value. Canonical Core handles are
         * created/consumed only at a resource-table boundary.
         */
        turbowasm_value resource_rep;
        /* Private endpoint owner: direct operations are frozen while this
         * unique value owns it. Graph/type references remain borrowed. The
         * endpoint adapter supplies release; canonical codecs stay separate. */
        struct {
            turbowasm_component_endpoint_value_owner *owner;
            const turbowasm_component_type_graph *graph;
            turbowasm_component_type_ref type;
        } endpoint;
    } as;
    /* Optional lifted owner or import loan. Moving a value moves this cleanup
     * obligation; borrowed public argument copies have no release hook. */
    turbowasm_component_value_release_fn release;
    void *release_context;
    uint64_t resource_identity;
};

typedef turbowasm_status (*turbowasm_component_realloc_fn)(
    void *context,
    uint64_t old_pointer,
    uint64_t old_size,
    uint64_t alignment,
    uint64_t new_size,
    uint64_t *out_pointer);

typedef turbowasm_status (*turbowasm_component_resource_lower_fn)(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle);

typedef turbowasm_status (*turbowasm_component_resource_lift_fn)(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out);

/* Equal carrier signatures, separate ownership contexts and callbacks. */
typedef turbowasm_component_resource_lower_fn turbowasm_component_endpoint_lower_fn;
typedef turbowasm_component_resource_lift_fn turbowasm_component_endpoint_lift_fn;

typedef struct turbowasm_component_canonical_memory {
    turbowasm_instance *instance;
    uint32_t memory_index;
    turbowasm_component_pointer_type pointer_type;
    turbowasm_component_string_encoding string_encoding;
    turbowasm_component_realloc_fn guest_realloc;
    void *realloc_context;

    /*
     * Optional C5 resource boundary. C3-only callers leave these NULL and
     * resource-valued memory representations remain unsupported.
     */
    turbowasm_component_resource_lower_fn resource_lower;
    turbowasm_component_resource_lift_fn resource_lift;
    void *resource_context;
    turbowasm_component_endpoint_lower_fn endpoint_lower;
    turbowasm_component_endpoint_lift_fn endpoint_lift;
    void *endpoint_context;
} turbowasm_component_canonical_memory;

turbowasm_status turbowasm_component_canonical_layout(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_layout *out);

/* Validate a nonempty typed guest region, returning its element stride only on
 * success. Checks memory width, alignment, count multiplication and full bounds.
 * No raw view survives this call; codecs reacquire memory for each access. */
turbowasm_status turbowasm_component_canonical_validate_range(
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address, uint32_t count, uint64_t *out_stride);

turbowasm_status turbowasm_component_canonical_parameter_layout(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_component_layout *out);

/* Validate the whole aligned tuple before transferring fields. Lift requires
 * zeroed output cells; the caller destroys all cells even on partial failure. */
turbowasm_status turbowasm_component_canonical_lift_parameters(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address, turbowasm_component_value *out);
turbowasm_status turbowasm_component_canonical_lower_parameters(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_canonical_memory *memory,
    uint64_t address, const turbowasm_component_value *values);

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
 * Canonical flat value conversion. Dynamic string/list values use guest memory.
 * Resource and private endpoint handles require their explicit callbacks; their
 * owning composition layer commits/rolls back any lower reservations.
 */
turbowasm_status turbowasm_component_canonical_lower_flat_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_component_value *value,
    turbowasm_value *out,
    uint32_t out_capacity,
    uint32_t *out_count);

turbowasm_status turbowasm_component_canonical_lift_flat_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *values,
    uint32_t value_count,
    turbowasm_component_value *out);

/*
 * Lift/lower scalar/composite values in canonical memory. Strings
 * follow the selected UTF-8, UTF-16 or compact encoding. Resource handles use
 * explicit callbacks attached by the owning composition layer, as do private
 * endpoint handles. Failed lifts destroy already lifted fields; failed lowers
 * require the caller to roll back its staged handles before destroying inputs.
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

/* Pure admission validation: no guest calls, writes, allocations or transfers.
 * A nonzero resource_identity additionally checks a host-provided nominal ID. */
turbowasm_status turbowasm_component_canonical_validate_value(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value);

turbowasm_status turbowasm_component_value_destroy(
    turbowasm_component_value *value);

#endif /* TURBOWASM_COMPONENT_CANONICAL_H */
