#ifndef TURBOWASM_EXAMPLE_REACTOR_SESSION_H
#define TURBOWASM_EXAMPLE_REACTOR_SESSION_H

#include <turbowasm/turbowasm.h>
#include <stdatomic.h>

/* Example embedding policy, not an installed Runtime ABI. Initialize storage
 * with {0}; its address must remain stable until all callers have stopped.
 * Module bytes, linker providers and host capabilities are borrowed. */
typedef enum reactor_session_state {
    REACTOR_EMPTY, REACTOR_INITIALIZING, REACTOR_READY,
    REACTOR_CLOSING, REACTOR_FAILED, REACTOR_CLOSED
} reactor_session_state;

typedef struct reactor_session {
    atomic_bool busy;
    reactor_session_state state;
    turbowasm_instance instance;
    const turbowasm_module *module;
    uint64_t fuel;
} reactor_session;

/* Nonblocking admission: concurrent/reentrant calls return INVALID_ARGUMENT.
 * Initialization requires _initialize : () -> () and no Wasm start section.
 * State fields are owner-only; other threads must use the guarded operations. */
turbowasm_status reactor_session_open(reactor_session *session,
    const turbowasm_module *module, const turbowasm_linker *linker,
    uint64_t fuel, turbowasm_trap *trap);
turbowasm_status reactor_session_call(reactor_session *session, const char *name,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t capacity, size_t *count, turbowasm_trap *trap);
/* close_export : () -> i32 explicitly releases application resources/flushes
 * stdio. Its return code is reported even though close is terminal. A FAILED
 * session skips guest code. Host teardown is available through destroy. */
turbowasm_status reactor_session_close(reactor_session *session,
    const char *close_export, int32_t *application_status, turbowasm_trap *trap);
/* Does not execute guest cleanup. Rejects destruction while a call is active.
 * After success, borrowed providers/capabilities may be released. */
bool reactor_session_destroy(reactor_session *session);

#endif
