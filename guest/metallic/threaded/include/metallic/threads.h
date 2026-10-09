#ifndef METALLIC_THREAD_LIFETIME_H
#define METALLIC_THREAD_LIFETIME_H

#include <threads.h>

/* Root-thread lifecycle extension. Stop application child work before calling.
 * Permanently closes spawn admission, waits for stack-free child terminals,
 * then releases retained child stacks/TLS/results. NULL waits without a
 * deadline; otherwise use an absolute TIME_UTC deadline. Returns thrd_success,
 * thrd_timedout, or thrd_error for a wrong thread, invalid time or clock failure.
 * Timeout/error on the root retains live storage and closed admission; retry
 * is allowed. Success is idempotent. Wrong-thread calls do not change admission.
 * Does not flush streams, run atexit/application cleanup or terminate the root.
 * After a fatal group error, use host teardown after actual child termination
 * instead of attempting guest cleanup. Example: if (metallic_threads_close(&t)
 * == thrd_timedout) retain the instance and retry after children can finish.
 */
int metallic_threads_close(const struct timespec *utc_deadline);

#endif
