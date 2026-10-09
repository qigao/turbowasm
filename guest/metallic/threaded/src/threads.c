#include "internal.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

__attribute__((import_module("wasi"), import_name("thread-spawn")))
extern int __metallic_spawn(void *);
extern unsigned __metallic_tls_size(void), __metallic_tls_align(void);

typedef struct {
    void *stack_top;
    void *tls;
    atomic_int terminal;
    void *stack;
    thrd_start_t function;
    void *argument;
    uint32_t generation;
    unsigned allocated, detached, join_claimed;
    int result;
    struct { uint32_t generation; void *value; } specific[METALLIC_TSS_CAPACITY];
} thread_record;
_Static_assert(offsetof(thread_record, tls) == 4, "thread entry TLS field");
_Static_assert(offsetof(thread_record, terminal) == 8, "thread entry terminal field");

static thread_record records[METALLIC_THREAD_CAPACITY + 1];
static struct { uint32_t generation; unsigned allocated; tss_dtor_t destroy; } keys[METALLIC_TSS_CAPACITY];
static metallic_lock registry;
static unsigned closed;
static _Thread_local thread_record *current;
static _Thread_local jmp_buf exit_target;
static _Thread_local unsigned destructing;

static thread_record *lookup(thrd_t thread) {
    if (thread.slot > METALLIC_THREAD_CAPACITY) return NULL;
    thread_record *record = &records[thread.slot];
    return record->allocated && record->generation == thread.generation ? record : NULL;
}
static void release_record(thread_record *record) {
    free(record->stack);
    free(record->tls);
    uint32_t generation = record->generation;
    /* Drain can still be waiting on this static record while another thread
     * joins it. Keep terminal published until a later admission initializes it;
     * clearing it here would lose the wakeup. Closed admission prevents reuse
     * during drain. Never reset an observed atomic with a plain memset. */
    record->stack_top = NULL;
    record->tls = NULL;
    memset(&record->stack, 0, sizeof(*record) - offsetof(thread_record, stack));
    record->generation = generation;
}
static void reap_detached(void) {
    for (unsigned i = 1; i <= METALLIC_THREAD_CAPACITY; ++i) {
        thread_record *record = &records[i];
        if (record->allocated && record->detached &&
            atomic_load_explicit(&record->terminal, memory_order_acquire)) release_record(record);
    }
}
void __metallic_threads_init(void) {
    if (current || records[0].allocated) __builtin_trap();
    current = &records[0];
    current->generation = 1;
    current->allocated = 1;
}
unsigned __metallic_thread_token(void) {
    if (!current) __builtin_trap();
    return (unsigned)(current - records) + 1;
}
thrd_t thrd_current(void) {
    return (thrd_t){__metallic_thread_token() - 1, current->generation};
}
int thrd_equal(thrd_t left, thrd_t right) {
    return left.slot == right.slot && left.generation == right.generation;
}

int thrd_create(thrd_t *output, thrd_start_t function, void *argument) {
    if (!output || !function || !current) return thrd_error;
    unsigned size = __metallic_tls_size(), alignment = __metallic_tls_align();
    if (!alignment || (alignment & (alignment - 1)) || alignment > METALLIC_THREAD_TLS_LIMIT ||
        size > METALLIC_THREAD_TLS_LIMIT || size > UINT_MAX - (alignment - 1)) return thrd_nomem;
    size = (size + alignment - 1) & ~(alignment - 1);
    if (!size) size = alignment;

    metallic_lock_acquire(&registry);
    reap_detached();
    if (closed) { metallic_lock_release(&registry); return thrd_error; }
    unsigned slot;
    for (slot = 1; slot <= METALLIC_THREAD_CAPACITY; ++slot)
        if (!records[slot].allocated && records[slot].generation != UINT32_MAX) break;
    if (slot > METALLIC_THREAD_CAPACITY) { metallic_lock_release(&registry); return thrd_nomem; }
    thread_record *record = &records[slot];
    record->allocated = 1;
    ++record->generation;
    record->stack = aligned_alloc(16, METALLIC_THREAD_STACK_BYTES);
    record->tls = aligned_alloc(alignment, size);
    if (!record->stack || !record->tls) {
        release_record(record); metallic_lock_release(&registry); return thrd_nomem;
    }
    record->stack_top = (unsigned char *)record->stack + METALLIC_THREAD_STACK_BYTES;
    record->function = function; record->argument = argument;
    atomic_init(&record->terminal, 0);
    int tid = __metallic_spawn(record);
    if (tid <= 0) {
        release_record(record); metallic_lock_release(&registry);
        return tid == -2 ? thrd_nomem : thrd_error;
    }
    *output = (thrd_t){slot, record->generation};
    metallic_lock_release(&registry);
    return thrd_success;
}

int thrd_join(thrd_t thread, int *result) {
    if (!current || thrd_equal(thread, thrd_current())) return thrd_error;
    metallic_lock_acquire(&registry);
    thread_record *record = lookup(thread);
    if (!record || thread.slot == 0 || record->detached || record->join_claimed) {
        metallic_lock_release(&registry); return thrd_error;
    }
    record->join_claimed = 1;
    metallic_lock_release(&registry);
    while (!atomic_load_explicit(&record->terminal, memory_order_acquire))
        __builtin_wasm_memory_atomic_wait32((int *)&record->terminal, 0, -1);
    metallic_lock_acquire(&registry);
    if (result) *result = record->result;
    release_record(record);
    metallic_lock_release(&registry);
    return thrd_success;
}
int thrd_detach(thrd_t thread) {
    metallic_lock_acquire(&registry);
    thread_record *record = lookup(thread);
    if (!record || thread.slot == 0 || record->detached || record->join_claimed) {
        metallic_lock_release(&registry); return thrd_error;
    }
    record->detached = 1;
    if (atomic_load_explicit(&record->terminal, memory_order_acquire)) release_record(record);
    metallic_lock_release(&registry);
    return thrd_success;
}

int tss_create(tss_t *key, tss_dtor_t destroy) {
    if (!key) return thrd_error;
    metallic_lock_acquire(&registry);
    for (unsigned i = 0; i < METALLIC_TSS_CAPACITY; ++i) {
        if (!keys[i].allocated && keys[i].generation != UINT32_MAX) {
            keys[i].allocated = 1; ++keys[i].generation; keys[i].destroy = destroy;
            *key = (tss_t){i, keys[i].generation};
            metallic_lock_release(&registry); return thrd_success;
        }
    }
    metallic_lock_release(&registry); return thrd_error;
}
static int valid_key(tss_t key) {
    return key.slot < METALLIC_TSS_CAPACITY && keys[key.slot].allocated &&
        key.generation == keys[key.slot].generation;
}
void tss_delete(tss_t key) {
    metallic_lock_acquire(&registry);
    if (valid_key(key)) { keys[key.slot].allocated = 0; keys[key.slot].destroy = NULL; }
    metallic_lock_release(&registry);
}
void *tss_get(tss_t key) {
    if (!current) return NULL;
    metallic_lock_acquire(&registry);
    void *value = valid_key(key) && current->specific[key.slot].generation == key.generation ?
        current->specific[key.slot].value : NULL;
    metallic_lock_release(&registry);
    return value;
}
int tss_set(tss_t key, void *value) {
    if (!current) return thrd_error;
    metallic_lock_acquire(&registry);
    if (!valid_key(key)) { metallic_lock_release(&registry); return thrd_error; }
    current->specific[key.slot].generation = key.generation;
    current->specific[key.slot].value = value;
    metallic_lock_release(&registry); return thrd_success;
}
static void run_destructors(void) {
    destructing = 1;
    for (unsigned pass = 0; pass < TSS_DTOR_ITERATIONS; ++pass) {
        unsigned called = 0;
        for (unsigned i = 0; i < METALLIC_TSS_CAPACITY; ++i) {
            metallic_lock_acquire(&registry);
            void *value = current->specific[i].value;
            tss_dtor_t destroy = keys[i].allocated &&
                current->specific[i].generation == keys[i].generation ? keys[i].destroy : NULL;
            current->specific[i].value = NULL;
            metallic_lock_release(&registry);
            if (value && destroy) { ++called; destroy(value); }
        }
        if (!called) break;
    }
}
void __metallic_thread_run(int tid, thread_record *record) {
    (void)tid;
    current = record;
    /* Acquire the creator's completed reservation before consuming its data. */
    metallic_lock_acquire(&registry);
    thrd_start_t function = record->function;
    void *argument = record->argument;
    metallic_lock_release(&registry);
    if (setjmp(exit_target) == 0) record->result = function(argument);
    run_destructors();
    /* Assembly publishes terminal only after this C frame has returned. */
}

int __metallic_threads_drain(const struct timespec *deadline) {
    if (current != &records[0]) return thrd_error;
    metallic_lock_acquire(&registry);
    closed = 1;
    metallic_lock_release(&registry);
    for (;;) {
        metallic_lock_acquire(&registry);
        thread_record *waiting = NULL;
        for (unsigned i = 1; i <= METALLIC_THREAD_CAPACITY; ++i) {
            thread_record *record = &records[i];
            if (!record->allocated) continue;
            if (!atomic_load_explicit(&record->terminal, memory_order_acquire)) { waiting = record; break; }
        }
        if (!waiting) {
            /* A live child may still intend to join an already terminal peer.
             * Reclaim unclaimed joinable records only after all children end. */
            for (unsigned i = 1; i <= METALLIC_THREAD_CAPACITY; ++i) {
                if (records[i].allocated) release_record(&records[i]);
            }
        }
        metallic_lock_release(&registry);
        if (!waiting) return thrd_success;
        int64_t timeout = deadline ? __metallic_deadline_ns(deadline) : -1;
        if (deadline && timeout <= 0) return timeout == 0 ? thrd_timedout : thrd_error;
        __builtin_wasm_memory_atomic_wait32((int *)&waiting->terminal, 0, timeout);
    }
}
_Noreturn void thrd_exit(int result) {
    if (!current || destructing) __builtin_trap();
    current->result = result;
    if (current != &records[0]) longjmp(exit_target, 1);
    run_destructors();
    if (__metallic_threads_drain(NULL) != thrd_success) __builtin_trap();
    exit(0);
}
