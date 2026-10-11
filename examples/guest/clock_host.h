#ifndef TURBOWASM_EXAMPLE_CLOCK_HOST_H
#define TURBOWASM_EXAMPLE_CLOCK_HOST_H

#include <turbowasm/link.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Application adapter, not a Runtime ABI. read_ns is synchronous and writes
 * nanoseconds since its context's origin only on success. It must return
 * promptly; this scalar adapter does not support host waits.
 * The host owns this struct, read_ns code and context; keep them alive and the
 * struct immutable until all bound instances/operations have been destroyed.
 * Each instance has one execution owner. Sharing a context across instances
 * requires a concurrency-safe provider; the adapter adds no synchronization.
 */
typedef struct app_clock {
    bool (*read_ns)(void *context, uint64_t *out_ns);
    void *context;
} app_clock;

/* Define app:clock/v1.now_ns : () -> i64. Borrows clock; copies no service state.
 * Returns INVALID_ARGUMENT for a missing clock/reader or invalid linker,
 * LINK_ERROR for a duplicate or provider namespace collision, OUT_OF_MEMORY
 * on allocation failure, otherwise OK. No provider call occurs at registration.
 * Compose on a fresh linker; if any module registration fails, destroy that
 * linker instead of creating a consumer from the partially composed scope.
 * Example: app_clock clock = {read_ns, &service};
 *          status = app_clock_define(&linker, &clock); // check before creation
 */
turbowasm_status app_clock_define(turbowasm_linker *linker, app_clock *clock);

#ifdef __cplusplus
}
#endif
#endif
