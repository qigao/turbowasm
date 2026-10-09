#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../guest/metallic/src/wasi/wasi.h"

/* This guest links scripted definitions at the libc's WASI boundary. The
 * command runner still executes the real libc and observes its exit status. */
static unsigned calls, fail_on, zero_write;
static char output[64];
static size_t output_size, input_pos;
enum { WASI_BADF = 8, WASI_IO = 29 };

__wasi_errno_t __wasi_fd_write(__wasi_fd_t fd, const __wasi_ciovec_t *iov, size_t count, size_t *written)
{
    *written = 0;
    if (fd != 1 || count != 1) return WASI_BADF;
    if (++calls == fail_on) return WASI_IO;
    if (zero_write) return 0;
    size_t size = iov->buf_len < 2 ? iov->buf_len : 2;
    if (size > sizeof(output) - output_size) return WASI_IO;
    memcpy(output + output_size, iov->buf, size);
    output_size += size;
    *written = size;
    return 0;
}

__wasi_errno_t __wasi_fd_read(__wasi_fd_t fd, const __wasi_iovec_t *iov, size_t count, size_t *read)
{
    *read = 0;
    if (fd != 0 || count != 1) return WASI_BADF;
    if (++calls == fail_on) return WASI_IO;
    const char input[] = "abcdef";
    size_t size = sizeof(input) - 1 - input_pos;
    if (size > 2) size = 2;
    if (size > iov->buf_len) size = iov->buf_len;
    memcpy(iov->buf, input + input_pos, size);
    input_pos += size;
    *read = size;
    return 0;
}

#define REQUIRE(x) do { if (!(x)) _Exit(__LINE__); } while (0)
int main(int argc, char **argv)
{
    REQUIRE(argc == 3);
    if (!strcmp(argv[1], "write") || !strcmp(argv[1], "write-fill")) {
        char buffer[16];
        int fill = !strcmp(argv[1], "write-fill");
        REQUIRE(!setvbuf(stdout, buffer, _IOFBF, fill ? 8 : sizeof(buffer)));
        if (fill) {
            fail_on = 2;
            REQUIRE(fwrite("abcdefgh", 1, 8, stdout) == 2 && ferror(stdout) && errno == EIO);
        } else {
            REQUIRE(fwrite("abcdefgh", 1, 8, stdout) == 8 && calls == 0);
            fail_on = 2;
            REQUIRE(fflush(stdout) == EOF && ferror(stdout) && errno == EIO);
        }
        REQUIRE(output_size == 2 && !memcmp(output, "ab", 2));
        fail_on = 0; clearerr(stdout);
        REQUIRE(!fflush(stdout) && !ferror(stdout));
        REQUIRE(output_size == 8 && !memcmp(output, "abcdefgh", 8));
        REQUIRE(fputc('z', stdout) == 'z');
        zero_write = 1;
        REQUIRE(fflush(stdout) == EOF && errno == EIO);
        zero_write = 0; clearerr(stdout);
        REQUIRE(!fflush(stdout) && output_size == 9 && output[8] == 'z');
        REQUIRE(!fclose(stdout)); /* Borrowed automatic buffer cannot outlive main. */
        return 0;
    }
    REQUIRE(!setvbuf(stdin, NULL, _IOFBF, 8));
    char text[8] = {0};
    if (!strcmp(argv[1], "read-error")) {
        fail_on = 2;
        REQUIRE(fread(text, 1, 6, stdin) == 2 && ferror(stdin) && !feof(stdin));
        fail_on = 0; clearerr(stdin);
        REQUIRE(fread(text + 2, 1, 4, stdin) == 4 && !ferror(stdin) && !feof(stdin));
        REQUIRE(!memcmp(text, "abcdef", 6));
    } else {
        REQUIRE(!strcmp(argv[1], "read"));
        REQUIRE(fread(text, 1, 6, stdin) == 6 && !feof(stdin) && !ferror(stdin));
        REQUIRE(!memcmp(text, "abcdef", 6));
    }
    REQUIRE(fgetc(stdin) == EOF && feof(stdin) && !ferror(stdin));
    REQUIRE(!fclose(stdin));
    return 0;
}
