#ifndef TURBOWASM_STORE_H
#define TURBOWASM_STORE_H

#include <turbowasm/runtime.h>
#include <turbowasm/value.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbowasm_store {
    void *impl;
} turbowasm_store;
typedef struct turbowasm_root {
    void *impl;
} turbowasm_root;

typedef struct turbowasm_store_config {
    turbowasm_runtime_config runtime;
    size_t max_bytes;
    uint32_t max_objects;
    uint32_t max_roots;
} turbowasm_store_config;

typedef struct turbowasm_store_stats {
    size_t bytes;
    uint32_t objects;
    uint32_t roots; /* Host roots; active frame registrations are internal. */
    uint64_t collections;
} turbowasm_store_stats;

/* Zero limits select finite defaults: 64 MiB, 65536 objects, 4096 registrations
 * (host roots plus active frames). All operations belong to the creating thread.
 * GC values borrow the store; copied values do not keep an object alive.
 * Store/root handles own their implementation and must not be copied.
 * Output handles start zeroed. Budgets count requested payload/metadata bytes,
 * excluding allocator overhead. */
void turbowasm_store_config_init(turbowasm_store_config *config);
turbowasm_status turbowasm_store_create(turbowasm_store *store,
                                        const turbowasm_store_config *config);
/* Returns INVALID_ARGUMENT without changing the store while instances, active
 * executions or host roots remain. Release them before retrying destruction. */
turbowasm_status turbowasm_store_destroy(turbowasm_store *store);
/* Reclaims unreachable objects, invalidating their borrowed handles. */
turbowasm_status turbowasm_store_collect(turbowasm_store *store);
turbowasm_status turbowasm_store_get_stats(const turbowasm_store *store,
                                           turbowasm_store_stats *out);
/* Retain before an allocation/collection boundary. A failed retain leaves both
 * the borrow and the initially empty root unchanged. Cross-store or stale GC
 * handles return TYPE_MISMATCH; budget exhaustion returns OUT_OF_MEMORY.
 * Retaining a GC object does not extend the existing lifetime of function or
 * exception references stored in it: their owner instances must remain alive.
 * A complete lifecycle example is in examples/gc.c.
 * Check each returned status before using the root. */
turbowasm_status turbowasm_root_retain(turbowasm_store *store, const turbowasm_value *value,
                                       turbowasm_root *root);
turbowasm_status turbowasm_root_get(const turbowasm_root *root, turbowasm_value *out);
turbowasm_status turbowasm_root_release(turbowasm_root *root);

#ifdef __cplusplus
}
#endif
#endif
