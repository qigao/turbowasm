#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "numeric guest line %d: %s\n", __LINE__, #x); return 1; } } while (0)
typedef unsigned __int128 u128;
typedef __int128 i128;
typedef union { u128 u; i128 i; uint64_t words[2]; long double f; } bits128;
_Static_assert(sizeof(long double) == 16, "binary128 guest ABI required");

extern i128 __ashlti3(i128, int);
extern i128 __ashrti3(i128, int);
extern u128 __lshrti3(u128, int);
extern int32_t __fixtfsi(long double);
extern uint32_t __fixunstfsi(long double);
extern int64_t __fixtfdi(long double);
extern uint64_t __fixunstfdi(long double);
extern i128 __fixtfti(long double);
extern u128 __fixunstfti(long double);
extern long double __divtf3(long double, long double);

static int shifts(void) {
    static const uint64_t patterns[][2] = {
        {0, 0}, {1, 0}, {0, 1}, {UINT64_MAX, UINT64_MAX},
        {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)},
        {0, UINT64_C(0x8000000000000000)}
    };
    i128 (*volatile left)(i128, int) = __ashlti3;
    i128 (*volatile arithmetic)(i128, int) = __ashrti3;
    u128 (*volatile logical)(u128, int) = __lshrti3;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); ++p) {
        bits128 value = {.words = {patterns[p][0], patterns[p][1]}};
        for (int n = 0; n < 128; ++n) {
            for (int kind = 0; kind < 3; ++kind) {
                uint64_t expected[2] = {0, 0};
                /* Independent bit-by-bit oracle uses only 64-bit operations. */
                for (int bit = 0; bit < 128; ++bit) {
                    int source = kind == 0 ? bit - n : bit + n;
                    unsigned set = source >= 0 && source < 128
                        ? (unsigned)((value.words[source / 64] >> (source % 64)) & 1)
                        : (kind == 2 && source >= 128 ? (unsigned)(value.words[1] >> 63) : 0);
                    expected[bit / 64] |= (uint64_t)set << (bit % 64);
                }
                bits128 result;
                if (kind == 0) result.i = left(value.i, n);
                else if (kind == 1) result.u = logical(value.u, n);
                else result.i = arithmetic(value.i, n);
                REQUIRE(result.words[0] == expected[0] && result.words[1] == expected[1]);
            }
        }
    }
    return 0;
}

static int conversions(void) {
    int32_t (*volatile s32)(long double) = __fixtfsi;
    uint32_t (*volatile u32)(long double) = __fixunstfsi;
    int64_t (*volatile s64)(long double) = __fixtfdi;
    uint64_t (*volatile u64)(long double) = __fixunstfdi;
    i128 (*volatile s128)(long double) = __fixtfti;
    u128 (*volatile u128fn)(long double) = __fixunstfti;
    /* Values immediately below integer boundaries must truncate without an
     * intermediate double rounding up to the next integer. */
    REQUIRE(s32(0x1.ffffffffffffffffffffffffffffp-1L) == 0);
    REQUIRE(s32(0x1.fffffffbffffffffffffffffffffp30L) == INT32_MAX - 1);
    REQUIRE(s32(-0x1.fffffffbffffffffffffffffffffp30L) == -INT32_MAX + 1);
    REQUIRE(s32(-0x1p31L) == INT32_MIN);
    REQUIRE(u32(0x1.fffffffdffffffffffffffffffffp31L) == UINT32_MAX - 1);
    REQUIRE(u32(0x1.ffffffffffffffffffffffffffffp31L) == UINT32_MAX);
    REQUIRE(s64(0x1.fffffffffffffffbffffffffffffp62L) == INT64_MAX - 1);
    REQUIRE(s64(-0x1p63L) == INT64_MIN);
    REQUIRE(u64(0x1.fffffffffffffffdffffffffffffp63L) == UINT64_MAX - 1);
    REQUIRE(u64(0x1.ffffffffffffffffffffffffffffp63L) == UINT64_MAX);
    REQUIRE(u64(0x1p48L) == UINT64_C(0x1000000000000));
    bits128 result = {.u = u128fn(0x1.23456789abcdef0123456789abcdp112L)};
    REQUIRE(result.words[1] == UINT64_C(0x123456789abcd));
    REQUIRE(result.words[0] == UINT64_C(0xef0123456789abcd));
    result.u = u128fn(0x1.ffffffffffffffffffffffffffffp127L);
    REQUIRE(result.words[1] == UINT64_MAX && result.words[0] == UINT64_C(0xffffffffffff8000));
    result.i = s128(-0x1p127L);
    REQUIRE(result.words[1] == UINT64_C(0x8000000000000000) && result.words[0] == 0);
    result.i = s128(-0x1.8p64L);
    REQUIRE(result.words[1] == UINT64_MAX - 1 && result.words[0] == UINT64_C(0x8000000000000000));
    return 0;
}

static int division(void) {
    long double (*volatile divide)(long double, long double) = __divtf3;
    /* Fixed binary128 representations, not x/y expressions lowered to the
     * very builtin under test. Includes ties at the subnormal boundary. */
    static const struct { long double a, b; uint64_t high, low; } cases[] = {
        {1.0L, 1.0L, UINT64_C(0x3fff000000000000), 0},
        {1.0L, 3.0L, UINT64_C(0x3ffd555555555555), UINT64_C(0x5555555555555555)},
        {2.0L, 3.0L, UINT64_C(0x3ffe555555555555), UINT64_C(0x5555555555555555)},
        {-3.0L, 2.0L, UINT64_C(0xbfff800000000000), 0},
        {0x1p-16494L, 2.0L, 0, 0},
        {0x3p-16494L, 2.0L, 0, 2},
        {0x5p-16494L, 2.0L, 0, 2},
        {-0x1p-16494L, 2.0L, UINT64_C(0x8000000000000000), 0},
        {0x1p-16382L, 2.0L, UINT64_C(0x0000800000000000), 0},
        {0x1.ffffffffffffffffffffffffffffp16383L, 0.5L, UINT64_C(0x7fff000000000000), 0},
        {0.0L, -1.0L, UINT64_C(0x8000000000000000), 0},
        {1.0L, 0.0L, UINT64_C(0x7fff000000000000), 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bits128 result = {.f = divide(cases[i].a, cases[i].b)};
        if (result.words[1] != cases[i].high || result.words[0] != cases[i].low)
            fprintf(stderr, "division case %u: %016llx %016llx\n", (unsigned)i,
                (unsigned long long)result.words[1], (unsigned long long)result.words[0]);
        REQUIRE(result.words[1] == cases[i].high && result.words[0] == cases[i].low);
    }
    bits128 nan = {.f = divide(0.0L, 0.0L)};
    REQUIRE((nan.words[1] & UINT64_C(0x7fff000000000000)) == UINT64_C(0x7fff000000000000));
    REQUIRE((nan.words[1] & UINT64_C(0x0000ffffffffffff)) || nan.words[0]);
    return 0;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    if (!strcmp(argv[1], "shifts")) return shifts();
    if (!strcmp(argv[1], "conversions")) return conversions();
    if (!strcmp(argv[1], "division")) return division();
    return 99;
}
