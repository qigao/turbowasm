#ifndef TURBOWASM_GC_EXEC_H
#define TURBOWASM_GC_EXEC_H
#include "instance_internal.h"
#include "reader.h"
typedef struct turbowasm_gc_effect {
    uint32_t consumed;
    bool produces;
    bool branches;
    uint32_t label;
    turbowasm_value value;
} turbowasm_gc_effect;
turbowasm_status turbowasm_gc_execute(turbowasm_instance_impl *instance, turbowasm_reader *reader,
                                      const turbowasm_value *values, uint32_t count,
                                      turbowasm_gc_effect *effect, turbowasm_trap *trap);
bool turbowasm_gc_equal(turbowasm_store_impl *store, turbowasm_gcref left, turbowasm_gcref right);
#endif
