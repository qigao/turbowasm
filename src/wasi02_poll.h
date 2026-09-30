#ifndef TURBOWASM_WASI02_POLL_H
#define TURBOWASM_WASI02_POLL_H

#include "component_exec.h"
#include "component_resource.h"

#include <turbowasm/link.h>
#include <turbowasm/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef turbowasm_status (*turbowasm_wasi02_poll_ready_fn)(
    void *context,
    turbowasm_value rep,
    bool *out_ready);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_fn)(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_routed_fn)(
    void *context,
    turbowasm_value rep,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage);

/*
 * Arm one provider-owned wait-set for the supplied pollable reps. Completion
 * of the returned opaque token promises that at least one supplied source is
 * ready (terminal/error readiness counts as ready).
 */
typedef turbowasm_status (*turbowasm_wasi02_poll_arm_many_fn)(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    uintptr_t *out_operation_token);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_many_routed_fn)(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage);

typedef turbowasm_status (*turbowasm_wasi02_poll_drop_fn)(
    void *context,
    turbowasm_value rep);

typedef struct turbowasm_wasi02_poll_provider {
    void *context;
    turbowasm_wasi02_poll_ready_fn ready;
    turbowasm_wasi02_poll_arm_fn arm;
    turbowasm_wasi02_poll_arm_routed_fn arm_routed;
    turbowasm_wasi02_poll_arm_many_fn arm_many;
    turbowasm_wasi02_poll_arm_many_routed_fn arm_many_routed;
    turbowasm_wasi02_poll_drop_fn drop;
} turbowasm_wasi02_poll_provider;

typedef struct turbowasm_wasi02_poll {
    turbowasm_wasi02_poll_provider provider;
    turbowasm_component_resource_table resources;
    uint64_t pollable_identity;
    bool pollable_identity_bound;
    bool initialized;
} turbowasm_wasi02_poll;

turbowasm_status turbowasm_wasi02_poll_init(
    turbowasm_wasi02_poll *poll,
    const turbowasm_wasi02_poll_provider *provider,
    uint32_t max_pollables);

turbowasm_status turbowasm_wasi02_poll_destroy(
    turbowasm_wasi02_poll *poll);

turbowasm_status turbowasm_wasi02_pollable_new(
    turbowasm_wasi02_poll *poll,
    turbowasm_value provider_rep,
    uint32_t *out_resource);

turbowasm_status turbowasm_wasi02_pollable_drop(
    turbowasm_wasi02_poll *poll,
    uint32_t resource);

turbowasm_status turbowasm_wasi02_pollable_ready(
    turbowasm_wasi02_poll *poll,
    uint32_t resource,
    bool *out_ready);

/*
 * Return immediately when ready. Otherwise arm exactly one provider wait and
 * suspend through the current Runtime host-call frame. A one-shot call cannot
 * wait and returns TURBOWASM_UNSUPPORTED before provider arm.
 */
turbowasm_status turbowasm_wasi02_pollable_block(
    turbowasm_wasi02_poll *poll,
    uint32_t resource,
    turbowasm_host_call *call);

/*
 * WIT poll(list<borrow<pollable>>) wait-any semantics.
 *
 * Returns a Component list<u32> containing every ready input index. Empty input
 * traps. If no source is ready, only provider arm_many is allowed; this never
 * serially blocks individual pollables.
 */
turbowasm_status turbowasm_wasi02_poll_many(
    turbowasm_wasi02_poll *poll,
    const uint32_t *resources,
    size_t resource_count,
    turbowasm_host_call *call,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap);

turbowasm_status turbowasm_wasi02_poll_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_poll *poll);

#endif /* TURBOWASM_WASI02_POLL_H */
