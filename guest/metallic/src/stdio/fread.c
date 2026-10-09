#include "FILE.h"
#include <string.h>
#include <stdint.h>
#include <errno.h>

static size_t wrapper_(unsigned char* restrict buffer, size_t size, FILE stream[restrict static 1])
{
    size_t hit = stream->avail < size ? stream->avail : size;
    size_t missed = size - hit;

    memcpy(buffer, stream->cache + sizeof(stream->cache) - stream->avail, hit);
    stream->avail -= hit;
    return hit + (missed ? stream->read(stream, buffer + hit, missed) : 0);
}

size_t fread(void* restrict buffer, size_t size, size_t count, FILE stream[restrict static 1])
{
    METALLIC_STDIO_GUARD(stream, 1);
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { stream->state |= errbit_; errno = EOVERFLOW; return 0; }
    return wrapper_(buffer, size * count, stream) / size;
}
