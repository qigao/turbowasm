#ifndef TURBOWASM_INSTANCE_H
#define TURBOWASM_INSTANCE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbowasm_trap {
    TURBOWASM_TRAP_NONE = 0,
    TURBOWASM_TRAP_UNREACHABLE,
    TURBOWASM_TRAP_CALL_STACK_EXHAUSTED,
    TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO,
    TURBOWASM_TRAP_INTEGER_OVERFLOW
} turbowasm_trap;

typedef struct turbowasm_instance {
    void *impl;
} turbowasm_instance;

/* The instance borrows an already validated module.  The module and its
 * borrowed source bytes must outlive the instance. */
turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module);

void turbowasm_instance_destroy(turbowasm_instance *instance);

const turbowasm_module *turbowasm_instance_module(
    const turbowasm_instance *instance);

turbowasm_status turbowasm_instance_invoke(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap);

const char *turbowasm_trap_string(turbowasm_trap trap);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_INSTANCE_H */
