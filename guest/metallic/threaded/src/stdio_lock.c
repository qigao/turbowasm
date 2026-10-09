#include "../../src/stdio/FILE.h"
#include "internal.h"
#include <string.h>

static _Thread_local unsigned operation_depth;

metallic_stdio_guard __stdio_operation_enter(FILE *stream, int input) {
    /* Never acquire another stream while a public input operation holds its
     * stream. Flush line buffers before the outermost input lock instead. */
    if (input && !operation_depth && stream && stream->descriptor_stream) (void)__stdio_flush_all(1);
    if (operation_depth == UINT_MAX) __builtin_trap();
    ++operation_depth;
    if (stream) {
        unsigned token = __metallic_thread_token();
        for (;;) {
            unsigned owner = 0;
            if (atomic_compare_exchange_weak_explicit(&stream->lock_owner, &owner, token,
                    memory_order_acquire, memory_order_relaxed)) {
                stream->lock_depth = 1;
                break;
            }
            if (owner == token) {
                if (stream->lock_depth == UINT_MAX) __builtin_trap();
                ++stream->lock_depth;
                break;
            }
            if (owner) __builtin_wasm_memory_atomic_wait32((int *)&stream->lock_owner, (int)owner, -1);
        }
    }
    return (metallic_stdio_guard){stream, 1};
}

void __stdio_operation_leave(metallic_stdio_guard *guard) {
    if (!guard->active) return;
    FILE *stream = guard->stream;
    if (stream && --stream->lock_depth == 0) {
        atomic_store_explicit(&stream->lock_owner, 0, memory_order_release);
        __builtin_wasm_memory_atomic_notify((int *)&stream->lock_owner, 1);
    }
    --operation_depth;
    guard->active = 0;
}

void __stdio_reset(FILE *stream, const FILE *fresh) {
    /* freopen owns the operation lock. Preserve it and any flush-all references
     * acquired before unlinking; a bulk FILE assignment would destroy both. */
    memcpy(stream, fresh, offsetof(FILE, lock_owner));
}
