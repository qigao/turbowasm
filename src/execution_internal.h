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

/* Runs once on the invocation coroutine before Core entry. Fills the reserved
 * fixed-signature arguments; context/temporary owners survive suspension. The
 * adapter validates types and cleans partial conversion before returning an
 * error. No borrowed argument/control pointer may outlive this execution. */
typedef turbowasm_status (*turbowasm_execution_prepare_fn)(
    void *context, turbowasm_value *arguments, size_t argument_count,
    turbowasm_jit_execution_control *control, turbowasm_trap *trap);

/* Internal host-task entry using the same coroutine/control/lifetime as Core
 * execution. It borrows instance/context, has no raw parameters or results and
 * permits prepare/completion hooks. Public Core execution admission is unchanged. */
turbowasm_status turbowasm_execution_create_host_entry(turbowasm_execution *execution,
    turbowasm_instance *instance, turbowasm_host_entry_fn entry, void *context);

turbowasm_status turbowasm_execution_set_prepare(turbowasm_execution *execution,
    turbowasm_execution_prepare_fn prepare, void *context);

turbowasm_status turbowasm_execution_set_completion(
    turbowasm_execution *execution,
    turbowasm_execution_completion_fn completion, void *context);

/* Borrowed on the execution owner thread until execution destruction. Used by
 * task scheduling to account a nested quantum before its Core handle is freed. */
turbowasm_jit_execution_control *turbowasm_execution_control_get(turbowasm_execution *execution);

#endif
