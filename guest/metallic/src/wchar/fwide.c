#include "../stdio/FILE.h"
#include <wchar.h>

int fwide(FILE* stream, int mode)
{
    METALLIC_STDIO_GUARD(stream, 0);
    if (stream->orient == 0 && mode != 0)
        stream->orient = mode > 0 ? 1 : -1;
    return stream->orient;
}
