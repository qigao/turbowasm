#include <limits.h>
#include <stdlib.h>

/* The allocation deliberately happens in a constructor: Reactor startup must
 * initialize libc before constructors and must not repeat them for each call. */
static int *counter;
static int constructor_count;
__attribute__((constructor)) static void initialize_counter(void) {
    ++constructor_count;
    counter = calloc(1, sizeof(*counter));
    if (!counter) __builtin_trap();
}

int counter_add(int amount) {
    if (!counter || amount < 0 || *counter > INT_MAX - amount) return -1;
    *counter += amount;
    return *counter;
}
int counter_initializations(void) { return constructor_count; }
int counter_close(void) {
    free(counter);
    counter = NULL;
    return 0;
}
