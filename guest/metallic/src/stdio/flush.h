#include "FILE.h"
#include <stdio.h>

static int flush_(FILE stream[static 1])
{
    return __stdio_flush(stream);
}
