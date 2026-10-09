#include "FILE.h"

int ferror(FILE stream[static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    return stream->state & errbit_;
}
