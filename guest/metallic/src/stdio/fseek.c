#include "flush.h"
#include "FILE.h"

int fseek(FILE stream[static 1], long offset, int origin)
{
    return __stdio_position(stream, offset, origin);
}
