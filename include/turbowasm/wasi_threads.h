#ifndef TURBOWASM_WASI_THREADS_H
#define TURBOWASM_WASI_THREADS_H

#include <cflow/executor.h>
#include <turbowasm/link.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Legacy WASI Preview1 threads adapter.
 *
 * The upstream ABI exposes exactly one host import:
 *   "wasi"."thread-spawn" : (i32 start_arg) -> i32
 *
 * Positive results are TurboWasm-assigned TIDs. Negative results are stable
 * adapter errors. Join/synchronization remain guest-side Wasm atomics +
 * memory.atomic.wait/notify; TurboWasm does not invent a host join API.
 */
enum {
    TURBOWASM_WASI_THREADS_SPAWN_BAD_MODULE = -1,
    TURBOWASM_WASI_THREADS_SPAWN_CAPACITY = -2,
    TURBOWASM_WASI_THREADS_SPAWN_INSTANTIATE = -3,
    TURBOWASM_WASI_THREADS_SPAWN_EXECUTOR = -4,
    TURBOWASM_WASI_THREADS_SPAWN_TID_EXHAUSTED = -5
};

typedef struct turbowasm_wasi_threads {
    void *impl;
} turbowasm_wasi_threads;

typedef struct turbowasm_wasi_threads_config {
    /*
     * Borrowed concurrent CFlow executor. It must outlive this capability and
     * all admitted child tasks.
     */
    cflow_executor *executor;

    /* Fixed maximum number of concurrently live child instances. */
    size_t capacity;
} turbowasm_wasi_threads_config;

turbowasm_status turbowasm_wasi_threads_init(
    turbowasm_wasi_threads *threads,
    const turbowasm_wasi_threads_config *config);

/*
 * Destroy succeeds only when no child task is active. Consumer instances whose
 * imported host function borrows this object must already be quiescent.
 */
bool turbowasm_wasi_threads_destroy(
    turbowasm_wasi_threads *threads);

/*
 * Register "wasi"."thread-spawn". The capability object must outlive all
 * linked instances that may invoke the import.
 */
turbowasm_status turbowasm_wasi_threads_define(
    turbowasm_wasi_threads *threads,
    turbowasm_linker *linker);

size_t turbowasm_wasi_threads_active(
    const turbowasm_wasi_threads *threads);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_THREADS_H */
