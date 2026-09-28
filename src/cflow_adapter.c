#include <turbowasm/cflow.h>

static bool turbowasm_cflow_deadline_should_interrupt(void *context) {
    turbowasm_cflow_deadline_policy *policy =
        (turbowasm_cflow_deadline_policy *)context;
    cflow_instant now;

    if (policy == NULL || policy->clock == NULL)
        return true;

    now = cflow_clock_now(policy->clock);
    if (now.ns >= policy->deadline.ns)
        return true;

    return policy->chained_interrupt != NULL &&
           policy->chained_interrupt(policy->chained_context);
}

bool turbowasm_cflow_deadline_init_at(
    turbowasm_cflow_deadline_policy *policy,
    cflow_clock *clock,
    cflow_deadline deadline) {
    if (policy == NULL || clock == NULL)
        return false;

    policy->clock = clock;
    policy->deadline = deadline;
    policy->chained_interrupt = NULL;
    policy->chained_context = NULL;
    return true;
}

bool turbowasm_cflow_deadline_init_after(
    turbowasm_cflow_deadline_policy *policy,
    cflow_clock *clock,
    cflow_duration delay) {
    cflow_instant now;

    if (policy == NULL || clock == NULL)
        return false;

    now = cflow_clock_now(clock);
    return turbowasm_cflow_deadline_init_at(
        policy, clock, cflow_deadline_after(now, delay));
}

bool turbowasm_cflow_deadline_apply(
    turbowasm_cflow_deadline_policy *policy,
    turbowasm_execution_options *options) {
    if (policy == NULL || policy->clock == NULL || options == NULL)
        return false;

    if (options->should_interrupt !=
            turbowasm_cflow_deadline_should_interrupt ||
        options->interrupt_context != policy) {
        policy->chained_interrupt = options->should_interrupt;
        policy->chained_context = options->interrupt_context;
    }

    options->should_interrupt =
        turbowasm_cflow_deadline_should_interrupt;
    options->interrupt_context = policy;
    return true;
}


static bool turbowasm_cflow_call_valid(
    const turbowasm_cflow_call *call) {
    const turbowasm_module *module;
    turbowasm_function_signature signature;
    size_t index;

    if (call == NULL || call->instance == NULL)
        return false;
    if (call->argument_count != 0u && call->arguments == NULL)
        return false;
    if (call->result_capacity != 0u && call->results == NULL)
        return false;

    module = turbowasm_instance_module(call->instance);
    if (module == NULL ||
        !turbowasm_module_function_signature_get(
            module, call->function_index, &signature))
        return false;
    if (call->argument_count != signature.param_count)
        return false;

    for (index = 0u; index < call->argument_count; ++index) {
        const cmeta_type_desc *expected =
            turbowasm_module_function_param_type(
                module, call->function_index, (uint32_t)index);
        const cmeta_type_desc *actual =
            turbowasm_value_type_descriptor(call->arguments[index].kind);

        if (expected == NULL || actual == NULL ||
            (expected != actual && !cmeta_type_equal(expected, actual)))
            return false;
    }

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
