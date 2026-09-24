#include <turbowasm/cflow.h>

static bool turbowasm_cflow_call_valid(
    const turbowasm_cflow_call *call) {
    if (call == NULL || call->instance == NULL)
        return false;
    if (call->argument_count != 0u && call->arguments == NULL)
        return false;
    if (call->result_capacity != 0u && call->results == NULL)
        return false;
    return true;
}

static void turbowasm_cflow_run(void *user) {
    turbowasm_cflow_call *call = (turbowasm_cflow_call *)user;
    turbowasm_cflow_completion completion = {0};

    completion.terminal = TURBOWASM_CFLOW_EXECUTED;
    completion.trap = TURBOWASM_TRAP_NONE;

    if (call->use_options) {
        completion.status = turbowasm_instance_invoke_with_options(
            call->instance,
            call->function_index,
            call->arguments,
            call->argument_count,
            call->results,
            call->result_capacity,
            &completion.result_count,
            &completion.trap,
            &call->options);
    } else {
        completion.status = turbowasm_instance_invoke(
            call->instance,
            call->function_index,
            call->arguments,
            call->argument_count,
            call->results,
            call->result_capacity,
            &completion.result_count,
            &completion.trap);
    }

    if (call->complete != NULL)
        call->complete(call->user, &completion);
}

static void turbowasm_cflow_cancel(void *user) {
    turbowasm_cflow_call *call = (turbowasm_cflow_call *)user;
    turbowasm_cflow_completion completion = {0};

    completion.terminal = TURBOWASM_CFLOW_CANCELLED;
    completion.status = TURBOWASM_OK;
    completion.trap = TURBOWASM_TRAP_NONE;
    completion.result_count = 0u;

    if (call->complete != NULL)
        call->complete(call->user, &completion);
}

static void turbowasm_cflow_finalize(void *user) {
    turbowasm_cflow_call *call = (turbowasm_cflow_call *)user;
    if (call->finalize != NULL)
        call->finalize(call->user);
}

cflow_admission_status turbowasm_cflow_try_submit(
    cflow_executor *executor,
    turbowasm_cflow_call *call) {
    cflow_executor_task task = {0};

    if (executor == NULL || !turbowasm_cflow_call_valid(call))
        return CFLOW_ADMISSION_INVALID_ARGUMENT;

    task.run = turbowasm_cflow_run;
    task.cancel = turbowasm_cflow_cancel;
    task.finalize = turbowasm_cflow_finalize;
    task.user = call;

    return cflow_executor_try_post_task(executor, &task);
}
