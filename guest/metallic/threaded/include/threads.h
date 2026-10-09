#ifndef METALLIC_THREADS_H
#define METALLIC_THREADS_H

/* C11 thread API for the opt-in Metallic shared-memory SDK profile. Handles
 * are guest-local slot/generation values, never native host thread handles.
 * Rebuild every guest object/archive with the matching THREADS profile. */
#if !defined(__METALLIC_THREADS__) || !defined(__wasm_atomics__)
#error "Metallic threads require the matching shared-memory SDK profile"
#endif
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>

typedef struct { uint32_t slot, generation; } thrd_t;
typedef int (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);
typedef struct { uint32_t slot, generation; } tss_t;
typedef struct { atomic_uint state; } once_flag;
typedef struct { atomic_uint owner; unsigned depth, type; } mtx_t;
typedef struct { _Atomic uint64_t sequence; } cnd_t;

#define ONCE_FLAG_INIT { ATOMIC_VAR_INIT(0) }
#define TSS_DTOR_ITERATIONS 4
#define thread_local _Thread_local
enum { thrd_success, thrd_nomem, thrd_timedout, thrd_busy, thrd_error };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

void call_once(once_flag *, void (*)(void));
/* create borrows its argument until the child returns; success publishes a
 * handle, failure leaves the output unchanged. Host/guest capacity or allocation
 * exhaustion returns thrd_nomem; closed admission/invalid input/spawn errors
 * return thrd_error. Each child owns 128 KiB stack and at most 64 KiB native TLS.
 * At most 32 child records (including retained joinable results) exist. */
int thrd_create(thrd_t *, thrd_start_t, void *);
thrd_t thrd_current(void);
int thrd_equal(thrd_t, thrd_t);
/* join consumes a joinable handle after its stack-free terminal; result may
 * be NULL. detach consumes the join right, not the running child's storage.
 * Invalid/stale/root/self-join/already-claimed handles return thrd_error.
 * Blocking calls need an independently runnable child; owned host pool capacity
 * must cover all admitted children. thrd_exit runs TSS destructors before
 * publishing the result. Main return/exit terminates the entire host group. */
int thrd_join(thrd_t, int *);
int thrd_detach(thrd_t);
_Noreturn void thrd_exit(int);
int thrd_sleep(const struct timespec *, struct timespec *);
void thrd_yield(void);
/* Mutex/condition objects are caller-owned and require quiescence at destroy.
 * plain, recursive and timed mutex modes are supported. Timed calls use
 * absolute TIME_UTC deadlines and may return thrd_timedout or thrd_error on
 * invalid times/clock failure. Condition waits release/reacquire the mutex;
 * callers must loop on their predicate. No asynchronous cancellation exists. */
int mtx_init(mtx_t *, int);
void mtx_destroy(mtx_t *);
int mtx_lock(mtx_t *);
int mtx_trylock(mtx_t *);
int mtx_timedlock(mtx_t *restrict, const struct timespec *restrict);
int mtx_unlock(mtx_t *);
int cnd_init(cnd_t *);
void cnd_destroy(cnd_t *);
int cnd_signal(cnd_t *);
int cnd_broadcast(cnd_t *);
int cnd_wait(cnd_t *, mtx_t *);
int cnd_timedwait(cnd_t *restrict, mtx_t *restrict, const struct timespec *restrict);
/* 128 process-wide generation-checked keys, with a value per guest thread.
 * Deleting a key does not destroy its values. Thread exit clears each value
 * before invoking its destructor, up to TSS_DTOR_ITERATIONS passes. */
int tss_create(tss_t *, tss_dtor_t);
void tss_delete(tss_t);
void *tss_get(tss_t);
int tss_set(tss_t, void *);
#endif
