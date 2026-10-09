#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "buffer guest line %d: %s\n", __LINE__, #x); _Exit(1); } } while (0)
static FILE *exit_file;
static void finish(void) { REQUIRE(fputs("!", exit_file) >= 0); REQUIRE(fputs("exit", stdout) >= 0); }

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    if (!strcmp(argv[1], "full")) {
        char buffer[8];
        FILE *out = fopen("buffer", "w"); REQUIRE(out);
        REQUIRE(!setvbuf(out, buffer, _IOFBF, sizeof(buffer)));
        REQUIRE(fwrite("abc", 1, 3, out) == 3 && ftell(out) == 3);
        FILE *in = fopen("buffer", "r"); REQUIRE(in);
        REQUIRE(fgetc(in) == EOF);
        REQUIRE(!fflush(out) && !fseek(in, 0, SEEK_SET));
        char text[16] = {0}; REQUIRE(fread(text, 1, 3, in) == 3 && !strcmp(text, "abc"));
        REQUIRE(fwrite("defghijk", 1, 8, out) == 8);
        REQUIRE(!fseek(in, 0, SEEK_END) && ftell(in) == 11);
        REQUIRE(fputs("tail", out) >= 0 && !fclose(out));
        REQUIRE(!fseek(in, 0, SEEK_END) && ftell(in) == 15);
        REQUIRE(!fclose(in));
        char setbuf_storage[BUFSIZ];
        out = fopen("buffer2", "w"); REQUIRE(out);
        setbuf(out, setbuf_storage);
        REQUIRE(fputs("setbuf", out) >= 0 && !fclose(out));
        in = fopen("buffer2", "r"); REQUIRE(in);
        REQUIRE(!setvbuf(in, NULL, _IONBF, 0));
        REQUIRE(fgetc(in) == 's' && !fclose(in));
        return 0;
    }
    if (!strcmp(argv[1], "line")) {
        FILE *out = fopen("buffer", "w"); REQUIRE(out);
        REQUIRE(!setvbuf(out, NULL, _IOLBF, 8));
        REQUIRE(fputs("ab\ncd", out) >= 0);
        FILE *in = fopen("buffer", "r"); REQUIRE(in);
        REQUIRE(!setvbuf(in, NULL, _IOFBF, 8));
        char text[8] = {0};
        REQUIRE(fread(text, 1, sizeof(text), in) == 3 && !strcmp(text, "ab\n"));
        REQUIRE(!fflush(NULL));
        REQUIRE(!fseek(in, 0, SEEK_SET));
        memset(text, 0, sizeof(text));
        REQUIRE(fread(text, 1, sizeof(text), in) == 5 && !strcmp(text, "ab\ncd"));
        REQUIRE(!fclose(out) && !fclose(in));
        return 0;
    }
    if (!strcmp(argv[1], "input")) {
        FILE *out = fopen("buffer", "w"); REQUIRE(out);
        REQUIRE(fputs("abcdefghijkl", out) >= 0 && !fclose(out));
        FILE *in = fopen("buffer", "r"); REQUIRE(in);
        REQUIRE(!setvbuf(in, NULL, _IOFBF, 8));
        REQUIRE(fgetc(in) == 'a' && ftell(in) == 1);
        REQUIRE(ungetc('a', in) == 'a' && ftell(in) == 0);
        fpos_t start; REQUIRE(!fgetpos(in, &start));
        REQUIRE(fseek(in, 0, 99) != 0);
        REQUIRE(fgetc(in) == 'a' && fgetc(in) == 'b');
        REQUIRE(!fsetpos(in, &start) && fgetc(in) == 'a');
        REQUIRE(!fseek(in, 0, SEEK_CUR) && fgetc(in) == 'b');
        char text[16] = {0};
        REQUIRE(fread(text, 1, sizeof(text), in) == 10 && !strcmp(text, "cdefghijkl"));
        REQUIRE(feof(in) && ftell(in) == 12 && feof(in));
        REQUIRE(!fsetpos(in, &start) && !feof(in));
        REQUIRE(!fclose(in));
        return 0;
    }
    if (!strcmp(argv[1], "update")) {
        FILE *f = fopen("buffer", "w+"); REQUIRE(f);
        REQUIRE(!setvbuf(f, NULL, _IOFBF, 8));
        REQUIRE(fputs("abcdef", f) >= 0 && !fseek(f, 0, SEEK_SET));
        REQUIRE(fgetc(f) == 'a' && !fseek(f, 0, SEEK_CUR));
        REQUIRE(fputc('Z', f) == 'Z' && ftell(f) == 2);
        REQUIRE(!fflush(f) && !fseek(f, 0, SEEK_SET));
        REQUIRE(fgetc(f) == 'a');
        REQUIRE(freopen(NULL, "r+", f) == f);
        REQUIRE(!setvbuf(f, NULL, _IOFBF, 8));
        REQUIRE(ftell(f) == 1);
        REQUIRE(fgetc(f) == 'Z');
        REQUIRE(freopen("buffer2", "w", f) == f);
        REQUIRE(!setvbuf(f, NULL, _IOFBF, 8));
        REQUIRE(fputs("second", f) >= 0 && !fclose(f));
        f = fopen("buffer2", "r"); REQUIRE(f);
        char text[8] = {0};
        REQUIRE(fread(text, 1, 8, f) == 6 && !strcmp(text, "second") && !fclose(f));
        return 0;
    }
    if (!strcmp(argv[1], "errors")) {
        FILE *f = fopen("buffer", "w"); REQUIRE(f);
        errno = 0; REQUIRE(setvbuf(f, NULL, 99, 8) != 0 && errno == EINVAL);
        errno = 0; REQUIRE(setvbuf(f, NULL, _IOFBF, 32u * 1024u * 1024u) != 0 && errno == ENOMEM);
        REQUIRE(fputs("x", f) >= 0 && !fclose(f));
        FILE *other = fopen("buffer2", "w"); REQUIRE(other);
        REQUIRE(!setvbuf(other, NULL, _IOFBF, 8) && fputs("good", other) >= 0);
        f = fopen("buffer", "r"); REQUIRE(f);
        REQUIRE(!setvbuf(f, NULL, _IOFBF, 8));
        REQUIRE(fputs("pending", f) >= 0);
        REQUIRE(fflush(NULL) == EOF && ferror(f));
        FILE *verify = fopen("buffer2", "r"); REQUIRE(verify);
        REQUIRE(fgetc(verify) == 'g' && !fclose(verify) && !fclose(other));
        clearerr(f);
        REQUIRE(!ferror(f) && fflush(f) == EOF && ferror(f));
        REQUIRE(fclose(f) == EOF);
        f = fopen("buffer", "r+"); REQUIRE(f);
        volatile size_t huge = SIZE_MAX;
        char c = 'x';
        REQUIRE(fwrite(&c, huge, 2, f) == 0 && ferror(f) && errno == EOVERFLOW);
        clearerr(f);
        REQUIRE(fread(&c, huge, 2, f) == 0 && ferror(f) && errno == EOVERFLOW);
        REQUIRE(!fclose(f));
        for (unsigned i = 0; i < 128; ++i) {
            f = fopen("buffer", "w"); REQUIRE(f);
            REQUIRE(!setvbuf(f, NULL, _IOFBF, 16384));
            REQUIRE(fputc('x', f) == 'x' && !fclose(f));
        }
        REQUIRE(!fflush(NULL));
        return 0;
    }
    if (!strcmp(argv[1], "prompt")) {
        REQUIRE(!setvbuf(stdout, NULL, _IOLBF, 16));
        REQUIRE(fputs("prompt", stdout) >= 0);
        REQUIRE(getchar() == 'x');
        _Exit(0); /* Only the read-triggered flush can deliver the prompt. */
    }
    if (!strcmp(argv[1], "exit") || !strcmp(argv[1], "quick") || !strcmp(argv[1], "immediate")) {
        exit_file = fopen("buffer", "w"); REQUIRE(exit_file);
        REQUIRE(!setvbuf(exit_file, NULL, _IOFBF, 16));
        REQUIRE(!setvbuf(stdout, NULL, _IOFBF, 16));
        REQUIRE(fputs("file", exit_file) >= 0 && !atexit(finish));
        if (!strcmp(argv[1], "quick")) quick_exit(0);
        if (!strcmp(argv[1], "immediate")) _Exit(0);
        return 0;
    }
    return 99;
}
