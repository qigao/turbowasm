#ifndef TURBOWASM_WASI02_POLL_NATIVE_IO_H
#define TURBOWASM_WASI02_POLL_NATIVE_IO_H

#include "wasi02_poll.h"

#include <salts/native_io.h>

#include <stddef.h>
#include <stdint.h>

typedef int (*turbowasm_wasi02_native_io_cancel_fn)(
    native_io_backend *backend,
    native_io_request request);

typedef struct turbowasm_wasi02_native_io_poll {
    void *impl;
} turbowasm_wasi02_native_io_poll;

/*
 * Bounded NativeIO completion router for WASI 0.2 pollables.
 *
 * This layer never submits I/O and never owns payload/address storage.
 * register_request borrows one already-active NativeIO request identity whose
 * operation lifetime remains owned by the stream/capability provider.
 *
 * route_capacity bounds simultaneously suspended Runtime wait-sets.
 * max_members_per_route bounds one WIT poll() wait-set.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_init(
    turbowasm_wasi02_native_io_poll *adapter,
    native_io_backend *backend,
    size_t pollable_capacity,
    size_t route_capacity,
    size_t max_members_per_route,
    turbowasm_wasi02_native_io_cancel_fn cancel_fn);

turbowasm_status turbowasm_wasi02_native_io_poll_destroy(
    turbowasm_wasi02_native_io_poll *adapter);

/*
 * Register one active NativeIO request and return an opaque provider rep for
 * turbowasm_wasi02_pollable_new(). Duplicate active request identities are
 * rejected.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_register_request(
    turbowasm_wasi02_native_io_poll *adapter,
    native_io_request request,
    turbowasm_value *out_provider_rep);

/* Populate the generic poll provider callbacks backed by this adapter. */
turbowasm_status turbowasm_wasi02_native_io_poll_provider(
    turbowasm_wasi02_native_io_poll *adapter,
    turbowasm_wasi02_poll_provider *out_provider);

/*
 * Route one terminal completion observed by the NativeIO owner.
 * Matching pollables become ready. Every suspended wait-set containing that
 * pollable is completed through its exact Runtime host-wait generation.
 *
 * The completion remains caller-owned; this adapter does not consume payload
 * storage or observe the NativeIO backend itself.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_complete(
    turbowasm_wasi02_native_io_poll *adapter,
    const native_io_completion *completion);

#endif /* TURBOWASM_WASI02_POLL_NATIVE_IO_H */
