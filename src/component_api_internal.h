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
    uint32_t ref_count;
    turbowasm_component_host_resource *resources;
    uint32_t resource_count;

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

/* Private constructor used while the public async boundary is being completed.
 * The ordinary loader and public instance constructor retain their async gate. */
turbowasm_status turbowasm_component_instance_create_async_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits);

/* Owner-thread budget, shared by deferred host admissions. The budget must
 * outlive every reservation; limits are finite and immutable while in use. */
typedef struct turbowasm_component_host_budget {
    size_t limit;
    size_t used;
} turbowasm_component_host_budget;

typedef struct turbowasm_component_host_arguments {
    void *impl;
} turbowasm_component_host_arguments;

/* Private staging for public async owners. Copies ordinary input storage and
 * retains the instance. Prepare reserves resources; commit consumes own leaves.
 * Graph/type are needed only during prepare. Source cells must survive until
 * commit or destruction. Failed prepare leaves output, budget and inputs intact.
 * These values borrow resource obligations from this owner: do not destroy or
 * move individual cells. Async mode adopts resource leaves into the exec's
 * existing codec, with storage charged before allocation; requires an async
 * domain. Lowering is permitted after commit and published() follows the codec
 * commit. Ordinary mode keeps the synchronous canonical publication contract. */
turbowasm_status turbowasm_component_host_arguments_prepare(
    turbowasm_component_host_arguments *owner,
    turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, uint32_t function_type,
    const turbowasm_component_host_value *arguments, size_t count, bool move, bool async_resources,
    turbowasm_component_host_budget *budget);
const turbowasm_component_value *turbowasm_component_host_arguments_values(
    const turbowasm_component_host_arguments *owner, size_t *count);
turbowasm_status turbowasm_component_host_arguments_commit(
    turbowasm_component_host_arguments *owner);
/* Call only after canonical ownership publication succeeds. Leaves loans live. */
turbowasm_status turbowasm_component_host_arguments_published(
    turbowasm_component_host_arguments *owner);
/* Requires no in-flight guest use. Lower reservations reject destruction without
 * mutation. Otherwise frees the owner even if a destructor reports failure. */
turbowasm_status turbowasm_component_host_arguments_destroy(
    turbowasm_component_host_arguments *owner);

typedef struct turbowasm_component_host_result { void *impl; } turbowasm_component_host_result;
/* Private staging for terminal async delivery. The graph is an instantiated
 * result type. Fresh canonical resources are retained with their release
 * authority; argument proxies and borrowed results cannot escape. Failure keeps
 * source/output/budget intact. Success empties source and holds the byte charge
 * and instance until take/destroy. Source allocation must belong to this runtime.
 * Owner-thread only; the shared budget must outlive this owner. */
turbowasm_status turbowasm_component_host_result_prepare(
    turbowasm_component_host_result *owner, turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    turbowasm_component_value *source, turbowasm_component_host_budget *budget);
/* Moves the result into an empty host value and returns the retained byte charge.
 * The ordinary host-value destroy contract then owns its storage/resources. */
turbowasm_status turbowasm_component_host_result_take(
    turbowasm_component_host_result *owner, turbowasm_component_host_value *out);
turbowasm_status turbowasm_component_host_result_destroy(turbowasm_component_host_result *owner);

#endif /* TURBOWASM_COMPONENT_API_INTERNAL_H */
