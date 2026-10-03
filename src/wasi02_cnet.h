#ifndef TURBOWASM_WASI02_CNET_H
#define TURBOWASM_WASI02_CNET_H

#include <turbowasm/wasi02_sockets.h>

#include <cnet/cnet.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_wasi02_cnet {
    void *impl;
} turbowasm_wasi02_cnet;

typedef struct turbowasm_wasi02_cnet_config {
    native_io_backend_kind backend;
    uint32_t socket_capacity;
    size_t default_listen_backlog;
} turbowasm_wasi02_cnet_config;

/*
 * Private poll-registry seam. It keeps the CNet core independent from one
 * concrete W4 poll implementation while allowing a single shared poll
 * namespace to register reusable socket readiness sources.
 */
typedef turbowasm_status
(*turbowasm_wasi02_cnet_poll_ready_fn)(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready);

typedef turbowasm_status
(*turbowasm_wasi02_cnet_poll_prepare_fn)(
    void *context,
    turbowasm_value source_rep,
    bool *out_ready,
    native_io_request *out_request);

typedef turbowasm_status
(*turbowasm_wasi02_cnet_poll_drop_fn)(
    void *context,
    turbowasm_value source_rep);

typedef turbowasm_status
(*turbowasm_wasi02_cnet_poll_register_fn)(
    void *registry_context,
    void *source_context,
    turbowasm_value source_rep,
    turbowasm_wasi02_cnet_poll_ready_fn ready_fn,
    turbowasm_wasi02_cnet_poll_prepare_fn prepare_fn,
    turbowasm_wasi02_cnet_poll_drop_fn drop_fn,
    turbowasm_value *out_pollable_rep);

/*
 * Bounded released-Salts CNet provider for the WASI 0.2 TCP control plane.
 *
 * The control-only initializer owns unbound/bound/listening CNet socket owners
 * and creates no progress engine. It never exposes a native fd/SOCKET/HANDLE.
 */
turbowasm_status turbowasm_wasi02_cnet_init(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config);

/*
 * External-progress initializer for the #393 data-plane substrate.
 *
 * CNet borrows external_backend and may submit/cancel NativeIO work through it,
 * but CNet never observes, closes or destroys that backend. The embedding
 * runtime remains the single NativeIO progress owner and must route observed
 * completions through turbowasm_wasi02_cnet_route_external_completion().
 *
 * This substrate deliberately does not enable connect/accept/stream/subscribe
 * provider callbacks yet; those become visible only after W4 stream/poll
 * ownership is composed over this same backend.
 */
turbowasm_status turbowasm_wasi02_cnet_init_external(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config,
    native_io_backend *external_backend,
    const cnet_client_config *client_config);

/*
 * Attach the one poll registry used by the surrounding WASI02 context.
 * Must be configured before obtaining a socket provider that exposes
 * tcp-socket.subscribe.
 */
turbowasm_status turbowasm_wasi02_cnet_attach_poll_registry(
    turbowasm_wasi02_cnet *adapter,
    void *registry_context,
    turbowasm_wasi02_cnet_poll_register_fn register_fn);

/*
 * Internal socket-readiness substrate used by the later dynamic TCP poll
 * provider. This does not create a WASI pollable by itself.
 *
 * poll_ready is a side-effect-free state snapshot. Stable synchronous states
 * are ready immediately; a listening socket becomes ready only after the
 * currently armed accept request reaches a terminal result.
 *
 * poll_prepare arms at most one shared external accept request for a listening
 * socket and returns its generation-safe NativeIO identity. Repeated calls
 * while that request is active return the same identity. If readiness is
 * already terminal, out_ready is true and out_request remains zero.
 *
 * Listener-owned terminal completions remain routable after the external CNet
 * client has stopped; listener accept requests are not cnet_client requests.
 */
turbowasm_status turbowasm_wasi02_cnet_socket_poll_ready(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_value socket_rep,
    bool *out_ready);

turbowasm_status turbowasm_wasi02_cnet_socket_poll_prepare(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_value socket_rep,
    bool *out_ready,
    native_io_request *out_request);

/* External-progress wrappers preserve exact Salts status semantics. */
int turbowasm_wasi02_cnet_advance_external(
    turbowasm_wasi02_cnet *adapter,
    size_t *out_events);

int turbowasm_wasi02_cnet_route_external_completion(
    turbowasm_wasi02_cnet *adapter,
    const native_io_completion *completion,
    bool *out_consumed,
    size_t *out_events);

int turbowasm_wasi02_cnet_external_timeout(
    turbowasm_wasi02_cnet *adapter,
    uint32_t max_wait_ms,
    uint32_t *out_timeout_ms);

/*
 * Stop only the CNet external client. SALTS_EBUSY means the embedding runtime
 * must continue observing/routing NativeIO completions before retrying.
 */
int turbowasm_wasi02_cnet_stop_external(
    turbowasm_wasi02_cnet *adapter);

turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter);

turbowasm_status turbowasm_wasi02_cnet_socket_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_socket_provider *out_provider);

#endif /* TURBOWASM_WASI02_CNET_H */
