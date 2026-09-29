#ifndef TURBOWASM_NATIVE_IO_H
#define TURBOWASM_NATIVE_IO_H

#include <turbowasm/execution.h>
#include <turbowasm/link.h>

#include <salts/native_io.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional bounded routing adapter between TurboWasm resumable host waits and
 * a caller-owned Salts NativeIO backend.
 *
 * The bridge borrows backend and never submits/observes from another thread.
 * NativeIO remains the operation/progress/terminal truth source. Bridge slots
 * only retain request -> suspended host-wait routing metadata.
 */
typedef struct turbowasm_native_io_bridge {
    void *impl;
} turbowasm_native_io_bridge;

int turbowasm_native_io_bridge_init(
    turbowasm_native_io_bridge *bridge,
    native_io_backend *backend,
    size_t capacity);

int turbowasm_native_io_bridge_destroy(
    turbowasm_native_io_bridge *bridge);

/*
 * Submit one NativeIO operation and suspend the current host callback until
 * the matching terminal completion is routed through bridge_complete().
 *
 * Returns Salts status codes. I/O terminal success/failure remains encoded in
 * out_completion->kind/status exactly as NativeIO defines it.
 */
int turbowasm_native_io_await(
    turbowasm_native_io_bridge *bridge,
    turbowasm_host_call *call,
    const native_io_operation *operation,
    native_io_completion *out_completion);

/*
 * Route one completion previously returned by native_io_backend_observe().
 * The adapter restores the operation's original user_data in the completion
 * copied to the suspended host callback.
 */
int turbowasm_native_io_bridge_complete(
    turbowasm_native_io_bridge *bridge,
    const native_io_completion *completion);

/* Resolve the NativeIO request currently awaited by execution, if any. */
bool turbowasm_native_io_pending_request(
    const turbowasm_native_io_bridge *bridge,
    const turbowasm_execution *execution,
    native_io_request *out_request);

/*
 * Request cancellation through NativeIO. SALTS_OK / SALTS_EALREADY are not
 * terminal; the caller must still observe and route the eventual completion.
 */
int turbowasm_native_io_cancel_execution(
    turbowasm_native_io_bridge *bridge,
    const turbowasm_execution *execution);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_NATIVE_IO_H */
