#ifndef TURBOWASM_STORE_INTERNAL_H
#define TURBOWASM_STORE_INTERNAL_H

#include <turbowasm/store.h>
#include "validation_context.h"

struct turbowasm_instance_impl;
typedef struct turbowasm_store_impl turbowasm_store_impl;
typedef struct turbowasm_gc_object {
    const turbowasm_validation_func_type *type;
    size_t bytes;
    uint32_t count;
    turbowasm_value *values;
} turbowasm_gc_object;

/* Intrusive registration borrows the source until removal. Coroutine teardown
 * removes every registration owned by its execution before releasing its stack. */
typedef struct turbowasm_gc_source {
    struct turbowasm_gc_source *next;
    const void *owner;
    void *context;
    void (*trace)(turbowasm_store_impl *, void *);
} turbowasm_gc_source;

bool turbowasm_store_is_owner(const turbowasm_store_impl *store);
bool turbowasm_gc_value_valid(turbowasm_store_impl *store, const turbowasm_value *value);
turbowasm_gc_object *turbowasm_gc_resolve(turbowasm_store_impl *store, turbowasm_gcref ref);
void turbowasm_gc_mark_values(turbowasm_store_impl *store, const turbowasm_value *values,
                              size_t count);
turbowasm_status turbowasm_gc_source_add(turbowasm_store_impl *store, turbowasm_gc_source *source);
void turbowasm_gc_source_remove(turbowasm_store_impl *store, turbowasm_gc_source *source);
void turbowasm_gc_sources_remove_owner(turbowasm_store_impl *store, const void *owner);
turbowasm_status turbowasm_gc_allocate(turbowasm_store_impl *store,
                                       const turbowasm_validation_func_type *type, uint32_t count,
                                       turbowasm_value *out);
turbowasm_status turbowasm_store_attach(turbowasm_store_impl *store,
                                        struct turbowasm_instance_impl *instance);
void turbowasm_store_detach(struct turbowasm_instance_impl *instance);
turbowasm_value turbowasm_gc_i31(uint32_t bits);
bool turbowasm_gc_is_i31(turbowasm_gcref ref);
uint32_t turbowasm_gc_i31_bits(turbowasm_gcref ref);

#endif
