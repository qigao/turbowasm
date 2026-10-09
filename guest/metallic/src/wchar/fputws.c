#include "../stdio/FILE.h"
#include <wchar.h>
#include <stdio.h>

int fputws(const wchar_t* restrict s, FILE* restrict stream)
{
    METALLIC_STDIO_GUARD(stream, 0);
    for (; *s; ++s)
        if (fputwc(*s, stream) == WEOF)
            return -1;
    return 0;
}
