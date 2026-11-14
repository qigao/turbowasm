/* Shared registry/budget state uses the dispatch mutex only in threaded mode.
 * Provider and clock callbacks, allocation, Runtime suspension and user wake
 * callbacks always run outside that mutex. */
static void tw_p1_lock(struct tw_p1_async *a) {
    if (a->dispatch) cmeta_mutex_lock(&a->dispatch->mutex);
}
static void tw_p1_unlock(struct tw_p1_async *a) {
    if (a->dispatch) cmeta_mutex_unlock(&a->dispatch->mutex);
}
static bool tw_p1_stopping_locked(struct tw_p1_async *a) {
    return a->dispatch ? a->dispatch->stopping : a->stopping;
}
static bool tw_p1_stopping(struct tw_p1_async *a) {
    tw_p1_lock(a); bool stop = tw_p1_stopping_locked(a); tw_p1_unlock(a); return stop;
}
static turbowasm_status tw_p1_interrupt(struct tw_p1_async *a, turbowasm_host_call *call) {
    return a->dispatch ? turbowasm_host_call_check_interrupt(call) : TURBOWASM_OK;
}
static turbowasm_status tw_p1_progress_begin(struct tw_p1_async *a) {
    if (!a->dispatch) return TURBOWASM_OK;
    if (!tw_wasi_dispatch_is_owner(a->dispatch)) return TURBOWASM_INVALID_ARGUMENT;
    tw_p1_lock(a);
    bool busy = a->progressing || a->dispatch->advancing || a->dispatch->direct_calls;
    if (!busy) a->progressing = true;
    tw_p1_unlock(a);
    return busy ? TURBOWASM_INVALID_ARGUMENT : TURBOWASM_OK;
}
static void tw_p1_progress_end(struct tw_p1_async *a) {
    if (a->dispatch) { tw_p1_lock(a); a->progressing = false; tw_p1_unlock(a); }
}

static turbowasm_status tw_p1_park(turbowasm_wasi_preview1_impl *p, turbowasm_host_call *call,
    tw_p1_subscription *subs, size_t count, uint32_t *error) {
    struct tw_p1_async *a = p->async;
    tw_wasi_dispatch *d = a->dispatch;
    bool worker = d && !tw_wasi_dispatch_is_owner(d);
    tw_p1_lock(a);
    if (tw_p1_stopping_locked(a)) {
        tw_p1_unlock(a); *error = TURBOWASM_WASI_ERRNO_INTR; return TURBOWASM_OK;
    }
    if (!worker && !turbowasm_host_call_can_wait(call)) {
        tw_p1_unlock(a); *error = TURBOWASM_WASI_ERRNO_AGAIN; return TURBOWASM_OK;
    }
    tw_p1_wait *w = NULL;
    for (size_t i = 0; i < a->capacity; ++i) if (!a->waits[i].active) { w = &a->waits[i]; break; }
    if (!w) { tw_p1_unlock(a); *error = TURBOWASM_WASI_ERRNO_AGAIN; return TURBOWASM_OK; }
    *w = (tw_p1_wait){.active = true, .worker = worker, .call = call, .subscriptions = subs, .count = count};
    ++a->active;
    tw_p1_unlock(a);
    turbowasm_status status = TURBOWASM_OK;
    int completion = 0;
    if (worker) {
        d->wake(d->wake_context);
        tw_p1_lock(a);
        while (!w->completed) {
            cmeta_cond_timedwait(&d->changed, &d->mutex, a->interrupt_interval_ns);
            /* Query the invocation only on its worker and outside adapter
             * locks. Cancellation never withdraws a frame the owner can read. */
            tw_p1_unlock(a);
            if (status == TURBOWASM_OK) status = turbowasm_host_call_check_interrupt(call);
            tw_p1_lock(a);
            if (status != TURBOWASM_OK && !w->cancelled) {
                w->cancelled = true;
                tw_p1_unlock(a); d->wake(d->wake_context); tw_p1_lock(a);
            }
        }
        completion = (int)w->completion;
        tw_p1_unlock(a);
        if (status == TURBOWASM_OK) status = turbowasm_host_call_check_interrupt(call);
    } else {
        /* The owner cannot advance the registry until Runtime has initialized
         * the wait handle and suspended this same callback frame. */
        status = turbowasm_host_call_wait(call, (uintptr_t)w, &w->handle, &completion);
    }
    tw_p1_lock(a);
    *w = (tw_p1_wait){0}; --a->active;
    tw_p1_unlock(a);
    if (status == TURBOWASM_OK) *error = (uint32_t)completion;
    return status;
}

turbowasm_status turbowasm_wasi_preview1_advance(turbowasm_wasi_preview1 *wasi) {
    if (!wasi || !wasi->impl) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_preview1_impl *p = wasi->impl;
    struct tw_p1_async *a = p->async;
    if (!a) return TURBOWASM_UNSUPPORTED;
    turbowasm_status status = tw_p1_progress_begin(a);
    if (status != TURBOWASM_OK) return status;
    if (a->dispatch) status = tw_wasi_dispatch_advance(a->dispatch);
    for (size_t i = 0; i < a->capacity && status == TURBOWASM_OK; ++i) {
        tw_p1_lock(a);
        tw_p1_wait *w = &a->waits[i];
        if (!w->active || w->completed) { tw_p1_unlock(a); continue; }
        w->inspecting = true;
        bool stopped = tw_p1_stopping_locked(a), ready = stopped || w->cancelled;
        tw_p1_unlock(a);
        if (!ready) for (size_t j = 0; j < w->count; ++j)
            if (tw_p1_subscription_ready(p, &w->subscriptions[j])) ready = true;
        tw_p1_lock(a);
        stopped = tw_p1_stopping_locked(a);
        ready |= stopped || w->cancelled;
        uint32_t completion = stopped || w->cancelled ? TURBOWASM_WASI_ERRNO_INTR : 0;
        if (ready && !w->worker) {
            tw_p1_unlock(a);
            status = turbowasm_host_call_complete_wait(w->call, w->handle, (int)completion);
            tw_p1_lock(a);
        }
        w->inspecting = false;
        if (ready && status == TURBOWASM_OK) { w->completion = completion; w->completed = true; }
        if (a->dispatch) cmeta_cond_broadcast(&a->dispatch->changed);
        /* Publication releases the owner's last access to the frame. Workers
         * only retire completed records, including cancelled registrations. */
        tw_p1_unlock(a);
    }
    tw_p1_progress_end(a);
    return status;
}

turbowasm_status turbowasm_wasi_preview1_next_timeout(turbowasm_wasi_preview1 *wasi, uint64_t *out) {
    if (!wasi || !wasi->impl || !out) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_preview1_impl *p = wasi->impl;
    struct tw_p1_async *a = p->async;
    if (!a) return TURBOWASM_UNSUPPORTED;
    turbowasm_status status = tw_p1_progress_begin(a);
    if (status != TURBOWASM_OK) return status;
    *out = UINT64_MAX;
    tw_p1_lock(a);
    if (a->dispatch && (a->dispatch->outstanding || a->dispatch->callers || a->dispatch->cleanup_waiters)) *out = 0;
    tw_p1_unlock(a);
    for (size_t i = 0; i < a->capacity; ++i) {
        tw_p1_lock(a);
        tw_p1_wait *w = &a->waits[i];
        if (!w->active) { tw_p1_unlock(a); continue; }
        if (w->completed || w->cancelled || tw_p1_stopping_locked(a)) {
            *out = 0; tw_p1_unlock(a); continue;
        }
        w->inspecting = true;
        tw_p1_unlock(a);
        for (size_t j = 0; j < w->count; ++j) {
            tw_p1_subscription *s = &w->subscriptions[j];
            if (s->type != TURBOWASM_WASI_EVENT_CLOCK || s->error) continue;
            uint64_t now = 0;
            uint32_t e = p->clock_time(p->clock_context, s->clock, s->precision, &now);
            if (e) { s->error = e; *out = 0; continue; }
            uint64_t delay = now >= s->deadline ? 0 : s->deadline - now;
            if (delay < *out) *out = delay;
        }
        tw_p1_lock(a); w->inspecting = false;
        if (w->cancelled) *out = 0;
        tw_p1_unlock(a);
    }
    tw_p1_progress_end(a);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi_preview1_shutdown_request(turbowasm_wasi_preview1 *wasi) {
    if (!wasi || !wasi->impl) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_preview1_impl *p = wasi->impl;
    struct tw_p1_async *a = p->async;
    if (!a) return TURBOWASM_UNSUPPORTED;
    turbowasm_status status = tw_p1_progress_begin(a);
    if (status != TURBOWASM_OK) return status;
    if (a->dispatch) status = tw_wasi_dispatch_stop(a->dispatch);
    else a->stopping = true;
    tw_p1_progress_end(a);
    return status == TURBOWASM_OK ? turbowasm_wasi_preview1_advance(wasi) : status;
}

turbowasm_status turbowasm_wasi_preview1_shutdown_poll(turbowasm_wasi_preview1 *wasi, bool *out) {
    if (!wasi || !wasi->impl || !out) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_preview1_impl *p = wasi->impl;
    struct tw_p1_async *a = p->async;
    if (!a) return TURBOWASM_UNSUPPORTED;
    turbowasm_status status = tw_p1_progress_begin(a);
    if (status != TURBOWASM_OK) return status;
    tw_p1_lock(a);
    *out = tw_p1_stopping_locked(a) && !a->active && !a->calls && !a->used &&
        (!a->dispatch || tw_wasi_dispatch_idle_locked(a->dispatch));
    tw_p1_unlock(a);
    tw_p1_progress_end(a);
    return TURBOWASM_OK;
}

static void *tw_p1_allocate(turbowasm_wasi_preview1_impl *p, size_t bytes) {
    struct tw_p1_async *a = p->async;
    tw_p1_lock(a);
    if (bytes > a->budget - a->used) { tw_p1_unlock(a); return NULL; }
    a->used += bytes;
    tw_p1_unlock(a);
    void *out = calloc(1, bytes ? bytes : 1);
    if (!out) { tw_p1_lock(a); a->used -= bytes; tw_p1_unlock(a); }
    return out;
}
static void tw_p1_free(turbowasm_wasi_preview1_impl *p, void *ptr, size_t bytes) {
    if (ptr) {
        free(ptr); tw_p1_lock(p->async); p->async->used -= bytes; tw_p1_unlock(p->async);
    }
}
