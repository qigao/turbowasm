#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
