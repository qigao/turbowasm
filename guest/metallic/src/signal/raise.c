#include <signal.h>
#include <stdlib.h>

/* Internal helper from src/signal/signal.c — not exposed in <signal.h>. */
__sighandler_t __metallic_signal_handler(int);

int raise(int sig)
{
    __sighandler_t h = __metallic_signal_handler(sig);

    if (h == SIG_IGN)
        return 0;

    if (h == SIG_DFL) {
        /* Default action for the C11 terminating signals: abort the
         * process. For anything else (including SIGUSR1 when the user
         * has not installed a handler), the default action is to do
         * nothing. Terminate directly: abort itself raises SIGABRT and must
         * not recurse back through abort when the default action is selected.
         */
        switch (sig) {
        case SIGABRT:
        case SIGFPE:
        case SIGILL:
        case SIGINT:
        case SIGSEGV:
        case SIGTERM:
            _Exit(128 + sig);
        default:
            return 0;
        }
    }

    h(sig);
    return 0;
}
