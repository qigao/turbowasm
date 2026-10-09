#include "FILE.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifdef __METALLIC_THREADS__
#include "../internal/thread_lock.h"
static metallic_lock stream_list_lock;
#endif

/* Nodes live in their FILE objects;
 * the guest's descriptor and linear-memory limits bound their lifetime/count. */
static FILE *buffered_;

void __stdio_buffer_register(FILE *stream)
{
#ifdef __METALLIC_THREADS__
    metallic_lock_acquire(&stream_list_lock);
#endif
    stream->flush_next = buffered_;
    buffered_ = stream;
#ifdef __METALLIC_THREADS__
    metallic_lock_release(&stream_list_lock);
#endif
}

void __stdio_buffer_release(FILE *stream)
{
    if (stream->buffer) {
#ifdef __METALLIC_THREADS__
        metallic_lock_acquire(&stream_list_lock);
#endif
        FILE **link = &buffered_;
        while (*link && *link != stream) link = &(*link)->flush_next;
        if (*link) *link = stream->flush_next;
#ifdef __METALLIC_THREADS__
        metallic_lock_release(&stream_list_lock);
#endif
        if (stream->buffer_owned) free(stream->buffer);
    }
    stream->buffer = NULL;
    stream->capacity = stream->read_pos = stream->read_end = stream->write_end = 0;
    stream->buffer_owned = 0;
    stream->flush_next = NULL;
}

int __stdio_flush(FILE *stream)
{
    METALLIC_STDIO_GUARD(stream, 0);
#ifdef __METALLIC_THREADS__
    if (stream->retired) return 0;
#endif
    if (!stream->write_end) return 0;
    size_t written = __stdio_write_raw(stream, stream->buffer, stream->write_end);
    stream->write_end -= written;
    if (stream->write_end) {
        memmove(stream->buffer, stream->buffer + written, stream->write_end);
        return EOF;
    }
    return 0;
}

int __stdio_flush_all(int line_only)
{
    int result = 0, error = 0;
#ifdef __METALLIC_THREADS__
    uintptr_t cursor = 0;
    for (;;) {
        metallic_lock_acquire(&stream_list_lock);
        FILE *chosen = NULL;
        for (FILE *f = buffered_; f; f = f->flush_next)
            if ((uintptr_t)f > cursor && (!chosen || (uintptr_t)f < (uintptr_t)chosen)) chosen = f;
        if (chosen) {
            if (atomic_load_explicit(&chosen->flush_refs, memory_order_relaxed) == UINT_MAX) __builtin_trap();
            atomic_fetch_add_explicit(&chosen->flush_refs, 1, memory_order_relaxed);
        }
        metallic_lock_release(&stream_list_lock);
        if (!chosen) break;
        cursor = (uintptr_t)chosen;
        metallic_stdio_guard guard = __stdio_operation_enter(chosen, 0);
        if (!chosen->retired && (!line_only || chosen->buffer_mode == _IOLBF) && __stdio_flush(chosen)) {
            if (!error) error = errno;
            result = EOF;
        }
        __stdio_operation_leave(&guard);
        /* Retirement waits for this list-locked decrement AND notification;
         * it cannot free the FILE between the zero publication and notify. */
        metallic_lock_acquire(&stream_list_lock);
        if (atomic_fetch_sub_explicit(&chosen->flush_refs, 1, memory_order_release) == 1)
            __builtin_wasm_memory_atomic_notify((int *)&chosen->flush_refs, UINT_MAX);
        metallic_lock_release(&stream_list_lock);
    }
#else
    for (FILE *f = buffered_; f; f = f->flush_next) {
        if ((!line_only || f->buffer_mode == _IOLBF) && __stdio_flush(f)) {
            if (!error) error = errno;
            result = EOF;
        }
    }
#endif
    if (error) errno = error;
    return result;
}

#ifdef __METALLIC_THREADS__
void __stdio_wait_flush_refs(FILE *stream) {
    for (;;) {
        metallic_lock_acquire(&stream_list_lock);
        unsigned refs = atomic_load_explicit(&stream->flush_refs, memory_order_acquire);
        metallic_lock_release(&stream_list_lock);
        if (!refs) return;
        __builtin_wasm_memory_atomic_wait32((int *)&stream->flush_refs, (int)refs, -1);
    }
}
#endif

int __stdio_position(FILE *stream, off_t offset, int origin)
{
    METALLIC_STDIO_GUARD(stream, 0);
    stream->io_started = 1;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    if (__stdio_flush(stream)) return -1;
    if (origin == SEEK_CUR) {
        off_t unread = (off_t)(stream->read_end - stream->read_pos) + stream->avail;
        if (offset < LLONG_MIN + unread) { errno = EOVERFLOW; return -1; }
        offset -= unread;
    }
    if (stream->seek(stream, offset, origin) == -1) return -1;
    stream->read_pos = stream->read_end = 0;
    stream->avail = 0;
    stream->state &= ~(eofbit_ | wpushbit_);
    return 0;
}
