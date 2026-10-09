#include "FILE.h"
#include "../wasi/wasi.h"
#include "../wasi/errno.h"
#include <stdio.h>
#include <string.h>

static size_t read_(FILE *stream, void *buffer, size_t size)
{
#ifndef __METALLIC_THREADS__
    if (!stream->buffer || stream->buffer_mode == _IOLBF) (void)__stdio_flush_all(1);
#endif
    __wasi_iovec_t iov = { buffer, size };
    size_t nread = 0;
    __wasi_errno_t e = __wasi_fd_read((__wasi_fd_t)stream->fd, &iov, 1, &nread);

    if (e || nread > size) {
        stream->state |= errbit_;
        errno = e ? wasi_to_posix[e] : EIO;
        return 0;
    }

    if (!nread) stream->state |= eofbit_;
    return nread;
}

size_t __stdio_read(FILE stream[restrict static 1], void* restrict buffer, size_t size)
{
    METALLIC_STDIO_GUARD(stream, 1);
    stream->io_started = 1;
    if (!size || __stdio_flush(stream) || (stream->state & eofbit_)) return 0;
    size_t total = 0;
    while (total < size) {
        if (!stream->buffer) {
            size_t got = read_(stream, (unsigned char*)buffer + total, size - total);
            if (!got) break;
            total += got;
        } else {
            if (stream->read_pos == stream->read_end) {
                stream->read_pos = 0;
                stream->read_end = read_(stream, stream->buffer, stream->capacity);
                if (!stream->read_end) break;
            }
            size_t take = stream->read_end - stream->read_pos;
            if (take > size - total) take = size - total;
            memcpy((unsigned char*)buffer + total, stream->buffer + stream->read_pos, take);
            stream->read_pos += take;
            total += take;
        }
    }
    return total;
}
