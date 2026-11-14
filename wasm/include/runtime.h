#ifndef TURBOWASM_RUNTIME_H
#define TURBOWASM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*turbowasm_allocate_fn)(void *context, size_t size);
typedef void *(*turbowasm_reallocate_fn)(void *context, void *pointer, size_t size);
typedef void (*turbowasm_deallocate_fn)(void *context, void *pointer);

typedef struct turbowasm_allocator {
    void *context;
    turbowasm_allocate_fn allocate;
    turbowasm_reallocate_fn reallocate;
    turbowasm_deallocate_fn deallocate;
} turbowasm_allocator;

typedef struct turbowasm_resource_limits {
    size_t max_module_bytes;
    size_t max_allocation_bytes;
    size_t max_linear_memory_bytes;
    uint32_t max_table_elements;
} turbowasm_resource_limits;

typedef struct turbowasm_runtime_config {
    turbowasm_allocator allocator;
    turbowasm_resource_limits limits;
} turbowasm_runtime_config;

/* Zero initialization means default allocator and unlimited resources.
 * Custom allocate/deallocate callbacks must be supplied as a pair.
 * reallocate is optional; allocator.context is borrowed. */
void turbowasm_runtime_config_init(turbowasm_runtime_config *config);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_RUNTIME_H */
