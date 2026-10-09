#include "../../guest/metallic/src/wasi/wasi.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "internal.h"

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
enum { WORKERS = 4, ROUNDS = 8, RETRIES = 10000 };
static atomic_int arrived, released;
extern uintptr_t __metallic_brk;
extern unsigned char __heap_base;
extern void __wasm_call_ctors(void);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("random_get")))
extern uint32_t random_bytes(void *, size_t);

int initialize(void) {
    __metallic_brk = (uintptr_t)&__heap_base;
    __metallic_threads_init();
    __wasm_call_ctors();
    return 0;
}
/* Namespace transactions report BUSY without consuming ownership. This test
 * application retries only that documented conflict, with a finite bound. */
static FILE *open_retry(const char *path, const char *mode) {
    for (unsigned i = 0; i < RETRIES; ++i) {
        FILE *file = fopen(path, mode);
        if (file || errno != EBUSY) return file;
        thrd_yield();
    }
    return NULL;
}
static int rename_retry(const char *from, const char *to) {
    for (unsigned i = 0; i < RETRIES; ++i) {
        int result = rename(from, to);
        if (!result || errno != EBUSY) return result;
        thrd_yield();
    }
    return -1;
}
static int file_worker(void *argument) {
    unsigned id = (unsigned)(uintptr_t)argument;
    char path[] = "worker-0", moved[] = "moved-0";
    path[7] = moved[6] = (char)('0' + id);
    atomic_fetch_add(&arrived, 1);
    __builtin_wasm_memory_atomic_notify((int *)&arrived, UINT32_MAX);
    while (!atomic_load(&released)) __builtin_wasm_memory_atomic_wait32((int *)&released, 0, -1);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        unsigned char original[256], copy[512], random[32];
        for (unsigned i = 0; i < sizeof(original); ++i) original[i] = (unsigned char)(i ^ id ^ round);
        REQUIRE(random_bytes(random, sizeof(random)) == 0);
        for (unsigned i = 0; i < sizeof(random); ++i) REQUIRE(random[i] == 0x6d);
        FILE *file = open_retry(path, "w+b");
        REQUIRE(file != NULL);
        REQUIRE(fwrite(original, 1, sizeof(original), file) == sizeof(original));
        REQUIRE(fflush(file) == 0 && ftell(file) == sizeof(original));
        REQUIRE(fseek(file, 0, SEEK_SET) == 0);
        REQUIRE(fread(copy, 1, sizeof(original), file) == sizeof(original));
        REQUIRE(memcmp(copy, original, sizeof(original)) == 0);
        REQUIRE(fclose(file) == 0);
        file = open_retry(path, "a+b"); REQUIRE(file != NULL);
        REQUIRE(fwrite(original, 1, sizeof(original), file) == sizeof(original));
        REQUIRE(fclose(file) == 0);
        __wasi_filestat_t stat;
        REQUIRE(__wasi_path_filestat_get(3, 0, path, strlen(path), &stat) == 0 && stat.size == sizeof(copy));
        REQUIRE(rename_retry(path, moved) == 0);
        file = open_retry(moved, "rb"); REQUIRE(file != NULL);
        REQUIRE(fread(copy, 1, sizeof(copy), file) == sizeof(copy));
        REQUIRE(memcmp(copy, original, sizeof(original)) == 0);
        REQUIRE(memcmp(copy + sizeof(original), original, sizeof(original)) == 0);
        REQUIRE(fclose(file) == 0);
        REQUIRE(remove(moved) == 0);
    }
    return 0;
}
int filesystem(void) {
    thrd_t children[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        REQUIRE(thrd_create(&children[i], file_worker, (void *)(uintptr_t)i) == thrd_success);
    while (atomic_load(&arrived) != WORKERS) {
        int seen = atomic_load(&arrived);
        if (seen != WORKERS) __builtin_wasm_memory_atomic_wait32((int *)&arrived, seen, -1);
    }
    atomic_store(&released, 1);
    __builtin_wasm_memory_atomic_notify((int *)&released, UINT32_MAX);
    for (unsigned i = 0; i < WORKERS; ++i) {
        int result;
        REQUIRE(thrd_join(children[i], &result) == thrd_success);
        if (result) return result;
    }
    REQUIRE(__metallic_threads_drain(NULL) == thrd_success);
    FILE *temporary = tmpfile(); REQUIRE(temporary != NULL);
    REQUIRE(fputs("temporary", temporary) >= 0);
    REQUIRE(fseek(temporary, 0, SEEK_SET) == 0);
    char text[10] = {0};
    REQUIRE(fread(text, 1, 9, temporary) == 9 && strcmp(text, "temporary") == 0);
    REQUIRE(fclose(temporary) == 0);
    return 0;
}
