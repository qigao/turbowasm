#include <stdint.h>
#include "../../math/reinterpret.h"

__int128 __ashlti3(__int128 x, int shift)
{
    uint64_t high = (unsigned __int128)x >> 64;
    uint64_t low = x;

    /* Shifts operate on the bit representation; signed left shifts of a
     * negative value (or into its sign bit) are undefined in C. */
    unsigned __int128 result;

    if (shift & 64)
        result = (unsigned __int128)(low << (shift & 63)) << 64;
    else if (shift)
        result = (unsigned __int128)(high << shift | low >> (64 - shift)) << 64 | low << shift;
    else
        return x;
    return reinterpret(__int128, result);
}
