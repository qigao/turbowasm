#include <signal.h>
#ifdef __METALLIC_THREADS__
#include <stdatomic.h>
#endif

/* Placeholder used as SIG_IGN. Defined here so its address can serve
 * as a sentinel distinct from any user-supplied handler — on wasm32
 * a magic integer like (void*)1 would alias whatever real function
 * lands at function-table index 1. Must remain extern (non-static)
 * so SIG_IGN in <signal.h> can take its address. */
void __metallic_sig_ign(int sig) { (void)sig; }

/* Largest signal number we track. 32 covers the six C11 signals
 * (SIGABRT=6, SIGFPE=8, SIGILL=4, SIGINT=2, SIGSEGV=11, SIGTERM=15),
 * SIGUSR1=10, and gives headroom for any common Linux signum a
 * portable program might pass in.
 */
#define SIG_MAX 32

/* Zero-initialised => every entry is SIG_DFL (== (__sighandler_t)0). */
#ifdef __METALLIC_THREADS__
static _Atomic(__sighandler_t) handlers_[SIG_MAX];
#else
static __sighandler_t handlers_[SIG_MAX];
#endif

__sighandler_t signal(int sig, __sighandler_t handler)
{
    if (sig < 0 || sig >= SIG_MAX)
        return SIG_ERR;

#ifdef __METALLIC_THREADS__
    return atomic_exchange_explicit(&handlers_[sig], handler, memory_order_acq_rel);
#else
    __sighandler_t prev = handlers_[sig];
    handlers_[sig] = handler;
    return prev;
#endif
}

/* Internal lookup used by raise(). Out-of-range sigs report SIG_DFL
 * so the caller can decide between abort() and silent return.
 */
__sighandler_t __metallic_signal_handler(int sig)
{
    if (sig < 0 || sig >= SIG_MAX)
        return SIG_DFL;
#ifdef __METALLIC_THREADS__
    return atomic_load_explicit(&handlers_[sig], memory_order_acquire);
#else
    return handlers_[sig];
#endif
}
