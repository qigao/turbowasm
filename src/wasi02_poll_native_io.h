#ifndef TURBOWASM_WASI02_POLL_NATIVE_IO_H
#define TURBOWASM_WASI02_POLL_NATIVE_IO_H

#include "wasi02_poll.h"

#include <salts/native_io.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*turbowasm_wasi02_native_io_cancel_fn)(
    native_io_backend *backend,
    native_io_request request);

typedef struct turbowasm_wasi02_native_io_poll {
    void *impl;
} turbowasm_wasi02_native_io_poll;

/*
 * Dynamic readiness source used by reusable WASI pollables whose readiness is
 * derived from capability state rather than from one permanent NativeIO
 * request. prepare() may bind one current NativeIO request identity when the
 * source is not ready. drop() releases the source's capability lease.
 */
typedef turbowasm_status
(*turbowasm_wasi02_native_io_dynamic_ready_fn)(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready);

typedef turbowasm_status
(*turbowasm_wasi02_native_io_dynamic_prepare_fn)(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready,
    native_io_request *out_request);

typedef turbowasm_status
(*turbowasm_wasi02_native_io_dynamic_drop_fn)(
    void *context,
    turbowasm_value source_rep);

/*
 * Bounded NativeIO completion router for WASI 0.2 pollables.
 *
 * This layer never owns payload/address storage and never observes NativeIO.
 * Fixed request registrations borrow an already-active request identity.
 * Dynamic registrations delegate readiness/request preparation to a bounded
 * capability owner while retaining one logical pollable slot.
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
 * turbowasm_wasi02_pollable_new(). Multiple logical pollables may share the
 * same generation-safe request identity; one terminal completion fans out to
 * every alias.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_register_request(
    turbowasm_wasi02_native_io_poll *adapter,
    native_io_request request,
    turbowasm_value *out_provider_rep);

/*
 * Allocate one logical pollable that is already terminal/ready.
 *
 * This carries no NativeIO request identity and never owns cancellation.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_register_ready(
    turbowasm_wasi02_native_io_poll *adapter,
    turbowasm_value *out_provider_rep);

/*
 * Register one reusable dynamic readiness source.
 *
 * The source callbacks are borrowed for the pollable lifetime. prepare() must
 * return either out_ready=true with an invalid/zero request, or
 * out_ready=false with one valid generation-safe NativeIO request.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_register_dynamic(
    turbowasm_wasi02_native_io_poll *adapter,
    void *source_context,
    turbowasm_value source_rep,
    turbowasm_wasi02_native_io_dynamic_ready_fn ready_fn,
    turbowasm_wasi02_native_io_dynamic_prepare_fn prepare_fn,
    turbowasm_wasi02_native_io_dynamic_drop_fn drop_fn,
    turbowasm_value *out_provider_rep);

/* Populate the generic poll provider callbacks backed by this adapter. */
turbowasm_status turbowasm_wasi02_native_io_poll_provider(
    turbowasm_wasi02_native_io_poll *adapter,
    turbowasm_wasi02_poll_provider *out_provider);

/*
 * Route one terminal completion observed by the NativeIO owner.
 *
 * Fixed request pollables become permanently ready. Dynamic pollables only
 * have their current request binding cleared; subsequent ready() calls re-read
 * source state, so one pollable remains reusable across capability transitions.
 * Every affected suspended wait-set is completed exactly once.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_complete(
    turbowasm_wasi02_native_io_poll *adapter,
    const native_io_completion *completion);

/*
 * Explicitly detach one aggregate Runtime wait before destroying/abandoning
 * the corresponding execution. This does not cancel NativeIO requests; their
 * owners remain responsible for cancellation and terminal drain.
 */
turbowasm_status turbowasm_wasi02_native_io_poll_abandon_wait(
    turbowasm_wasi02_native_io_poll *adapter,
    uintptr_t operation_token);

#endif /* TURBOWASM_WASI02_POLL_NATIVE_IO_H */
