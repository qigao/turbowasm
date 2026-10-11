#include "clock_guest.h"

static int64_t initial;

/* Imports are usable during Reactor initialization too. A failed clock prevents
 * this application's startup; later failures are returned to its host caller.
 */
__attribute__((constructor)) static void initialize_clock(void) {
    initial = app_clock_now_ns();
    if (initial < 0) __builtin_trap();
}

int64_t clock_initial(void) { return initial; }
int64_t clock_sample(void) { return app_clock_now_ns(); }
int clock_close(void) { return 0; }
