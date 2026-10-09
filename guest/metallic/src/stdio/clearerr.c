#include "FILE.h"

void clearerr(FILE stream[static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    stream->state = 0;
}
