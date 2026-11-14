#include <metallic/threads.h>
#include <stdatomic.h>
#include <stdint.h>

int cmeta_metadata(void);
int cmeta_calls(void);
int cmeta_object_open(void);
int cmeta_object_read(void);
int cmeta_object_close(void);
int cmeta_alignment(void);

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
enum { CMETA_WORKERS = 4, CMETA_ROUNDS = 32 };
static atomic_uint arrived, released;

static int cmeta_worker(void *argument) {
    (void)argument;
    atomic_fetch_add(&arrived, 1);
    __builtin_wasm_memory_atomic_notify((int *)&arrived, UINT32_MAX);
    while (!atomic_load(&released))
        __builtin_wasm_memory_atomic_wait32((int *)&released, 0, -1);
    for (unsigned i = 0; i < CMETA_ROUNDS; ++i) {
        REQUIRE(cmeta_metadata() == 0);
        REQUIRE(cmeta_calls() == 0);
        REQUIRE(cmeta_object_read() == -1);
        REQUIRE(cmeta_object_open() == 42);
        thrd_yield();
        REQUIRE(cmeta_object_read() == 42);
        REQUIRE(cmeta_object_close() == (int)i + 1);
        REQUIRE(cmeta_object_read() == -1);
    }
    REQUIRE(cmeta_alignment() == 0);
    return 0;
}

int cmeta_concurrent(void) {
    thrd_t children[CMETA_WORKERS];
    /* Root storage and lifetime must survive all independent child objects. */
    REQUIRE(cmeta_object_open() == 42);
    for (unsigned i = 0; i < CMETA_WORKERS; ++i)
        REQUIRE(thrd_create(&children[i], cmeta_worker, NULL) == thrd_success);
    while (atomic_load(&arrived) != CMETA_WORKERS) {
        unsigned seen = atomic_load(&arrived);
        if (seen != CMETA_WORKERS)
            __builtin_wasm_memory_atomic_wait32((int *)&arrived, (int)seen, -1);
    }
    atomic_store(&released, 1);
    __builtin_wasm_memory_atomic_notify((int *)&released, UINT32_MAX);
    for (unsigned i = 0; i < CMETA_WORKERS; ++i) {
        int result;
        REQUIRE(thrd_join(children[i], &result) == thrd_success);
        if (result) return result;
    }
    REQUIRE(metallic_threads_close(NULL) == thrd_success);
    REQUIRE(cmeta_metadata() == 0 && cmeta_calls() == 0);
    REQUIRE(cmeta_object_read() == 42);
    REQUIRE(cmeta_object_close() == 1);
    REQUIRE(cmeta_alignment() == 0);
    return 0;
}
