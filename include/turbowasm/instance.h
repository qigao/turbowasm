#ifndef TURBOWASM_INSTANCE_H
#define TURBOWASM_INSTANCE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbowasm_trap {
    TURBOWASM_TRAP_NONE = 0,
    TURBOWASM_TRAP_UNREACHABLE,
    TURBOWASM_TRAP_CALL_STACK_EXHAUSTED
} turbowasm_trap;

typedef struct turbowasm_instance {
    void *impl;
} turbowasm_instance;

/* The module must outlive the instance.  Start functions are not executed by
 * this first interpreter slice; modules with a start function are rejected as
 * unsupported until instantiation semantics are implemented. */
turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module);

void turbowasm_instance_destroy(turbowasm_instance *instance);

const turbowasm_module *turbowasm_instance_module(
    const turbowasm_instance *instance);

turbowasm_status turbowasm_instance_call(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    uint32_t argument_count,
    turbowasm_value *results,
    uint32_t result_capacity,
    uint32_t *out_result_count,
    turbowasm_trap *out_trap);

const char *turbowasm_trap_string(turbowasm_trap trap);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_INSTANCE_H */
