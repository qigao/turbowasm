#ifndef TURBOWASM_WASI02_POLL_H
#define TURBOWASM_WASI02_POLL_H

#include "component_exec.h"
#include "component_resource.h"
#include "wasi02_provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef turbowasm_status (*turbowasm_wasi02_poll_ready_fn)(
    void *context,
    uint64_t token,
    bool *out_ready);

typedef turbowasm_status (*turbowasm_wasi02_poll_block_fn)(
    void *context,
    turbowasm_host_call *call,
    uint64_t token);

typedef turbowasm_status (*turbowasm_wasi02_poll_many_fn)(
    void *context,
    turbowasm_host_call *call,
    const uint64_t *tokens,
    size_t token_count,
    uint32_t *out_indices,
    size_t result_capacity,
    size_t *out_count);

typedef turbowasm_status (*turbowasm_wasi02_poll_drop_fn)(
    void *context,
    uint64_t token);

typedef struct turbowasm_wasi02_poll_config {
    void *context;
    turbowasm_wasi02_poll_ready_fn ready;
    turbowasm_wasi02_poll_block_fn block;
    turbowasm_wasi02_poll_many_fn poll_many;
    turbowasm_wasi02_poll_drop_fn drop;
} turbowasm_wasi02_poll_config;

typedef struct turbowasm_wasi02_poll {
    turbowasm_wasi02_poll_config config;
    turbowasm_component_resource_table resources;
    uint64_t pollable_identity;
    bool pollable_identity_bound;
    bool initialized;
} turbowasm_wasi02_poll;

turbowasm_status turbowasm_wasi02_poll_init(
    turbowasm_wasi02_poll *poll,
    const turbowasm_wasi02_poll_config *config,
    uint32_t max_pollables);

turbowasm_status turbowasm_wasi02_poll_destroy(
    turbowasm_wasi02_poll *poll);

turbowasm_status turbowasm_wasi02_pollable_new(
    turbowasm_wasi02_poll *poll,
    uint64_t token,
    uint32_t *out_handle);

turbowasm_status turbowasm_wasi02_pollable_drop(
    turbowasm_wasi02_poll *poll,
    uint32_t handle);

turbowasm_status turbowasm_wasi02_pollable_ready(
    turbowasm_wasi02_poll *poll,
    uint32_t handle,
    bool *out_ready);

turbowasm_status turbowasm_wasi02_pollable_block(
    turbowasm_wasi02_poll *poll,
    turbowasm_host_call *call,
    uint32_t handle);

turbowasm_status turbowasm_wasi02_poll_many(
    turbowasm_wasi02_poll *poll,
    turbowasm_host_call *call,
    const uint32_t *handles,
    size_t handle_count,
    uint32_t *out_indices,
    size_t result_capacity,
    size_t *out_count);

turbowasm_status turbowasm_wasi02_poll_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_poll *poll);

#endif /* TURBOWASM_WASI02_POLL_H */
