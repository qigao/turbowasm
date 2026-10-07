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
    TURBOWASM_COMPONENT_TYPE_RECORD,
    TURBOWASM_COMPONENT_TYPE_TUPLE,
    TURBOWASM_COMPONENT_TYPE_VARIANT,
    TURBOWASM_COMPONENT_TYPE_OPTION,
    TURBOWASM_COMPONENT_TYPE_RESULT,
    TURBOWASM_COMPONENT_TYPE_ENUM,
    TURBOWASM_COMPONENT_TYPE_FLAGS,
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

typedef struct turbowasm_component_record_field {
    const uint8_t *name;
    uint32_t name_size;
    turbowasm_component_type_ref type;
} turbowasm_component_record_field;

typedef struct turbowasm_component_label {
    const uint8_t *name;
    uint32_t name_size;
} turbowasm_component_label;

typedef struct turbowasm_component_variant_case {
    const uint8_t *name;
    uint32_t name_size;
    bool has_payload;
    turbowasm_component_type_ref payload;
} turbowasm_component_variant_case;

typedef struct turbowasm_component_type {
    turbowasm_component_type_kind kind;
    union {
        struct {
            turbowasm_component_type_ref element_type;
        } list;
        struct {
            turbowasm_component_record_field *fields;
            uint32_t count;
        } record;
        struct {
            turbowasm_component_type_ref *elements;
            uint32_t count;
        } tuple;
        struct {
            turbowasm_component_variant_case *cases;
            uint32_t count;
        } variant;
        struct {
            turbowasm_component_type_ref payload;
        } option;
        struct {
            bool has_ok;
            turbowasm_component_type_ref ok;
            bool has_error;
            turbowasm_component_type_ref error;
        } result;
        struct {
            turbowasm_component_label *labels;
            uint32_t count;
        } enumeration;
        struct {
            turbowasm_component_label *labels;
            uint32_t count;
        } flags;
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
            bool identity_alias;
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

typedef enum turbowasm_component_instance_type_export_kind {
    TURBOWASM_COMPONENT_INSTANCE_EXPORT_FUNCTION = 1,
    TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE = 3
} turbowasm_component_instance_type_export_kind;

typedef struct turbowasm_component_instance_type_export {
    const uint8_t *name;
    uint32_t name_size;
    turbowasm_component_instance_type_export_kind kind;
    turbowasm_component_type_id type_index;
} turbowasm_component_instance_type_export;

struct turbowasm_component_instance_type {
    turbowasm_component_type_graph type_graph;
    turbowasm_component_instance_type_export *exports;
    uint32_t export_count;
};

/* Resolve nominal aliases within one graph. An imported identity has no local
 * definition and resolves to its alias node; it retains identity_alias=true. */
const turbowasm_component_type *turbowasm_component_resource_definition(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id resource_type);

enum {
    TURBOWASM_COMPONENT_VALUE_MAX_DEPTH = 64,
    TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY = 1u,
    TURBOWASM_COMPONENT_VALUE_RESOURCES = 2u
};

/* Inspect a synchronous value tree without allocation. False rejects non-value
 * nodes, invalid references and excessive depth; output is unchanged on error.
 * Time O(expanded value-type tree), stack O(depth), bounded above. */
bool turbowasm_component_value_type_features(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref ref,
    uint32_t *out_features);

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

bool turbowasm_component_type_graph_define_record(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_record_field *fields,
    uint32_t field_count);

bool turbowasm_component_type_graph_define_tuple(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_type_ref *elements,
    uint32_t element_count);

/* Pure preflight separates malformed cases from allocation failure. */
bool turbowasm_component_variant_cases_valid(
    const turbowasm_component_type_graph *graph,
    const turbowasm_component_variant_case *cases,
    uint32_t case_count);

bool turbowasm_component_type_graph_define_variant(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_variant_case *cases,
    uint32_t case_count);

bool turbowasm_component_type_graph_define_option(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    turbowasm_component_type_ref payload);

bool turbowasm_component_type_graph_define_result(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    bool has_ok,
    turbowasm_component_type_ref ok,
    bool has_error,
    turbowasm_component_type_ref error);

bool turbowasm_component_labels_valid(
    const turbowasm_component_label *labels,
    uint32_t label_count);

bool turbowasm_component_type_graph_define_enum(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_label *labels,
    uint32_t label_count);

bool turbowasm_component_type_graph_define_flags(
    turbowasm_component_type_graph *graph,
    turbowasm_component_type_id id,
    const turbowasm_component_label *labels,
    uint32_t label_count);

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

bool turbowasm_component_type_graph_define_resource_alias(
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
