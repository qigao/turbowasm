#include "wasi_dispatch_private.h"
#include <stdlib.h>
#include <string.h>

typedef struct tw_dispatch_command {
    uint32_t (*run)(void *);
    void *frame;
    uint32_t error;
    bool done;
} tw_dispatch_command;

bool tw_wasi_dispatch_is_owner(const tw_wasi_dispatch *d) {
    return d && d->owner == cmeta_thread_current_token();
}

turbowasm_status tw_wasi_dispatch_init(tw_wasi_dispatch *d, size_t capacity,
    void (*wake)(void *), void *context) {
    if (!d || d->owner || !capacity || !wake ||
        capacity >= SIZE_MAX / sizeof(tw_dispatch_command *)) return TURBOWASM_INVALID_ARGUMENT;
    /* One whole unused pointer slot covers the ring's empty/full sentinel and
     * preserves alignment when fixed-size entries wrap. */
    size_t bytes = (capacity + 1) * sizeof(tw_dispatch_command *);
    uint8_t *storage = malloc(bytes);
    if (!storage) return TURBOWASM_OUT_OF_MEMORY;
    cmeta_mutex_t mutex = NULL; cmeta_cond_t changed = NULL;
    cmeta_mutex_init(&mutex);
    if (mutex) cmeta_cond_init(&changed);
    if (!mutex || !changed) {
        if (mutex) cmeta_mutex_destroy(&mutex);
        free(storage); return TURBOWASM_OUT_OF_MEMORY;
    }
    *d = (tw_wasi_dispatch){.mutex = mutex, .changed = changed,
        .owner = cmeta_thread_current_token(), .wake = wake, .wake_context = context,
        .storage = storage, .capacity = capacity};
    ring_init(&d->queue, storage, bytes);
    return TURBOWASM_OK;
}

bool tw_wasi_dispatch_idle_locked(const tw_wasi_dispatch *d) {
    return !d->outstanding && !d->cleanup_waiters && !d->direct_calls && !d->callers;
}

turbowasm_status tw_wasi_dispatch_destroy(tw_wasi_dispatch *d) {
    if (!tw_wasi_dispatch_is_owner(d)) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_mutex_lock(&d->mutex);
    bool idle = tw_wasi_dispatch_idle_locked(d) && !d->advancing;
    cmeta_mutex_unlock(&d->mutex);
    if (!idle) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_cond_destroy(&d->changed); cmeta_mutex_destroy(&d->mutex);
    free(d->storage); *d = (tw_wasi_dispatch){0};
    return TURBOWASM_OK;
}

uint32_t tw_wasi_dispatch_call(tw_wasi_dispatch *d, uint32_t (*run)(void *),
    void *frame, bool cleanup, bool must_admit) {
    if (!d) return run(frame);
    cmeta_mutex_lock(&d->mutex);
    if (d->stopping && !cleanup) {
        cmeta_mutex_unlock(&d->mutex); return TURBOWASM_WASI_ERRNO_INTR;
    }
    if (tw_wasi_dispatch_is_owner(d)) {
        if (d->direct_calls == SIZE_MAX) {
            cmeta_mutex_unlock(&d->mutex); return TURBOWASM_WASI_ERRNO_BUSY;
        }
        ++d->direct_calls;
        cmeta_mutex_unlock(&d->mutex);
        uint32_t error = run(frame);
        cmeta_mutex_lock(&d->mutex);
        --d->direct_calls;
        cmeta_cond_broadcast(&d->changed);
        cmeta_mutex_unlock(&d->mutex);
        return error;
    }
    if (must_admit) {
        /* Infallible cleanup borrows its caller's frame while waiting. There
         * is no overflow allocation or replay of an already accepted command. */
        ++d->cleanup_waiters;
        cmeta_cond_broadcast(&d->changed);
        while (d->outstanding == d->capacity)
            cmeta_cond_wait(&d->changed, &d->mutex);
        --d->cleanup_waiters;
    } else if (d->outstanding == d->capacity) {
        cmeta_mutex_unlock(&d->mutex); return TURBOWASM_WASI_ERRNO_BUSY;
    }
    tw_dispatch_command command = {.run = run, .frame = frame};
    tw_dispatch_command *pointer = &command;
    uint8_t *entry = ring_write_acquire(&d->queue, sizeof(pointer));
    /* outstanding includes the currently executing command, so available
     * queue space is guaranteed by admission and fixed-size ring geometry. */
    memcpy(entry, &pointer, sizeof(pointer));
    ring_write_release(&d->queue, sizeof(pointer));
    ++d->outstanding;
    ++d->callers;
    cmeta_mutex_unlock(&d->mutex);
    d->wake(d->wake_context);
    cmeta_mutex_lock(&d->mutex);
    while (!command.done) cmeta_cond_wait(&d->changed, &d->mutex);
    uint32_t error = command.error;
    --d->callers;
    cmeta_cond_broadcast(&d->changed);
    cmeta_mutex_unlock(&d->mutex);
    return error;
}

turbowasm_status tw_wasi_dispatch_advance(tw_wasi_dispatch *d) {
    if (!tw_wasi_dispatch_is_owner(d)) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_mutex_lock(&d->mutex);
    if (d->advancing || d->direct_calls) {
        cmeta_mutex_unlock(&d->mutex); return TURBOWASM_INVALID_ARGUMENT;
    }
    d->advancing = true;
    /* A fixed batch prevents hot producers from starving the owner's other
     * transport/timer work. All callback effects happen outside this mutex. */
    for (size_t i = 0; i < d->capacity; ++i) {
        size_t available = 0;
        uint8_t *entry = ring_read_acquire(&d->queue, &available);
        if (!available) break;
        tw_dispatch_command *command;
        memcpy(&command, entry, sizeof(command));
        ring_read_release(&d->queue, sizeof(command));
        cmeta_mutex_unlock(&d->mutex);
        uint32_t error = command->run(command->frame);
        cmeta_mutex_lock(&d->mutex);
        command->error = error;
        command->done = true;
        --d->outstanding;
        cmeta_cond_broadcast(&d->changed);
        /* No command/frame access after publishing done: its worker can
         * return as soon as the mutex is released. */
    }
    d->advancing = false;
    cmeta_mutex_unlock(&d->mutex);
    return TURBOWASM_OK;
}

turbowasm_status tw_wasi_dispatch_stop(tw_wasi_dispatch *d) {
    if (!tw_wasi_dispatch_is_owner(d)) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_mutex_lock(&d->mutex);
    if (d->advancing || d->direct_calls) {
        cmeta_mutex_unlock(&d->mutex); return TURBOWASM_INVALID_ARGUMENT;
    }
    d->stopping = true;
    cmeta_cond_broadcast(&d->changed);
    cmeta_mutex_unlock(&d->mutex);
    return TURBOWASM_OK;
}
