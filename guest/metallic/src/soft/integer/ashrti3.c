#include <stdint.h>
#include "../../math/reinterpret.h"

__int128 __ashrti3(__int128 x, int shift)
{
    int64_t high = x >> 64;
    uint64_t low = x;

    if (shift & 64)
        return high >> (shift & 63);
    else if (shift) {
        /* Preserve arithmetic sign extension, then assemble unsigned bits. */
        unsigned __int128 result = (unsigned __int128)(uint64_t)(high >> shift) << 64 |
            ((uint64_t)high << (64 - shift) | low >> shift);
        return reinterpret(__int128, result);
    }
    else
        return x;
}
