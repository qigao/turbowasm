#include <stdlib.h>
#include "../internal/exit_callbacks.h"

#include "../wasi/wasi.h"

/* C11 §7.22.4.7: quick_exit runs at_quick_exit handlers in LIFO order and
 * then calls _Exit.  It does not flush streams or run atexit handlers. */
static metallic_exit_callbacks callbacks;

int at_quick_exit(void (*fn)(void))
{
    return metallic_exit_push(&callbacks, fn);
}

void __run_quick_exit_(void) { metallic_exit_run(&callbacks); }

_Noreturn void quick_exit(int rc)
{
    __run_quick_exit_();

    __wasi_proc_exit((__wasi_exitcode_t)(unsigned)rc);
}
