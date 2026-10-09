#include "FILE.h"
#include <stdio.h>
#include <errno.h>
#include <limits.h>

off_t __ftello(FILE stream[static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    off_t position = stream->seek(stream, 0,
        (stream->state & appbit_) && stream->write_end ? SEEK_END : SEEK_CUR);
    if (position == -1) return -1;
    off_t unread = (off_t)(stream->read_end - stream->read_pos) + stream->avail;
    if (position < unread || position - unread > LLONG_MAX - (off_t)stream->write_end) {
        errno = EOVERFLOW;
        return -1;
    }
    return position - unread + (off_t)stream->write_end;
}

long ftell(FILE stream[static 1])
{
    off_t position = __ftello(stream);
    if (position > LONG_MAX) { errno = EOVERFLOW; return -1; }
    return (long)position;
}
