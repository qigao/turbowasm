#ifndef TURBOWASM_EXAMPLE_CLOCK_GUEST_H
#define TURBOWASM_EXAMPLE_CLOCK_GUEST_H

#include <stdint.h>
#include <turbowasm/import.h>

#ifdef __cplusplus
extern "C" {
#endif

/* app:clock/v1: () -> i64. Nonnegative nanoseconds since the host-selected
 * origin, or -1 if the provider fails or the value exceeds INT64_MAX.
 * No memory or resource ownership crosses this synchronous call.
 */
TURBOWASM_IMPORT("app:clock/v1", "now_ns")
int64_t app_clock_now_ns(void);

#ifdef __cplusplus
}
#endif
#endif
