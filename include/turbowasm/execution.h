#ifndef TURBOWASM_EXECUTION_H
#define TURBOWASM_EXECUTION_H

#include <turbowasm/link.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbowasm_execution {
    void *impl;
} turbowasm_execution;

typedef enum turbowasm_execution_state {
    TURBOWASM_EXECUTION_READY = 0,
    TURBOWASM_EXECUTION_RUNNING,
    TURBOWASM_EXECUTION_YIELDED,
    TURBOWASM_EXECUTION_COMPLETED,
    TURBOWASM_EXECUTION_TRAPPED,
    TURBOWASM_EXECUTION_EXCEPTION,
    TURBOWASM_EXECUTION_FAILED
} turbowasm_execution_state;

typedef enum turbowasm_yield_reason {
    TURBOWASM_YIELD_NONE = 0,
    TURBOWASM_YIELD_FUEL,
    TURBOWASM_YIELD_INTERRUPTION,
    TURBOWASM_YIELD_HOST_WAIT
} turbowasm_yield_reason;

/*
 * Create a restartable interpreter invocation.
 *
 * Arguments are copied into execution-owned storage. The instance and its
 * module are borrowed and must outlive the execution handle. Result storage is
 * owned by the execution until destroy.
 *
 * Resumable execution intentionally uses the interpreter reference path even
 * when a JIT backend is attached. Native-frame resume is not part of this
 * contract; eligible one-shot invoke calls keep their existing tiering path.
 */
turbowasm_status turbowasm_execution_create(
    turbowasm_execution *execution,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count);

void turbowasm_execution_destroy(turbowasm_execution *execution);

/*
 * Start or resume execution with a fresh safe-point budget.
 *
 * A cooperative suspension returns TURBOWASM_YIELDED and leaves state at
 * TURBOWASM_EXECUTION_YIELDED. Passing NULL options means unlimited fuel and
 * no interruption callback for this resume.
 */
turbowasm_status turbowasm_execution_resume(
    turbowasm_execution *execution,
    const turbowasm_execution_options *options);

turbowasm_execution_state turbowasm_execution_state_get(
    const turbowasm_execution *execution);

turbowasm_yield_reason turbowasm_execution_yield_reason_get(
    const turbowasm_execution *execution);

/*
 * Inspect/complete the one pending host wait.
 *
 * Completion only records the external terminal status; the retained callback
 * frame continues on a later turbowasm_execution_resume(). Calling resume
 * before completion leaves the execution yielded and does not enter Wasm.
 */
bool turbowasm_execution_pending_host_wait(
    const turbowasm_execution *execution,
    turbowasm_host_wait *out_wait);

turbowasm_status turbowasm_execution_complete_host_wait(
    turbowasm_execution *execution,
    turbowasm_host_wait wait,
    int status);

turbowasm_status turbowasm_execution_terminal_status(
    const turbowasm_execution *execution);

turbowasm_trap turbowasm_execution_trap(
    const turbowasm_execution *execution);

size_t turbowasm_execution_result_count(
    const turbowasm_execution *execution);

const turbowasm_value *turbowasm_execution_result_at(
    const turbowasm_execution *execution,
    size_t index);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_EXECUTION_H */
