#include <wchar.h>
#include <errno.h>
#include <stdint.h>

/* State encoding in the 32-bit __code slot:
 *   0 -> initial state (no partial sequence pending)
 *
 *   Otherwise, mid-sequence:
 *     bits 28..31: total expected bytes (2, 3, or 4)
 *     bits 24..27: bytes already absorbed (1..3, always less than total)
 *     bits  0..23: accumulated codepoint so far
 *
 * A complete UTF-8 codepoint fits in 21 bits (<= U+10FFFF), so 24 bits of
 * accumulator is comfortable.  We never store the "initial" lead-only state
 * with zero accumulated bits, only multi-byte sequences past the lead. */

static size_t fail_(void)
{
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbrtowc(wchar_t* restrict pwc, const char* restrict s, size_t n, mbstate_t* restrict ps)
{
    static mbstate_t internal;
    if (!ps)
        ps = &internal;

    if (!s) {
        /* C11 specifies an empty-string conversion with no output. A pending
         * UTF-8 sequence followed by NUL is an encoding error, not a reset. */
        pwc = NULL;
        s = "";
        n = 1;
    }

    if (!n)
        return (size_t)-2;

    const unsigned char* p = (const unsigned char*)s;
    uint_least32_t saved = ps->__code;
    unsigned total = saved >> 28;
    unsigned absorbed = (saved >> 24) & 0xF;
    uint_least32_t code = saved & 0xFFFFFF;
    size_t consumed = 0;

    if (!total) {
        unsigned char lead = p[consumed++];

        if (lead < 0x80) {
            if (pwc)
                *pwc = lead;
            ps->__code = 0;
            return lead ? 1 : 0;
        }

        if (lead < 0xC2 || lead > 0xF4)
            return fail_();

        if (lead < 0xE0) {
            code = lead & 0x1F;
            total = 2;
        }
        else if (lead < 0xF0) {
            code = lead & 0x0F;
            total = 3;
        }
        else {
            code = lead & 0x07;
            total = 4;
        }
        absorbed = 1;
    }

    while (absorbed < total) {
        if (consumed == n) {
            ps->__code = ((uint_least32_t)total << 28)
                       | ((uint_least32_t)absorbed << 24)
                       | (code & 0xFFFFFF);
            return (size_t)-2;
        }

        unsigned char byte = p[consumed++];

        if ((byte & 0xC0) != 0x80)
            return fail_();

        /* Reject prefixes that cannot become a Unicode scalar even when the
         * remaining bytes arrive in another call. */
        if (absorbed == 1 &&
            ((total == 3 && ((code == 0 && byte < 0xA0) || (code == 13 && byte >= 0xA0))) ||
             (total == 4 && ((code == 0 && byte < 0x90) || (code == 4 && byte >= 0x90)))))
            return fail_();

        code = (code << 6) | (byte & 0x3F);
        ++absorbed;
    }

    /* Overlong / range / surrogate checks based on total length */
    int bad = 0;
    if (total == 2)
        bad = code < 0x80;
    else if (total == 3)
        bad = code < 0x800 || (code >= 0xD800 && code <= 0xDFFF);
    else /* total == 4 */
        bad = code < 0x10000 || code > 0x10FFFF;

    if (bad)
        return fail_();

    if (pwc)
        *pwc = (wchar_t)code;

    ps->__code = 0;
    return code ? consumed : 0;
}
