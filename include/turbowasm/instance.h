#ifndef TURBOWASM_INSTANCE_H
#define TURBOWASM_INSTANCE_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdbool.h>
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
    TURBOWASM_TRAP_INTEGER_OVERFLOW,
    TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS,
    TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS,
    TURBOWASM_TRAP_INDIRECT_CALL_NULL,
    TURBOWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH,
    TURBOWASM_TRAP_INVALID_CONVERSION_TO_INTEGER,
    TURBOWASM_TRAP_NULL_REFERENCE,
    TURBOWASM_TRAP_UNALIGNED_ATOMIC,
    TURBOWASM_TRAP_EXPECTED_SHARED_MEMORY,
    TURBOWASM_TRAP_TOO_MANY_WAITERS
} turbowasm_trap;

typedef struct turbowasm_instance {
    void *impl;
} turbowasm_instance;

struct turbowasm_linker;

typedef bool (*turbowasm_interrupt_check_fn)(void *context);

typedef struct turbowasm_execution_options {
    uint64_t fuel;
    bool has_fuel_limit;
    turbowasm_interrupt_check_fn should_interrupt;
    void *interrupt_context;
} turbowasm_execution_options;

/* The instance borrows an already validated module.  The module and its
 * borrowed source bytes must outlive the instance. */
turbowasm_status turbowasm_instance_create(
    turbowasm_instance *instance,
    const turbowasm_module *module);

/*
 * Resolve imports through an explicit linker before executing the module start
 * function. Provider instances and their modules are borrowed and must outlive
 * the linked consumer instance. Imported functions, memories, tables, globals
 * and tags preserve the provider-owned WebAssembly store identity.
 */
turbowasm_status turbowasm_instance_create_linked(
    turbowasm_instance *instance,
    const turbowasm_module *module,
    const struct turbowasm_linker *linker);

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

turbowasm_status turbowasm_instance_invoke_with_options(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    const turbowasm_execution_options *options);

const char *turbowasm_trap_string(turbowasm_trap trap);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_INSTANCE_H */
