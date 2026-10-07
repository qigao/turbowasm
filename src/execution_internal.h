#ifndef TURBOWASM_EXECUTION_INTERNAL_H
#define TURBOWASM_EXECUTION_INTERNAL_H

#include <turbowasm/execution.h>
#include "instance_internal.h"

/* Runs once after a successful Core return, on the invocation's coroutine.
 * Results and control remain execution-owned; context must outlive execution.
 * The hook may invoke more Core code with the same control and suspend. */
typedef turbowasm_status (*turbowasm_execution_completion_fn)(
    void *context, const turbowasm_value *results, size_t result_count,
    turbowasm_jit_execution_control *control, turbowasm_trap *trap);

turbowasm_status turbowasm_execution_set_completion(
    turbowasm_execution *execution,
    turbowasm_execution_completion_fn completion, void *context);

/* Borrowed on the execution owner thread until execution destruction. Used by
 * task scheduling to account a nested quantum before its Core handle is freed. */
turbowasm_jit_execution_control *turbowasm_execution_control_get(turbowasm_execution *execution);

#endif
