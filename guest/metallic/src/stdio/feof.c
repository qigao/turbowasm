#include "FILE.h"

int feof(FILE stream[static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    return stream->state & eofbit_;
}
