#ifndef TURBOWASM_WASI02_POLL_H
#define TURBOWASM_WASI02_POLL_H

#include "component_exec.h"
#include "component_resource.h"

#include <turbowasm/wasi02.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


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

/* Canonical destruction consumes the logical handle even if its provider fails.
 * The direct drop operation above keeps its existing retryable contract. */
turbowasm_status turbowasm_wasi02_pollable_release(
    turbowasm_wasi02_poll *poll, uint32_t resource);

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

turbowasm_status turbowasm_wasi02_poll_imports(
    turbowasm_wasi02_poll *poll,
    turbowasm_component_exec_imports *out_imports);

turbowasm_status turbowasm_wasi02_poll_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_poll *poll);

#endif /* TURBOWASM_WASI02_POLL_H */
