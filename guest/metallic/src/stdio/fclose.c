#include "flush.h"
#include "FILE.h"
#include <stdio.h>
#include <stdlib.h>

extern _Thread_local int errno;

int fclose(FILE stream[static 1])
{
    int flushed = flush_(stream);
    int flush_error = errno;

    int status = stream->close(stream);
    __stdio_buffer_release(stream);
    if (stream != stdin && stream != stdout && stream != stderr) free(stream);

    if (status >= 0) {
        if (flushed) errno = flush_error;
        return flushed ? EOF : 0;
    }

    errno = -status;
    return EOF;
}
