#include "FILE.h"

size_t fwrite(const void* restrict buffer, size_t size, size_t count, FILE stream[restrict static 1])
{
    if (!size || !count) return 0;
    return stream->write(stream, buffer, size * count) / size;
}
