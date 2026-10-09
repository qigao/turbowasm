#ifndef TURBOWASM_WASI_DISPATCH_PRIVATE_H
#define TURBOWASM_WASI_DISPATCH_PRIVATE_H

#include <turbowasm/wasi_sockets.h>
#include <salts/thread.h>
#include <ring_buffer.h>

/* Internal owner bridge for #426. This is not an installed API. The Preview1
 * integration owns lifecycle admission; callbacks/frames remain borrowed until
 * request acknowledgement. All ring accesses require mutex, including owner. */
typedef struct tw_wasi_dispatch {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    const void *owner;
    void (*wake)(void *);
    void *wake_context;
    ring_data_type queue;
    uint8_t *storage;
    size_t capacity, outstanding, cleanup_waiters, direct_calls, callers;
    bool stopping, advancing;
} tw_wasi_dispatch;

turbowasm_status tw_wasi_dispatch_init(tw_wasi_dispatch *, size_t,
    void (*wake)(void *), void *);
bool tw_wasi_dispatch_is_owner(const tw_wasi_dispatch *);
/* Destruction requires exclusive host admission and an unattached FS table. */
turbowasm_status tw_wasi_dispatch_destroy(tw_wasi_dispatch *);
uint32_t tw_wasi_dispatch_call(tw_wasi_dispatch *, uint32_t (*run)(void *),
    void *frame, bool cleanup, bool must_admit);
turbowasm_status tw_wasi_dispatch_advance(tw_wasi_dispatch *);
turbowasm_status tw_wasi_dispatch_stop(tw_wasi_dispatch *);
/* The caller holds mutex. Counts include mandatory cleanup waiting for space. */
bool tw_wasi_dispatch_idle_locked(const tw_wasi_dispatch *);

#endif
