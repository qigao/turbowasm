#include "FILE.h"
#include <string.h>

int fputs(const char s[restrict static 1], FILE stream[restrict static 1])
{
    METALLIC_STDIO_GUARD(stream, 0);
    size_t size = strlen(s);

    return (stream->write(stream, s, size) == size) - 1;
}
