#include "FILE.h"
#include "../wasi/wasi.h"
#include "../wasi/errno.h"
#include <stdio.h>
#include <string.h>

size_t __stdio_write_raw(FILE *stream, const void *buffer, size_t size)
{
    METALLIC_STDIO_GUARD(stream, 0);
    size_t total = 0;
    while (total < size) {
        __wasi_ciovec_t iov = { (const unsigned char*)buffer + total, size - total };
        size_t written = 0;
        __wasi_errno_t e = __wasi_fd_write((__wasi_fd_t)stream->fd, &iov, 1, &written);
        if (e || !written || written > size - total) {
            stream->state |= errbit_;
            errno = e ? wasi_to_posix[e] : EIO;
            break;
        }
        total += written;
    }
    return total;
}

size_t __stdio_write(FILE stream[restrict static 1], const void* restrict buffer, size_t size)
{
    METALLIC_STDIO_GUARD(stream, 0);
    stream->io_started = 1;
    if (!size) return 0;
    if (stream->read_end != stream->read_pos || stream->avail) {
        if (__stdio_position(stream, 0, SEEK_CUR)) { stream->state |= errbit_; return 0; }
    }
    stream->read_pos = stream->read_end = 0;
    if (!stream->buffer) return __stdio_write_raw(stream, buffer, size);
    size_t accepted = 0;
    while (accepted < size) {
        if (stream->write_end == stream->capacity && __stdio_flush(stream)) return accepted;
        size_t take = stream->capacity - stream->write_end;
        if (take > size - accepted) take = size - accepted;
        const unsigned char *input = (const unsigned char*)buffer + accepted;
        const unsigned char *newline = stream->buffer_mode == _IOLBF ? memchr(input, '\n', take) : NULL;
        if (newline) take = (size_t)(newline - input) + 1;
        size_t previous = stream->write_end;
        memcpy(stream->buffer + stream->write_end, input, take);
        stream->write_end += take;
        if ((newline || stream->write_end == stream->capacity) && __stdio_flush(stream)) {
            size_t delivered = previous + take - stream->write_end;
            return accepted + (delivered > previous ? delivered - previous : 0);
        }
        accepted += take;
    }
    return accepted;
}
