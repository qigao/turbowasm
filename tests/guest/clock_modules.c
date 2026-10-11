#include <stdint.h>
#include <turbowasm/import.h>

TURBOWASM_IMPORT("test:clock/v1", "now_ns") int64_t other_now_ns(void);
int64_t other_sample(void) { return other_now_ns(); }
