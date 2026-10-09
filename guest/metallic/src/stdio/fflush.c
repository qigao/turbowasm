#include <stdio.h>
#include "FILE.h"

int fflush(FILE* stream)
{
    METALLIC_STDIO_GUARD(stream, 0);
    return stream ? __stdio_flush(stream) : __stdio_flush_all(0);
}
