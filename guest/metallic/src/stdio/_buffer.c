#include "FILE.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* Owned by this single-threaded guest libc. Nodes live in their FILE objects;
 * the guest's descriptor and linear-memory limits bound their lifetime/count. */
static FILE *buffered_;

void __stdio_buffer_register(FILE *stream)
{
    stream->flush_next = buffered_;
    buffered_ = stream;
}

void __stdio_buffer_release(FILE *stream)
{
    if (stream->buffer) {
        FILE **link = &buffered_;
        while (*link && *link != stream) link = &(*link)->flush_next;
        if (*link) *link = stream->flush_next;
        if (stream->buffer_owned) free(stream->buffer);
    }
    stream->buffer = NULL;
    stream->capacity = stream->read_pos = stream->read_end = stream->write_end = 0;
    stream->buffer_owned = 0;
    stream->flush_next = NULL;
}

int __stdio_flush(FILE *stream)
{
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
    for (FILE *f = buffered_; f; f = f->flush_next) {
        if ((!line_only || f->buffer_mode == _IOLBF) && __stdio_flush(f)) {
            if (!error) error = errno;
            result = EOF;
        }
    }
    if (error) errno = error;
    return result;
}

int __stdio_position(FILE *stream, off_t offset, int origin)
{
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
