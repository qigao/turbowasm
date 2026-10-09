#include "../../guest/metallic/src/wasi/wasi.h"
#include "../../guest/metallic/src/stdio/FILE_.h"
#include <threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
__attribute__((import_module("test"), import_name("stdio_stage")))
extern int stdio_stage(void);
static atomic_uint seen[64];

static int record_worker(void *argument) {
    int tag = 'A' + (int)(uintptr_t)argument;
    char payload[65]; wchar_t wide[17];
    memset(payload, tag, 64); payload[64] = 0;
    for (unsigned i = 0; i < 16; ++i) wide[i] = tag;
    wide[16] = 0;
    for (unsigned i = 0; i < 16; ++i) {
        REQUIRE(fprintf(stdout, "%c:%s:%c\n", tag, payload, tag) == 69);
        REQUIRE(fwprintf(stderr, L"%lc:%ls:%lc\n", (wint_t)tag, wide, (wint_t)tag) == 21);
        char input[6];
        REQUIRE(fgets(input, sizeof input, stdin) == input);
        REQUIRE(input[0] == 'R' && input[4] == '\n' && input[5] == 0);
        unsigned value = (unsigned)(input[1] - '0') * 100 + (unsigned)(input[2] - '0') * 10 + (unsigned)(input[3] - '0');
        REQUIRE(value < 64 && atomic_fetch_add(&seen[value], 1) == 0);
    }
    return 0;
}
int stdio_records(void) {
    REQUIRE(setvbuf(stdout, NULL, _IOFBF, 64) == 0);
    REQUIRE(fwide(stderr, 1) > 0);
    thrd_t children[4];
    for (unsigned i = 0; i < 4; ++i) REQUIRE(thrd_create(&children[i], record_worker, (void *)(uintptr_t)i) == thrd_success);
    for (unsigned i = 0; i < 4; ++i) {
        int result; REQUIRE(thrd_join(children[i], &result) == thrd_success && result == 0);
    }
    REQUIRE(fflush(NULL) == 0 && !ferror(stdout) && !ferror(stdin) && !ferror(stderr));
    for (unsigned i = 0; i < 64; ++i) REQUIRE(atomic_load(&seen[i]) == 1);
    return 0;
}

static int blocked_reader(void *argument) {
    (void)argument;
    return fgetc(stdin) == 'Q' ? 0 : __LINE__;
}
int stdio_progress(void) {
    thrd_t reader;
    REQUIRE(thrd_create(&reader, blocked_reader, NULL) == thrd_success);
    while (!stdio_stage()) thrd_yield();
    REQUIRE(fputs("release", stderr) >= 0);
    int result;
    REQUIRE(thrd_join(reader, &result) == thrd_success && result == 0);
    return 0;
}

static FILE *closing_stream;
static int close_stream(void *argument) { (void)argument; return fclose(closing_stream); }
static int flush_streams(void *argument) { (void)argument; return fflush(NULL); }
int stdio_close_flush(void) {
    /* A private descriptor fixture isolates libc retirement from the separate
     * shared-filesystem migration. The actual fclose/fflush implementations run. */
    closing_stream = malloc(sizeof(*closing_stream));
    REQUIRE(closing_stream != NULL);
    *closing_stream = FILE_(50);
    REQUIRE(setvbuf(closing_stream, NULL, _IOFBF, 64) == 0);
    REQUIRE(fputs("pending", closing_stream) >= 0);
    thrd_t closer, flusher;
    REQUIRE(thrd_create(&closer, close_stream, NULL) == thrd_success);
    while (!stdio_stage()) thrd_yield();
    REQUIRE(thrd_create(&flusher, flush_streams, NULL) == thrd_success);
    /* Force the close/flush overlap before allowing the provider to finish. */
    while (!atomic_load_explicit(&closing_stream->flush_refs, memory_order_acquire)) thrd_yield();
    REQUIRE(fputs("release", stderr) >= 0);
    int result;
    REQUIRE(thrd_join(closer, &result) == thrd_success && result == 0);
    REQUIRE(thrd_join(flusher, &result) == thrd_success && result == 0);
    REQUIRE(fflush(NULL) == 0);
    closing_stream = NULL;
    return 0;
}

static int reopen_stream(void *argument) {
    (void)argument;
    REQUIRE(freopen(NULL, "ab", closing_stream) == closing_stream);
    REQUIRE(fputs("reopened", closing_stream) >= 0);
    return 0;
}
int stdio_reopen_flush(void) {
    closing_stream = malloc(sizeof(*closing_stream));
    REQUIRE(closing_stream != NULL);
    *closing_stream = FILE_(50);
    REQUIRE(setvbuf(closing_stream, NULL, _IOFBF, 64) == 0);
    REQUIRE(fputs("pending", closing_stream) >= 0);
    thrd_t reopener, flusher;
    REQUIRE(thrd_create(&reopener, reopen_stream, NULL) == thrd_success);
    while (!stdio_stage()) thrd_yield();
    REQUIRE(thrd_create(&flusher, flush_streams, NULL) == thrd_success);
    while (!atomic_load_explicit(&closing_stream->flush_refs, memory_order_acquire)) thrd_yield();
    REQUIRE(fputs("release", stderr) >= 0);
    int result;
    REQUIRE(thrd_join(reopener, &result) == thrd_success && result == 0);
    REQUIRE(thrd_join(flusher, &result) == thrd_success && result == 0);
    REQUIRE(fclose(closing_stream) == 0 && fflush(NULL) == 0);
    closing_stream = NULL;
    return 0;
}

static int failing_writer(void *argument) {
    (void)argument;
    return fputs("fail", stdout) < 0 && ferror(stdout) ? 0 : __LINE__;
}
int stdio_error_release(void) {
    thrd_t writer;
    REQUIRE(thrd_create(&writer, failing_writer, NULL) == thrd_success);
    int result;
    REQUIRE(thrd_join(writer, &result) == thrd_success && result == 0);
    clearerr(stdout);
    REQUIRE(fputs("recovered", stdout) >= 0 && !ferror(stdout));
    return 0;
}
