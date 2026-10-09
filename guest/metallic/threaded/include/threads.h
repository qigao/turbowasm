#ifndef METALLIC_THREADS_H
#define METALLIC_THREADS_H

/* Private #426 profile until the complete libc/CRT qualification is delivered. */
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
int thrd_create(thrd_t *, thrd_start_t, void *);
thrd_t thrd_current(void);
int thrd_equal(thrd_t, thrd_t);
int thrd_join(thrd_t, int *);
int thrd_detach(thrd_t);
_Noreturn void thrd_exit(int);
int thrd_sleep(const struct timespec *, struct timespec *);
void thrd_yield(void);
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
int tss_create(tss_t *, tss_dtor_t);
void tss_delete(tss_t);
void *tss_get(tss_t);
int tss_set(tss_t, void *);
#endif
