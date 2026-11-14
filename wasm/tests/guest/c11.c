#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchar.h>
#include <wchar.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "C11 guest line %d: %s\n", __LINE__, #x); _Exit(1); } } while (0)
static void a(void) { fputs("A", stdout); }
static void b(void) { fputs("B", stdout); }
static void c(void) { fputs("C", stdout); }
static void d(void) { fputs("D", stdout); }
static char **saved_args;
static void args_at_exit(void) { REQUIRE(!strcmp(saved_args[2], "argument")); fputs("args-live", stdout); }
static int compare(const void *a, const void *b) { return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b); }

int main(int argc, char **argv) {
    REQUIRE(argc >= 2 && argv[argc] == NULL);
    if (!strcmp(argv[1], "exit")) {
        REQUIRE(!atexit(a) && !atexit(b));
        return 23;
    }
    if (!strcmp(argv[1], "quick")) {
        REQUIRE(!atexit(a) && !at_quick_exit(c) && !at_quick_exit(d));
        quick_exit(24);
    }
    if (!strcmp(argv[1], "immediate")) {
        REQUIRE(!atexit(a) && !at_quick_exit(c));
        _Exit(25);
    }
    if (!strcmp(argv[1], "loop")) { for (;;) {} }
    if (!strcmp(argv[1], "args")) {
        REQUIRE(argc == 3 && !strcmp(argv[2], "argument"));
        REQUIRE(getenv("C11_TEST") && !strcmp(getenv("C11_TEST"), "value"));
        REQUIRE(getenv("PATH") == NULL);
        saved_args = argv;
        REQUIRE(!atexit(args_at_exit));
        return 0;
    }
    if (!strcmp(argv[1], "library")) {
        int numbers[] = {7, -1, 4};
        qsort(numbers, 3, sizeof(int), compare);
        REQUIRE(numbers[0] == -1 && numbers[2] == 7);
        int key = 4;
        REQUIRE(bsearch(&key, numbers, 3, sizeof(int), compare) == numbers + 1);
        char *end;
        REQUIRE(strtol("42!", &end, 10) == 42 && *end == '!');
        char text[64]; int integer; double real;
        REQUIRE(snprintf(text, sizeof(text), "%d %.2f", 42, 1.25) == 7);
        REQUIRE(sscanf(text, "%d %lf", &integer, &real) == 2);
        REQUIRE(integer == 42 && fabs(real - 1.25) < 1e-12);
        void *block = aligned_alloc(64, 128);
        REQUIRE(block && (uintptr_t)block % 64 == 0);
        memset(block, 0x42, 128); free(block);
        void *(*volatile allocate)(size_t, size_t) = aligned_alloc;
        errno = 0; REQUIRE(!allocate(3, 12) && errno == EINVAL);
        errno = 0; REQUIRE(!allocate(64, 65) && errno == EINVAL);
        struct timespec now = {0};
        REQUIRE(timespec_get(&now, TIME_UTC) == TIME_UTC);
        REQUIRE(now.tv_sec > 0 && now.tv_nsec >= 0 && now.tv_nsec < 1000000000L);
        REQUIRE(timespec_get(&now, -1) == 0);
        return 0;
    }
    if (!strcmp(argv[1], "unicode-null")) {
        mbstate_t state = {0};
        char32_t c32 = 0x1234;
        char16_t c16 = 0x5678;
        wchar_t wide = 0x9abc;
        REQUIRE(mbrtoc32(&c32, NULL, 0, &state) == 0 && c32 == 0x1234);
        REQUIRE(mbrtoc16(&c16, NULL, 0, &state) == 0 && c16 == 0x5678);
        REQUIRE(mbrtowc(&wide, NULL, 0, &state) == 0 && wide == 0x9abc);
        REQUIRE(c16rtomb(NULL, 0xd800, &state) == 1 && mbsinit(&state));
        REQUIRE(mbrtowc(&wide, "\xe2", 1, &state) == (size_t)-2);
        errno = 0;
        REQUIRE(mbrtowc(NULL, NULL, 0, &state) == (size_t)-1 && errno == EILSEQ);
        return 0;
    }
    if (!strcmp(argv[1], "unicode-surrogates")) {
        mbstate_t state = {0};
        char bytes[4] = {0};
        REQUIRE(c16rtomb(bytes, 0xd83d, &state) == 0 && !mbsinit(&state));
        REQUIRE(c16rtomb(bytes, 0xde00, &state) == 4 && mbsinit(&state));
        REQUIRE(!memcmp(bytes, "\xf0\x9f\x98\x80", 4));
        REQUIRE(c16rtomb(bytes, 0xd800, &state) == 0);
        errno = 0;
        REQUIRE(c16rtomb(bytes, 0xd801, &state) == (size_t)-1 && errno == EILSEQ);
        state = (mbstate_t){0};
        errno = 0;
        REQUIRE(c16rtomb(bytes, 0xdc00, &state) == (size_t)-1 && errno == EILSEQ);
        state = (mbstate_t){0};
        char16_t c16;
        REQUIRE(mbrtoc16(&c16, "\xf0\x9f\x98\x80", 4, &state) == 4 && c16 == 0xd83d);
        REQUIRE(mbrtoc16(&c16, "A", 1, &state) == (size_t)-3 && c16 == 0xde00);
        REQUIRE(mbrtoc16(&c16, "A", 1, &state) == 1 && c16 == 'A');
        REQUIRE(mbrtoc16(&c16, "\xf0\x9f\x98\x80", 4, &state) == 4);
        c16 = 0x1234;
        REQUIRE(mbrtoc16(&c16, NULL, 0, &state) == (size_t)-3 && c16 == 0x1234);
        REQUIRE(mbsinit(&state));
        return 0;
    }
    if (!strcmp(argv[1], "unicode-restart")) {
        /* Implicit state belongs to each conversion function, not a shared
         * mbrtowc implementation detail. */
        char32_t c32;
        wchar_t wide;
        REQUIRE(mbrtoc32(&c32, "\xe2", 1, NULL) == (size_t)-2);
        REQUIRE(mbrtowc(&wide, "A", 1, NULL) == 1 && wide == 'A');
        REQUIRE(mbrtoc32(&c32, "\x82\xac", 2, NULL) == 2 && c32 == 0x20ac);
        static const char invalid[][2] = {{'\xe0','\x80'}, {'\xed','\xa0'}, {'\xf0','\x80'}, {'\xf4','\x90'}};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            mbstate_t state = {0};
            REQUIRE(mbrtoc32(&c32, invalid[i], 1, &state) == (size_t)-2);
            errno = 0;
            REQUIRE(mbrtoc32(&c32, invalid[i] + 1, 1, &state) == (size_t)-1 && errno == EILSEQ);
        }
        static const char text[] = "A\xc2\xa2\xe2\x82\xac\xf0\x9f\x98\x80";
        mbstate_t state = {0};
        const char *source = text;
        wchar_t decoded[5];
        REQUIRE(mbsrtowcs(NULL, &source, 0, &state) == 4 && source == text);
        REQUIRE(mbsrtowcs(decoded, &source, 2, &state) == 2 && source == text + 3);
        REQUIRE(mbsrtowcs(decoded + 2, &source, 3, &state) == 2 && source == NULL);
        REQUIRE(decoded[2] == 0x20ac && decoded[3] == 0x1f600 && decoded[4] == 0);
        const wchar_t *input = decoded;
        char output[11] = {0};
        REQUIRE(wcsrtombs(output, &input, 2, &state) == 1 && input == decoded + 1);
        REQUIRE(wcsrtombs(output + 1, &input, 10, &state) == 9 && input == NULL);
        REQUIRE(!strcmp(output, text));
        static const char32_t scalars[] = {0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff};
        for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); ++i) {
            char encoded[4];
            state = (mbstate_t){0};
            size_t length = c32rtomb(encoded, scalars[i], &state);
            REQUIRE(length >= 1 && length <= 4);
            for (size_t split = 0; split < length; ++split) {
                state = (mbstate_t){0};
                c32 = 0x1234;
                REQUIRE(mbrtoc32(&c32, encoded, split, &state) == (size_t)-2 && c32 == 0x1234);
                REQUIRE(mbrtoc32(&c32, encoded + split, length - split, &state) == length - split);
                REQUIRE(c32 == scalars[i] && mbsinit(&state));
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "files")) {
        FILE *f = fopen("first", "w+x"); REQUIRE(f);
        REQUIRE(fwrite("abc", 1, 3, f) == 3);
        REQUIRE(fwrite("", 0, 1, f) == 0);
        REQUIRE(!fclose(f));
        errno = 0; REQUIRE(!fopen("first", "wx") && errno == EEXIST);
        REQUIRE(!rename("first", "second"));
        errno = 0; REQUIRE(!fopen("first", "r") && errno == ENOENT);
        f = fopen("second", "a+"); REQUIRE(f);
        REQUIRE(!fseek(f, 0, SEEK_SET));
        REQUIRE(fwrite("d", 1, 1, f) == 1);
        REQUIRE(freopen(NULL, "r+", f) == f);
        REQUIRE(!fseek(f, 0, SEEK_SET));
        REQUIRE(fwrite("A", 1, 1, f) == 1);
        REQUIRE(!fseek(f, 0, SEEK_SET));
        char content[8] = {0};
        REQUIRE(fread(content, 1, 8, f) == 4 && !strcmp(content, "Abcd"));
        REQUIRE(feof(f));
        REQUIRE(!fseek(f, 0, SEEK_SET) && !feof(f));
        REQUIRE(fread(content, 0, 1, f) == 0);
        REQUIRE(freopen(NULL, "a+", f) == f);
        REQUIRE(fwrite("e", 1, 1, f) == 1);
        errno = 0;
        REQUIRE(!freopen(NULL, "w+", f) && errno == ENOTSUP);
        f = fopen("second", "r"); REQUIRE(f);
        memset(content, 0, sizeof(content));
        REQUIRE(fread(content, 1, 8, f) == 5 && !strcmp(content, "Abcde"));
        REQUIRE(!fclose(f) && !remove("second"));
        f = tmpfile(); REQUIRE(f);
        REQUIRE(fputs("temporary", f) >= 0 && !fseek(f, 0, SEEK_SET));
        char temp[16] = {0};
        REQUIRE(fread(temp, 1, 9, f) == 9 && !strcmp(temp, "temporary"));
        REQUIRE(!fclose(f));
        return 0;
    }
    if (!strcmp(argv[1], "no-directory")) {
        errno = 0; REQUIRE(!fopen("first", "w") && errno != 0);
        REQUIRE(!tmpfile());
        return 0;
    }
    if (!strcmp(argv[1], "allocation")) {
        static void *blocks[2048];
        size_t count = 0;
        while (count < 2048 && (blocks[count] = malloc(1024))) ++count;
        REQUIRE(count < 2048 && errno == ENOMEM);
        static void *small[128]; size_t n = 0;
        while (n < 128 && (small[n] = malloc(1))) ++n;
        REQUIRE(n < 128);
        for (unsigned i = 0; i < 80; ++i) {
            errno = 0; REQUIRE(fopen("oom", "w") == NULL && errno == ENOMEM);
        }
        for (size_t i = 0; i < n; ++i) free(small[i]);
        for (size_t i = 0; i < count; ++i) free(blocks[i]);
        FILE *f = fopen("oom", "w"); REQUIRE(f && !fclose(f) && !remove("oom"));
        void *recovered = calloc(1024, 16); REQUIRE(recovered);
        REQUIRE(((unsigned char *)recovered)[123] == 0); free(recovered);
        return 0;
    }
    if (!strcmp(argv[1], "streams")) {
        char input[8] = {0};
        REQUIRE(fread(input, 1, 5, stdin) == 5 && !strcmp(input, "hello"));
        REQUIRE(fclose(stdin) == 0);
        REQUIRE(fputs(input, stdout) >= 0);
        REQUIRE(fclose(stdout) == 0);
        return 0;
    }
    return 99;
}
