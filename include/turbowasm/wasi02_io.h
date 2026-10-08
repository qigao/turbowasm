#ifndef TURBOWASM_WASI02_IO_H
#define TURBOWASM_WASI02_IO_H

#include <turbowasm/wasi02.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbowasm_wasi02_io { void *impl; } turbowasm_wasi02_io;
/* Opaque identity; do not construct or compare its fields to infer readiness. */
typedef struct turbowasm_wasi02_io_source { uint64_t domain, token; } turbowasm_wasi02_io_source;
typedef struct turbowasm_wasi02_io_config {
    size_t size;
    uint32_t api_version;
    uint32_t sources, streams, errors, subscriptions, routes, members_per_route;
} turbowasm_wasi02_io_config;

typedef struct turbowasm_wasi02_io_source_ops {
    void *context;
    turbowasm_status (*ready)(void *context, bool *out_ready);
    void (*retain)(void *context);
    void (*release)(void *context);
} turbowasm_wasi02_io_source_ops;
typedef enum turbowasm_wasi02_io_stream_kind {
    TURBOWASM_WASI02_IO_INPUT = 1, TURBOWASM_WASI02_IO_OUTPUT = 2
} turbowasm_wasi02_io_stream_kind;
typedef enum turbowasm_wasi02_io_cli_kind {
    TURBOWASM_WASI02_IO_STDIN = 0, TURBOWASM_WASI02_IO_STDOUT = 1, TURBOWASM_WASI02_IO_STDERR = 2
} turbowasm_wasi02_io_cli_kind;

void turbowasm_wasi02_io_config_init(turbowasm_wasi02_io_config *config);
/* All carriers start zeroed. Config and allocator policy are copied; allocator
 * context remains borrowed. NULL config selects finite defaults. API version 1
 * requires size == sizeof(config) and every capacity nonzero. Calls belong to
 * one serialized host owner; callbacks must not reenter mutation/progress.
 * Invalid configuration/identity yields INVALID_ARGUMENT, capacity/allocation
 * exhaustion yields OUT_OF_MEMORY. Providers trap stale/wrong-kind reps. */
turbowasm_status turbowasm_wasi02_io_init(turbowasm_wasi02_io *io,
    const turbowasm_wasi02_io_config *config, const turbowasm_runtime_config *runtime_config);
/* Returns INVALID_ARGUMENT while sources, aliases, errors, waits, facades or
 * adapters remain live; failure preserves the owner. Empty destroy succeeds. */
turbowasm_status turbowasm_wasi02_io_destroy(turbowasm_wasi02_io *io);
/* Copies ops, calls retain exactly once on success, and release after close
 * and retirement of the final alias/wait/error. All three callbacks required.
 * out_source must be zeroed. Failed registration consumes no ownership. */
turbowasm_status turbowasm_wasi02_io_source_register(turbowasm_wasi02_io *io,
    const turbowasm_wasi02_io_source_ops *ops, turbowasm_wasi02_io_source *out_source);
/* Notification hint; allowed from a callback on the same host owner. Readiness
 * remains authoritative in ops.ready and may change repeatedly. */
turbowasm_status turbowasm_wasi02_io_source_changed(turbowasm_wasi02_io *io, turbowasm_wasi02_io_source source);
/* Consumes and zeros source on success. Existing aliases become ready and
 * retain their source; closing a source does not cancel transport requests. */
turbowasm_status turbowasm_wasi02_io_source_close(turbowasm_wasi02_io *io, turbowasm_wasi02_io_source *source);
/* Returns an owned provider rep, released by the domain's poll.drop callback.
 * One alias can be reused across arbitrarily many readiness cycles. */
turbowasm_status turbowasm_wasi02_io_pollable_register(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_source source, turbowasm_value *out_owned_rep);
/* Takes owned_rep only on success and copies the operation bundle. The source
 * owner must keep operation.context/rep valid through stream and error drops.
 * The returned domain rep is released through input_drop/output_drop. Stream
 * subscribe uses source; operation bundle subscribe callbacks are not used.
 * Data methods reserve an error slot before entering the provider. */
turbowasm_status turbowasm_wasi02_io_stream_register(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_stream_kind kind, const turbowasm_wasi02_stream_provider *operations,
    turbowasm_value owned_rep, turbowasm_wasi02_io_source source, turbowasm_value *out_domain_rep);
/* Register before providers/facade publication. A factory returns a newly
 * owned domain stream of the correct direction and may register that stream
 * inside its callback. Its context stays borrowed for the domain lifetime. */
turbowasm_status turbowasm_wasi02_io_cli_factory_set(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_cli_kind kind, turbowasm_wasi02_stream_factory_fn create, void *context);
/* Copies the shared bundles and freezes factory configuration. Both contexts
 * borrow io. Use io_wasi02_init for a retaining async-capable facade. */
turbowasm_status turbowasm_wasi02_io_providers(turbowasm_wasi02_io *io,
    turbowasm_wasi02_stream_provider *out_streams, turbowasm_wasi02_poll_provider *out_poll);
/* Rechecks bounded waits, preserving duplicate input positions. Completes the
 * exact Runtime wait generation; the host resumes Component calls explicitly.
 * No native observation or execution dispatch is performed by this function. */
turbowasm_status turbowasm_wasi02_io_advance(turbowasm_wasi02_io *io);
/* Initializes a facade from a copied config, installing this domain's shared
 * stream/poll callbacks and wait cleanup. Existing conflicting bundles fail. */
turbowasm_status turbowasm_wasi02_io_wasi02_init(turbowasm_wasi02_io *io,
    turbowasm_wasi02 *wasi02, const turbowasm_wasi02_config *config,
    const turbowasm_runtime_config *runtime_config);

#ifdef __cplusplus
}
#endif
#endif
