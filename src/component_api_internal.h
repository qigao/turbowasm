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

/* Owner-thread logical host storage; each reservation has one release owner. */
typedef struct turbowasm_component_host_budget {
    size_t limit;
    size_t used;
} turbowasm_component_host_budget;

/* Private staging of the approved public options contract. Counts/bytes are
 * finite, nonzero and copied on create; no caller-owned budget is borrowed. */
typedef struct turbowasm_component_async_options {
    uint32_t tasks, handles, transfers;
    size_t host_bytes;
} turbowasm_component_async_options;

/* Intrusive, non-owning registration in already bounded host owner storage.
 * Shutdown requests cancellation without taking or freeing the host carrier. */
typedef struct turbowasm_component_host_registration {
    struct turbowasm_component_host_registration *next;
    struct turbowasm_component_host_registration **previous;
    void *context;
    bool (*busy)(const void *context);
    turbowasm_status (*cancel)(void *context);
} turbowasm_component_host_registration;

typedef struct turbowasm_component_instance_public_impl {
    turbowasm_component_public_impl *component;
    turbowasm_component_exec exec;
    uint32_t ref_count;
    turbowasm_component_host_resource *resources;
    uint32_t resource_count;
    uint32_t host_transfer_count, host_transfer_limit;
    turbowasm_component_host_budget host_budget;
    bool host_budget_owned;
    turbowasm_component_host_registration *host_owners;
    uint32_t host_activity;
    bool admission_closed, shutdown_driving;
    struct turbowasm_component_shutdown_drain *shutdown;
    uint64_t shutdown_generation;
    turbowasm_status shutdown_status;
    bool shutdown_complete;

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

bool turbowasm_component_instance_public_impl_retain(turbowasm_component_instance_public_impl *impl);
void turbowasm_component_instance_public_impl_release(turbowasm_component_instance_public_impl *impl);

bool turbowasm_component_host_activity_enter(turbowasm_component_instance_public_impl *instance, bool admission);
void turbowasm_component_host_activity_leave(turbowasm_component_instance_public_impl *instance);
void turbowasm_component_host_register(turbowasm_component_instance_public_impl *instance,
    turbowasm_component_host_registration *registration, void *context,
    bool (*busy)(const void *), turbowasm_status (*cancel)(void *));
void turbowasm_component_host_unregister(turbowasm_component_host_registration *registration);
/* Private first shutdown transition, owner-thread only. Rejects active callbacks
 * and half-built admissions unchanged. Otherwise closes new host admission and
 * idempotently requests cancellation of registered tasks/transfers. OK means
 * cancellation was requested, not that owners, events or guest handles drained.
 * Existing owners retain their carriers and continue progress/delivery/cleanup.
 * An error after admission closes keeps it closed; retry requests cancellation
 * again without repeating an already acknowledged request. Public async stays
 * gated until drain, options and host-value integration are complete. */
turbowasm_status turbowasm_component_instance_request_shutdown_private(
    turbowasm_component_instance_public_impl *instance);
/* Requires a prior shutdown request. YIELDED means owners/loans or a retained
 * cleanup execution remain pending. Options control actual guest destructors.
 * Poll never frees external carriers or consumes their host events. Startup
 * failure preserves guest handles for retry; consumed cleanup failures are
 * saved while pending and returned after all obligations drain. Completion is
 * idempotent and closes all owner operations; Core storage stays until instance
 * destruction. The caller keeps its public instance handle alive through this
 * protocol. Destroy on a requested but incomplete private instance preserves
 * the carrier, so callbacks cannot orphan a retained cleanup driver. */
turbowasm_status turbowasm_component_instance_poll_shutdown_private(
    turbowasm_component_instance_public_impl *instance, const turbowasm_execution_options *options);

/* Borrowed owner-thread ticket; instance must remain alive through shutdown.
 * Runtime owns wait generation/token; shutdown_generation identifies the pass. */
typedef struct turbowasm_component_shutdown_wait {
    const turbowasm_component_instance_public_impl *instance;
    uint64_t shutdown_generation;
    turbowasm_host_wait wait;
} turbowasm_component_shutdown_wait;

/* Reject callback/progress reentry. Query failure preserves output. Completion
 * only records status, runs no guest code and allocates nothing; poll resumes.
 * Foreign, stale-pass and duplicate completions fail unchanged. */
turbowasm_yield_reason turbowasm_component_instance_shutdown_yield_reason_private(
    const turbowasm_component_instance_public_impl *instance);
bool turbowasm_component_instance_shutdown_pending_host_wait_private(
    const turbowasm_component_instance_public_impl *instance, turbowasm_component_shutdown_wait *out);
turbowasm_status turbowasm_component_instance_shutdown_complete_host_wait_private(
    turbowasm_component_instance_public_impl *instance, turbowasm_component_shutdown_wait wait, int status);

/* Private retained loader for async host-boundary integration and its tests. */
turbowasm_status turbowasm_component_load_async_private(turbowasm_component *component,
    const uint8_t *bytes, size_t size, const turbowasm_runtime_config *config);

/* Private constructor used while the public async boundary is being completed.
 * The ordinary loader and public instance constructor retain their async gate. */
turbowasm_status turbowasm_component_instance_create_async_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits);
/* Same retained construction, with copied import descriptors. Callback contexts
 * are borrowed until instance destruction, including pending cleanup waits. */
turbowasm_status turbowasm_component_instance_create_async_with_import_sets_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_exec_async_limits *limits,
    const turbowasm_component_exec_imports *imports, size_t import_count);

void turbowasm_component_async_options_init_private(turbowasm_component_async_options *options);
turbowasm_status turbowasm_component_instance_create_async_with_options_private(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    const turbowasm_component_async_options *options,
    const turbowasm_component_exec_imports *imports, size_t import_count);

/* Shared admission check. Options-backed instances accept only their own budget;
 * primitive test instances keep the explicit caller-budget contract. */
bool turbowasm_component_host_budget_valid(const turbowasm_component_instance_public_impl *instance,
    const turbowasm_component_host_budget *budget);

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

typedef struct turbowasm_component_host_task { void *impl; } turbowasm_component_host_task;
/* Private async root owner. Name/inputs are needed only during creation.
 * Successful creation consumes move inputs; failure keeps them intact. Budget
 * and source binary bytes outlive the owner. All operations run on the instance
 * owner thread. Endpoint host-value integration is still private work. */
turbowasm_status turbowasm_component_host_task_create(turbowasm_component_host_task *owner,
    turbowasm_component_instance_public_impl *instance, turbowasm_name name,
    const turbowasm_component_host_value *arguments, size_t count, bool move,
    turbowasm_component_host_budget *budget);
turbowasm_status turbowasm_component_host_task_resume(turbowasm_component_host_task *owner,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_host_task_request_cancel(turbowasm_component_host_task *owner);
/* Borrowed runtime view for state/host-wait progress, never for direct mutation
 * or destruction of the task. Invalidated when the owner is destroyed. */
const turbowasm_component_task *turbowasm_component_host_task_view(const turbowasm_component_host_task *owner);
/* Complete Core exit is required. Allocation/byte failure keeps the result for
 * retry. Result/count outputs are changed only on successful delivery. */
turbowasm_status turbowasm_component_host_task_take_result(turbowasm_component_host_task *owner,
    turbowasm_component_host_value *out, size_t *count);
/* Live/reentrant owners reject destruction unchanged. Completed owners are
 * freed despite a destructor error; the primary guest error takes precedence. */
turbowasm_status turbowasm_component_host_task_destroy(turbowasm_component_host_task *owner);

typedef struct turbowasm_component_host_endpoint { void *impl; } turbowasm_component_host_endpoint;
/* Private finite host owner. Graph/type comes from the retained instance; both
 * empty output owners are published together. Budget outlives reservations.
 * Owner-thread only, output/source cells are exclusive through each transition. */
turbowasm_status turbowasm_component_host_endpoint_pair_create(
    turbowasm_component_host_endpoint *reader, turbowasm_component_host_endpoint *writer,
    turbowasm_component_instance_public_impl *instance, uint32_t type,
    turbowasm_component_host_budget *budget);
/* Read-only borrowed view, NULL while driving and invalidated by move/destroy.
 * Never mutate or close it. */
const turbowasm_component_endpoint *turbowasm_component_host_endpoint_view(
    const turbowasm_component_host_endpoint *owner);
/* Internal transfer driver: successful submit borrows the stable canonical
 * buffer and its payload/graph until event delivery. Failed admission preserves
 * them. Driver is borrowed during submission, including retained realloc. */
turbowasm_status turbowasm_component_host_endpoint_submit(turbowasm_component_host_endpoint *owner,
    turbowasm_component_buffer *buffer, turbowasm_component_task *driver);
turbowasm_status turbowasm_component_host_endpoint_take(turbowasm_component_host_endpoint *owner,
    turbowasm_component_event *event);
turbowasm_status turbowasm_component_host_endpoint_cancel(turbowasm_component_host_endpoint *owner);
/* Success consumes a readable owner into an empty canonical value and returns
 * its host byte charge. Failure preserves ownership and output. */
turbowasm_status turbowasm_component_host_endpoint_into_value(
    turbowasm_component_host_endpoint *owner, turbowasm_component_value *out);
/* Fresh idle canonical readable end in retained domain storage. Allocates before
 * taking source; failure preserves source/budget/output. The receiving instance
 * stays alive while the host body exists; the pair retains its creation instance. */
turbowasm_status turbowasm_component_host_endpoint_from_value(
    turbowasm_component_host_endpoint *owner, turbowasm_component_instance_public_impl *instance,
    turbowasm_component_value *source, turbowasm_component_host_budget *budget);
/* Busy/reentrant destruction rejects without mutation. Idle close returns the
 * byte reservation and instance reference. Empty owner destruction succeeds. */
turbowasm_status turbowasm_component_host_endpoint_destroy(turbowasm_component_host_endpoint *owner);

typedef struct turbowasm_component_host_transfer { void *impl; } turbowasm_component_host_transfer;
typedef struct turbowasm_component_host_transfer_state {
    uint32_t length, progress;
    turbowasm_status status;
    bool readable, terminal;
    turbowasm_component_event event;
} turbowasm_component_host_transfer_state;
/* Private canonical storage boundary, owner-thread only. Inputs are exclusively
 * borrowed through admission, including callbacks. Write trees are unique,
 * Runtime-owned, with fresh retained resource/endpoint leaves and no borrows.
 * Success moves endpoint and write cells; failure preserves both. Read capacity
 * counts retained payload bytes beyond the separately charged root cells.
 * Budget/source binary bytes outlive the transfer; limits are finite. */
turbowasm_status turbowasm_component_host_transfer_read(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *endpoint, uint32_t count, size_t payload_capacity,
    turbowasm_component_host_budget *budget, turbowasm_component_task *driver);
turbowasm_status turbowasm_component_host_transfer_write_move(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *endpoint, turbowasm_component_value *values, uint32_t count,
    turbowasm_component_host_budget *budget, turbowasm_component_task *driver);
/* Delivers the endpoint event once. Error delivery returns the primary error,
 * releases the buffer borrow and preserves event output. YIELDED is pending. */
turbowasm_status turbowasm_component_host_transfer_poll(turbowasm_component_host_transfer *owner,
    turbowasm_component_event *event);
turbowasm_status turbowasm_component_host_transfer_cancel(turbowasm_component_host_transfer *owner);
turbowasm_status turbowasm_component_host_transfer_state_get(const turbowasm_component_host_transfer *owner,
    turbowasm_component_host_transfer_state *out);
/* Terminal-only borrowed canonical cells: read gives received cells; write gives
 * its untransferred tail. Never move/destroy/mutate through this view. It stays
 * charged and valid until destruction. No partial public result API is exposed. */
const turbowasm_component_value *turbowasm_component_host_transfer_values(
    const turbowasm_component_host_transfer *owner, uint32_t *count);
turbowasm_status turbowasm_component_host_transfer_take_endpoint(turbowasm_component_host_transfer *owner,
    turbowasm_component_host_endpoint *out);
/* Live/reentrant destruction preserves ownership. Terminal destruction cleans
 * every cell even after a destructor error; primary copy failure takes priority. */
turbowasm_status turbowasm_component_host_transfer_destroy(turbowasm_component_host_transfer *owner);

#endif /* TURBOWASM_COMPONENT_API_INTERNAL_H */
