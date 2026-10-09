#include <stdlib.h>
#include "../internal/exit_callbacks.h"

/* C11 §7.22.4.2 mandates at least 32 atexit slots and LIFO execution. */
static metallic_exit_callbacks callbacks;

int atexit(void (*fn)(void))
{
    return metallic_exit_push(&callbacks, fn);
}

void __run_atexit_(void)
{
    metallic_exit_run(&callbacks);
}
