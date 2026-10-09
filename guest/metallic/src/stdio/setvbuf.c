#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "FILE.h"

int setvbuf(FILE* restrict stream, char* restrict buf, int mode, size_t size)
{
    if ((mode != _IONBF && mode != _IOLBF && mode != _IOFBF) || stream->io_started) {
        errno = EINVAL;
        return -1;
    }
    unsigned char *storage = NULL;
    if (mode != _IONBF) {
        if (!size && !buf) size = BUFSIZ;
        if (!size || size > PTRDIFF_MAX) { errno = EINVAL; return -1; }
        storage = buf ? (unsigned char*)buf : malloc(size);
        if (!storage) { errno = ENOMEM; return -1; }
    }
    /* Acquire first: failed admission cannot alter the previous buffer. */
    __stdio_buffer_release(stream);
    stream->buffer = storage;
    stream->capacity = storage ? size : 0;
    stream->buffer_owned = storage && !buf;
    stream->buffer_mode = mode;
    if (storage) __stdio_buffer_register(stream);
    return 0;
}
