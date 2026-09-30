#ifndef TURBOWASM_COMPONENT_TYPE_GRAPH_H
#define TURBOWASM_COMPONENT_TYPE_GRAPH_H

#include <cmeta/cmeta.h>

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t turbowasm_component_type_id;

enum {
    TURBOWASM_COMPONENT_TYPE_INVALID = UINT32_MAX
};

typedef enum turbowasm_component_type_kind {
    TURBOWASM_COMPONENT_TYPE_UNDEFINED = 0,
    TURBOWASM_COMPONENT_TYPE_BOOL,
    TURBOWASM_COMPONENT_TYPE_S8,
    TURBOWASM_COMPONENT_TYPE_U8,
    TURBOWASM_COMPONENT_TYPE_S16,
    TURBOWASM_COMPONENT_TYPE_U16,
    TURBOWASM_COMPONENT_TYPE_S32,
    TURBOWASM_COMPONENT_TYPE_U32,
    TURBOWASM_COMPONENT_TYPE_S64,
    TURBOWASM_COMPONENT_TYPE_U64,
    TURBOWASM_COMPONENT_TYPE_F32,
    TURBOWASM_COMPONENT_TYPE_F64,
    TURBOWASM_COMPONENT_TYPE_CHAR,
    TURBOWASM_COMPONENT_TYPE_STRING,
    TURBOWASM_COMPONENT_TYPE_LIST,
    TURBOWASM_COMPONENT_TYPE_FUNCTION,
    TURBOWASM_COMPONENT_TYPE_RESOURCE,
    TURBOWASM_COMPONENT_TYPE_OWN,
    TURBOWASM_COMPONENT_TYPE_BORROW,
    TURBOWASM_COMPONENT_TYPE_INSTANCE
} turbowasm_component_type_kind;

typedef enum turbowasm_component_type_ref_kind {
    TURBOWASM_COMPONENT_TYPE_REF_INDEXED = 0,
    TURBOWASM_COMPONENT_TYPE_REF_INLINE
} turbowasm_component_type_ref_kind;

typedef struct turbowasm_component_type_ref {
    turbowasm_component_type_ref_kind kind;
    union {
        turbowasm_component_type_id indexed;
        turbowasm_component_type_kind inline_type;
    } as;
} turbowasm_component_type_ref;

typedef struct turbowasm_component_instance_type
    turbowasm_component_instance_type;

typedef struct turbowasm_component_type {
    turbowasm_component_type_kind kind;
    union {
        struct {
            turbowasm_component_type_ref element_type;
        } list;
        struct {
            turbowasm_component_type_ref *params;
            uint32_t param_count;
            bool has_result;
            turbowasm_component_type_ref result;
        } function;
        struct {
            uint64_t identity;
            uint8_t rep_type;
            bool has_destructor;
            uint32_t destructor_index;
        } resource;
        struct {
            turbowasm_component_type_id resource_type;
        } handle;
        turbowasm_component_instance_type *instance;
    } as;
} turbowasm_component_type;

typedef struct turbowasm_component_type_graph {
    turbowasm_component_type *types;
    uint32_t count;
} turbowasm_component_type_graph;

typedef struct turbowasm_component_instance_type_export {
    const uint8_t *name;
    uint32_t name_size;
    turbowasm_component_type_id function_type;
} turbowasm_component_instance_type_export;

struct turbowasm_component_instance_type {
    turbowasm_component_type_graph type_graph;
    turbowasm_component_instance_type_export *exports;
    uint32_t export_count;
};

/*
 * Allocate an exact number of stable type-id slots. The graph owns all nodes
 * but never owns Core Wasm validation metadata. Allocation uses the current
 * TurboWasm runtime allocation scope.
 */
bool turbowasm_component_type_graph_allocate(
    turbowasm_component_type_graph *graph,
    uint32_t count);

void turbowasm_component_type_graph_destroy(
    turbowasm_component_type_graph *graph);

bool turbowasm_component_type_graph_define_scalar(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_kind kind);

bool turbowasm_component_type_graph_define_string(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id);

bool turbowasm_component_type_graph_define_list(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_id element_type);

bool turbowasm_component_type_graph_define_list_ref(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_ref element_type);

bool turbowasm_component_type_graph_define_function(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_type_ref *params,
    uint32_t param_count,
    bool has_result,
    turbowasm_component_type_ref result);

bool turbowasm_component_type_graph_define_resource(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity);

bool turbowasm_component_type_graph_define_resource_full(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    uint64_t nominal_identity,
    uint8_t rep_type,
    bool has_destructor,
    uint32_t destructor_index);

turbowasm_component_type_ref turbowasm_component_type_ref_indexed(
    turbowasm_component_type_id id);

turbowasm_component_type_ref turbowasm_component_type_ref_inline(
    turbowasm_component_type_kind kind);

bool turbowasm_component_type_ref_validate(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref);

bool turbowasm_component_type_graph_define_handle(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_kind kind,
    turbowasm_component_type_id resource_type);

/*
 * Transfer ownership of a retained instance type into one graph slot.
 * The nested graph is local to that instance type's declarator scope.
 */
bool turbowasm_component_type_graph_define_instance(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_instance_type *instance_type);

const turbowasm_component_type *
turbowasm_component_type_graph_get(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id);

/*
 * Validate all indexed references and resource nominal identities. This is
 * intentionally independent of Core validation_context.
 */
bool turbowasm_component_type_graph_validate(
    const turbowasm_component_type_graph *graph);

/*
 * Read-only CMeta projection for Component scalar carriers. String/list/
 * resource/own/borrow deliberately return NULL: their Component semantics are
 * not equivalent to a single C ABI type.
 */
const cmeta_type_desc *turbowasm_component_scalar_cmeta_type(
    turbowasm_component_type_kind kind);

#endif /* TURBOWASM_COMPONENT_TYPE_GRAPH_H */
