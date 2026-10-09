#ifndef TURBOWASM_EXAMPLE_THREADED_SESSION_H
#define TURBOWASM_EXAMPLE_THREADED_SESSION_H

#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_threads.h>
#include <stdatomic.h>

/* Embedding example, not an installed Runtime ABI. One session owns one root;
 * it borrows module/source bytes, providers and a dedicated WASI thread group.
 * Keep all of them alive until root calls and actual child tasks have ended.
 * Root calls run outside the group's pool. A socket progress owner must pump
 * its transports/Preview1 while root or children execute on other threads. */
typedef enum threaded_state {
    THREADED_EMPTY, THREADED_INITIALIZING, THREADED_READY,
    THREADED_CLOSING, THREADED_FAILED, THREADED_CLOSED
} threaded_state;

typedef struct threaded_session {
    atomic_bool busy;
    threaded_state state;
    turbowasm_instance instance;
    const turbowasm_module *module;
    turbowasm_wasi_threads *threads;
    turbowasm_execution_options options;
    turbowasm_wasi_threads_execution_policy policy;
} threaded_session;

/* Zero-initialize at a stable address. options (including its interrupt
 * context) must remain usable through destroy. Start and CRT receive separate
 * copies of this budget. command selects _start; otherwise _initialize.
 * Command proc_exit returns INTERRUPTED with the group's recorded exit code. */
turbowasm_status threaded_session_open(threaded_session *session,
    const turbowasm_module *module, const turbowasm_linker *linker,
    turbowasm_wasi_threads *threads, bool command,
    const turbowasm_execution_options *options, turbowasm_trap *trap);
turbowasm_status threaded_session_call(threaded_session *session, const char *name,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap);
/* close export takes args and returns i32: zero means drained; nonzero retains
 * CLOSING and all resources, allowing another close attempt. Business calls
 * are closed once the first valid close starts. A runtime failure stops the
 * group and marks FAILED; never call arbitrary guest cleanup after that. */
turbowasm_status threaded_session_close(threaded_session *session, const char *name,
    const turbowasm_value *args, size_t argc, int32_t *application_status,
    turbowasm_trap *trap);
/* Never waits or executes guest cleanup. Rejects a busy root or active child.
 * After failure, keep host progress alive until this succeeds, then destroy
 * the group/providers/module in their ownership order. Lifecycle and external
 * thread-spawn entry must remain exclusive while destroying the group. */
bool threaded_session_destroy(threaded_session *session);

#endif
