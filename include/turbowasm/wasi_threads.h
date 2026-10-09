#ifndef TURBOWASM_WASI_THREADS_H
#define TURBOWASM_WASI_THREADS_H

#include <cflow/executor.h>
#include <turbowasm/link.h>
#include <turbowasm/instance.h>

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
    TURBOWASM_WASI_THREADS_SPAWN_TID_EXHAUSTED = -5,
    TURBOWASM_WASI_THREADS_SPAWN_GROUP_TERMINATED = -6
};

typedef struct turbowasm_wasi_threads {
    void *impl;
} turbowasm_wasi_threads;

typedef struct turbowasm_wasi_threads_execution_policy {
    turbowasm_wasi_threads *threads;
    turbowasm_interrupt_check_fn chained_interrupt;
    void *chained_context;
} turbowasm_wasi_threads_execution_policy;

/*
 * Explicitly compose group-fatal interruption into a root/main invocation.
 * Child invocations use this policy internally. Existing caller interruption
 * policy is chained after the group-fatal check.
 */
bool turbowasm_wasi_threads_execution_policy_init(
    turbowasm_wasi_threads_execution_policy *policy,
    turbowasm_wasi_threads *threads);

bool turbowasm_wasi_threads_execution_policy_apply(
    turbowasm_wasi_threads_execution_policy *policy,
    turbowasm_execution_options *options);

typedef struct turbowasm_wasi_threads_config {
    /*
     * Borrowed concurrent CFlow executor. It must outlive this capability and
     * all admitted child tasks. Before waiting for executor shutdown, publish
     * group exit if children may be blocked: CANCEL_PENDING only cancels queued
     * tasks when a worker can deliver their callbacks; it cannot stop running
     * callbacks. The owner must also keep enough workers available for progress.
     */
    cflow_executor *executor;

    /* Fixed maximum number of concurrently live child instances. */
    size_t capacity;
} turbowasm_wasi_threads_config;

turbowasm_status turbowasm_wasi_threads_init(
    turbowasm_wasi_threads *threads,
    const turbowasm_wasi_threads_config *config);

/*
 * Own a private pool with capacity workers and capacity live-child slots.
 * The root must execute outside this pool. Each admitted child can then run
 * even when other children block in guest join or atomic wait. Excess spawn
 * returns SPAWN_CAPACITY; application-level lock cycles can still deadlock.
 *
 * threads must be zero-initialized. NULL/nonempty threads, zero capacity or
 * capacity outside the TID/slot representable range return INVALID_ARGUMENT.
 * Allocation/worker creation failure returns OUT_OF_MEMORY and leaves threads
 * empty. Destroy drains finalizers and joins this owned pool after children
 * become inactive; it never shuts down a borrowed executor.
 *
 * Example: turbowasm_wasi_threads threads = {0};
 *   status = turbowasm_wasi_threads_init_pool(&threads, 4);
 *   // Define imports, run root, then quiesce all consumers/children.
 *   if (status == TURBOWASM_OK) turbowasm_wasi_threads_destroy(&threads);
 */
turbowasm_status turbowasm_wasi_threads_init_pool(
    turbowasm_wasi_threads *threads, size_t capacity);

/*
 * Destroy succeeds only when no child task is active. Consumer instances whose
 * imported host function borrows this object must already be quiescent.
 * Calls from an owned pool worker are rejected. A rejected destroy preserves
 * the capability. External lifecycle operations must be serialized.
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

/*
 * Query the first fatal child terminal. Any unsuccessful child invocation
 * (including an uncaught exception or host error) publishes group-fatal once.
 * An accepted executor task cancelled before run publishes INTERRUPTED/NONE.
 * Earlier fatal/proc_exit state is preserved. Siblings/root using the policy leave
 * with TURBOWASM_INTERRUPTED at the next checkpoint or interruptible wait.
 */
bool turbowasm_wasi_threads_group_fatal(
    const turbowasm_wasi_threads *threads,
    turbowasm_status *out_status,
    turbowasm_trap *out_trap);

/*
 * Preview1 proc_exit callback compatible with turbowasm_wasi_proc_exit_fn.
 * Use the public turbowasm_wasi_threads object as callback context.
 *
 * The first proc_exit publishes the group terminal, records its exit code,
 * and wakes blocked shared-memory waiters without shutting down the borrowed
 * executor.
 */
void turbowasm_wasi_threads_proc_exit(
    void *context,
    turbowasm_instance *caller,
    uint32_t exit_code);

bool turbowasm_wasi_threads_group_exit_code(
    const turbowasm_wasi_threads *threads,
    uint32_t *out_exit_code);

/* Backward-compatible spelling retained for consumers that adopted the
 * earlier declaration. New code should prefer group_exit_code(). */
bool turbowasm_wasi_threads_group_exit(
    const turbowasm_wasi_threads *threads,
    uint32_t *out_exit_code);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_THREADS_H */
