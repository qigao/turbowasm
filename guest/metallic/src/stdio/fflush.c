#include <stdio.h>
#include "FILE.h"

int fflush(FILE* stream)
{
    return stream ? __stdio_flush(stream) : __stdio_flush_all(0);
}
