#ifndef TURBOWASM_COMPONENT_EXEC_H
#define TURBOWASM_COMPONENT_EXEC_H

#include "component_binary.h"
#include "component_core_call.h"
#include "component_resource_binding.h"

#include <turbowasm/instance.h>
#include <turbowasm/link.h>
#include <turbowasm/module.h>
#include <turbowasm/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_component_exec_core_function_kind {
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INVALID = 0,
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_INSTANCE,
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_RESOURCE_BUILTIN,
    TURBOWASM_COMPONENT_EXEC_CORE_FUNCTION_CANON_LOWER
} turbowasm_component_exec_core_function_kind;

typedef struct turbowasm_component_exec_core_function {
    turbowasm_component_exec_core_function_kind kind;
    uint32_t instance_index;
    uint32_t function_index;
    uint32_t resource_builtin_index;
    uint32_t canon_lower_index;
} turbowasm_component_exec_core_function;

typedef struct turbowasm_component_exec_core_memory {
    uint32_t instance_index;
    uint32_t memory_index;
} turbowasm_component_exec_core_memory;

typedef struct turbowasm_component_exec_realloc_context {
    turbowasm_instance *instance;
    uint32_t function_index;
    turbowasm_component_pointer_type pointer_type;
    bool *may_leave;
    turbowasm_host_call *call;
    turbowasm_trap *trap;
} turbowasm_component_exec_realloc_context;

typedef struct turbowasm_component_exec_resource_context {
    struct turbowasm_component_exec *exec;
    uint32_t resource_type;
    turbowasm_host_call *call;
    turbowasm_trap *trap;
} turbowasm_component_exec_resource_context;

typedef struct turbowasm_component_exec_resource_builtin_context {
    struct turbowasm_component_exec *exec;
    turbowasm_component_resource_binding *binding;
    turbowasm_component_resource_builtin_kind kind;
    uint32_t resource_type;
    bool external;
} turbowasm_component_exec_resource_builtin_context;

/*
 * Internal typed Component-import execution boundary.
 *
 * The generic Component layer owns import/index-space and canonical-ABI
 * semantics. Capability layers such as WASI 0.2 supply callbacks here without
 * teaching Core Runtime or the Component parser any capability-specific name.
 */
typedef bool (*turbowasm_component_import_can_bind_fn)(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type);

typedef turbowasm_status (*turbowasm_component_import_invoke_fn)(
    void *context,
    turbowasm_host_call *call,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

typedef turbowasm_status
(*turbowasm_component_import_resource_drop_fn)(
    void *context,
    uint64_t resource_identity,
    uint32_t handle);

typedef struct turbowasm_component_exec_imports {
    void *context;
    turbowasm_component_import_can_bind_fn can_bind;
    turbowasm_component_import_invoke_fn invoke;

    /* Optional imported-resource canonical boundary. Values contain provider
     * handles; Core sees only this exec's canonical table handles. resource_drop
     * consumes the provider's logical handle even when its destructor fails. */
    turbowasm_component_resource_lower_fn resource_lower;
    turbowasm_component_resource_lift_fn resource_lift;
    turbowasm_component_import_resource_drop_fn resource_drop;
} turbowasm_component_exec_imports;

typedef struct turbowasm_component_exec_canon_lower_context {
    struct turbowasm_component_exec *exec;
    turbowasm_component_name instance_name;
    turbowasm_component_name function_name;
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id function_type;

    turbowasm_component_flat_signature flat_signature;
    turbowasm_host_function_type host_type;
    turbowasm_value_kind
        params[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS];
    turbowasm_value_kind
        results[TURBOWASM_COMPONENT_MAX_FLAT_RESULTS];

    bool uses_memory;
    turbowasm_component_canonical_memory memory;
    turbowasm_component_exec_realloc_context realloc_context;
} turbowasm_component_exec_canon_lower_context;

typedef struct turbowasm_component_exec {
    const turbowasm_component_binary *binary;

    turbowasm_module *core_modules;
    uint32_t core_module_count;

    turbowasm_instance *core_instances;
    uint32_t core_instance_count;

    turbowasm_component_exec_core_function *core_functions;
    uint32_t core_function_count;

    turbowasm_component_exec_core_memory *core_memories;
    uint32_t core_memory_count;

    turbowasm_component_exec_realloc_context *realloc_contexts;

    turbowasm_component_resource_table resource_table;
    turbowasm_component_resource_binding *resource_bindings;
    turbowasm_component_exec_resource_context *resource_contexts;
    uint32_t resource_binding_count;

    turbowasm_component_exec_resource_builtin_context
        *resource_builtin_contexts;

    /*
     * imports is the synthetic router used by existing execution paths.
     * import_sets is a shallow owned copy of capability descriptors; each
     * descriptor context remains caller-owned and borrowed.
     */
    turbowasm_component_exec_imports imports;
    turbowasm_component_exec_imports *import_sets;
    uint32_t import_set_count;

    turbowasm_component_exec_canon_lower_context
        *canon_lower_contexts;

    turbowasm_component_core_call_adapter *functions;
    uint32_t adapter_count;

    uint32_t *function_adapter_indices;
    uint32_t function_count;

    bool may_leave;
    bool initialized;
} turbowasm_component_exec;

/*
 * Instantiate the C5c1 executable subset:
 * - no Component imports;
 * - Core instance definitions are zero-argument module instantiations;
 * - Core function definitions come from core-export aliases;
 * - Component functions come from synchronous canon lift with no options.
 *
 * The executable borrows the decoded Component binary and its source bytes.
 */
turbowasm_status turbowasm_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary);

turbowasm_status turbowasm_component_exec_init_with_imports(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_component_exec_imports *imports);

/*
 * Internal composition boundary for disjoint typed capability providers.
 * The descriptors are shallow-copied by the exec; descriptor contexts remain
 * borrowed and must outlive the exec.
 */
turbowasm_status turbowasm_component_exec_init_with_import_sets(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    const turbowasm_component_exec_imports *import_sets,
    size_t import_set_count);

/* Drop an abstract resource already removed from its canonical table. */
turbowasm_status turbowasm_component_exec_resource_release(
    turbowasm_component_exec *exec, uint64_t identity, turbowasm_value rep);

void turbowasm_component_exec_destroy(
    turbowasm_component_exec *exec);

turbowasm_status turbowasm_component_exec_invoke_export(
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

typedef struct turbowasm_component_exec_call {
    turbowasm_component_core_execution core;
    bool initialized;
} turbowasm_component_exec_call;

turbowasm_status turbowasm_component_exec_call_create(
    turbowasm_component_exec_call *call,
    const turbowasm_component_exec *exec,
    const uint8_t *name,
    uint32_t name_size,
    const turbowasm_component_value *arguments,
    size_t argument_count);

void turbowasm_component_exec_call_destroy(
    turbowasm_component_exec_call *call);

turbowasm_status turbowasm_component_exec_call_resume(
    turbowasm_component_exec_call *call,
    const turbowasm_execution_options *options);

turbowasm_execution_state turbowasm_component_exec_call_state_get(
    const turbowasm_component_exec_call *call);

turbowasm_yield_reason turbowasm_component_exec_call_yield_reason_get(
    const turbowasm_component_exec_call *call);

bool turbowasm_component_exec_call_pending_host_wait(
    const turbowasm_component_exec_call *call,
    turbowasm_host_wait *out_wait);

turbowasm_status turbowasm_component_exec_call_complete_host_wait(
    turbowasm_component_exec_call *call,
    turbowasm_host_wait wait,
    int status);

turbowasm_status turbowasm_component_exec_call_terminal_status(
    const turbowasm_component_exec_call *call);

turbowasm_trap turbowasm_component_exec_call_trap(
    const turbowasm_component_exec_call *call);

size_t turbowasm_component_exec_call_result_count(
    const turbowasm_component_exec_call *call);

turbowasm_status turbowasm_component_exec_call_take_result(
    turbowasm_component_exec_call *call,
    turbowasm_component_value *out_result);

#endif /* TURBOWASM_COMPONENT_EXEC_H */
