#include "flush.h"
#include "FILE.h"
#include <stdio.h>

int fsetpos(FILE stream[static 1], const fpos_t position[static 1])
{
    return __stdio_position(stream, position->_offset, SEEK_SET);
}
