#ifndef TURBOWASM_CFLOW_H
#define TURBOWASM_CFLOW_H

#include <cflow/executor.h>
#include <cflow/clock.h>
#include <turbowasm/instance.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct turbowasm_cflow_deadline_policy {
    cflow_clock *clock;
    cflow_deadline deadline;
    turbowasm_interrupt_check_fn chained_interrupt;
    void *chained_context;
} turbowasm_cflow_deadline_policy;

/*
 * Initialize a borrowed-clock deadline. The policy does not own or destroy
 * the clock. init_after uses Salts' saturating cflow_deadline_after().
 */
bool turbowasm_cflow_deadline_init_after(
    turbowasm_cflow_deadline_policy *policy,
    cflow_clock *clock,
    cflow_duration delay);

bool turbowasm_cflow_deadline_init_at(
    turbowasm_cflow_deadline_policy *policy,
    cflow_clock *clock,
    cflow_deadline deadline);

/*
 * Compose the deadline with existing execution options. Fuel fields are left
 * untouched. An existing interruption predicate is chained after the deadline.
 */
bool turbowasm_cflow_deadline_apply(
    turbowasm_cflow_deadline_policy *policy,
    turbowasm_execution_options *options);

typedef enum turbowasm_cflow_terminal_kind {
    TURBOWASM_CFLOW_EXECUTED = 0,
    TURBOWASM_CFLOW_CANCELLED
} turbowasm_cflow_terminal_kind;

typedef struct turbowasm_cflow_completion {
    turbowasm_cflow_terminal_kind terminal;
    /*
     * status/trap/result_count describe the Wasm invocation only when
     * terminal == TURBOWASM_CFLOW_EXECUTED. Scheduler cancellation means
     * Wasm was not entered and leaves these fields at their zero state.
     */
    turbowasm_status status;
    turbowasm_trap trap;
    size_t result_count;
} turbowasm_cflow_completion;

typedef void (*turbowasm_cflow_complete_fn)(
    void *user,
    const turbowasm_cflow_completion *completion);

typedef void (*turbowasm_cflow_finalize_fn)(void *user);

/*
 * Caller-owned descriptor for one CFlow-scheduled invocation.
 *
 * A successful admission borrows this descriptor, instance, arguments and
 * results until finalize returns (or until run/cancel returns when finalize is
 * NULL). The descriptor itself is not copied by TurboWasm, while options are
 * stored inline so no external options object is borrowed.
 *
 * Exactly one executed or cancelled completion is published for accepted work,
 * followed by finalize when non-NULL. Rejected admission invokes no callback.
 *
 * TurboWasm instances are not internally synchronized. Calls targeting the
 * same instance must be serialized by the caller (for example through one
 * Manual/Serial CFlow lane). Calls targeting distinct instances may be posted
 * to concurrent executors.
 */
typedef struct turbowasm_cflow_call {
    turbowasm_instance *instance;
    uint32_t function_index;

    const turbowasm_value *arguments;
    size_t argument_count;
    turbowasm_value *results;
    size_t result_capacity;

    bool use_options;
    turbowasm_execution_options options;

    turbowasm_cflow_complete_fn complete;
    turbowasm_cflow_finalize_fn finalize;
    void *user;
} turbowasm_cflow_call;

/*
 * Attempt bounded non-blocking admission through a built-in CFlow Executor.
 *
 * On CFLOW_ADMISSION_ACCEPTED, CFlow owns the call borrow until its terminal
 * run/cancel + finalize lifecycle settles. On any other result, no callback is
 * invoked and ownership remains entirely with the caller.
 */
cflow_admission_status turbowasm_cflow_try_submit(
    cflow_executor *executor,
    turbowasm_cflow_call *call);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_CFLOW_H */
