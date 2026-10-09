#include "flush.h"
#include "FILE.h"

int fseek(FILE stream[static 1], long offset, int origin)
{
    if (origin == SEEK_CUR)
        offset -= stream->avail;

    stream->avail = 0;

    if (flush_(stream) || stream->seek(stream, offset, origin) == -1) return -1;
    stream->state &= ~(eofbit_ | wpushbit_);
    return 0;
}
