#ifndef TURBOWASM_RUNTIME_ALLOC_H
#define TURBOWASM_RUNTIME_ALLOC_H
#include <turbowasm/runtime.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct turbowasm_runtime_scope {
    const turbowasm_runtime_config *previous;
} turbowasm_runtime_scope;
bool turbowasm_runtime_config_normalize(const turbowasm_runtime_config *config, turbowasm_runtime_config *out);
turbowasm_runtime_scope turbowasm_runtime_scope_enter(const turbowasm_runtime_config *config);
void turbowasm_runtime_scope_leave(turbowasm_runtime_scope scope);
void *turbowasm_rt_malloc(size_t size);
void *turbowasm_rt_calloc(size_t count, size_t size);
void *turbowasm_rt_realloc(void *pointer, size_t size);
void turbowasm_rt_free(void *pointer);
/* Internal immutable-allocation ownership handoff. The caller preflights every
 * allocation and reserves owner references before installing any finalizer.
 * Finalizers run after the allocating context's deallocator returns. */
typedef void (*turbowasm_allocation_release_fn)(void *owner, void *context, size_t bytes);
bool turbowasm_rt_allocation_unowned(const void *pointer);
void turbowasm_rt_allocation_own(void *pointer, void *owner, void *context, size_t bytes,
    turbowasm_allocation_release_fn release);
#endif
