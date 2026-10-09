#include "FILE.h"
#include <stdio.h>

void rewind(FILE stream[static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    fseek(stream, 0, SEEK_SET);
    stream->state &= ~errbit_;
}
