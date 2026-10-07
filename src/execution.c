#include <turbowasm/execution.h>

#include "instance_internal.h"
#include "module_internal.h"
#include "runtime_alloc.h"

#include <coro.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_RESUMABLE_STACK_SIZE = 512u * 1024u
};

typedef struct turbowasm_execution_impl {
    turbowasm_instance_impl *instance;
    uint32_t function_index;

    turbowasm_value *arguments;
    size_t argument_count;

    turbowasm_value *results;
    size_t result_capacity;
    size_t result_count;

    turbowasm_trap trap;
    turbowasm_status terminal_status;
    turbowasm_execution_state state;
    turbowasm_yield_reason yield_reason;

    turbowasm_jit_execution_control control;
    coro_t *coroutine;

    uint64_t next_host_wait_generation;
    turbowasm_host_wait host_wait;
    bool host_wait_active;
    bool host_wait_completed;
    int host_wait_status;
} turbowasm_execution_impl;

static turbowasm_execution_impl *turbowasm_execution_impl_mut(
    turbowasm_execution *execution) {
    return execution == NULL
        ? NULL
        : (turbowasm_execution_impl *)execution->impl;
}

static const turbowasm_execution_impl *turbowasm_execution_impl_get(
    const turbowasm_execution *execution) {
    return execution == NULL
        ? NULL
        : (const turbowasm_execution_impl *)execution->impl;
}

static turbowasm_status turbowasm_execution_suspend(
    void *context,
    turbowasm_status reason) {
    turbowasm_execution_impl *execution =
        (turbowasm_execution_impl *)context;

    if (execution == NULL ||
        (reason != TURBOWASM_FUEL_EXHAUSTED &&
         reason != TURBOWASM_INTERRUPTED))
        return TURBOWASM_INVALID_ARGUMENT;

    execution->yield_reason =
        reason == TURBOWASM_FUEL_EXHAUSTED
            ? TURBOWASM_YIELD_FUEL
            : TURBOWASM_YIELD_INTERRUPTION;
    execution->state = TURBOWASM_EXECUTION_YIELDED;

    if (coro_yield() != 0) {
        execution->state = TURBOWASM_EXECUTION_FAILED;
        execution->terminal_status = TURBOWASM_INVALID_ARGUMENT;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    execution->yield_reason = TURBOWASM_YIELD_NONE;
    execution->state = TURBOWASM_EXECUTION_RUNNING;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_execution_host_wait(
    void *context,
    uintptr_t operation_token,
    turbowasm_host_wait *out_wait,
    int *out_status) {
    turbowasm_execution_impl *execution =
        (turbowasm_execution_impl *)context;
    uint64_t generation;

    if (execution == NULL || out_wait == NULL || out_status == NULL ||
        execution->host_wait_active)
        return TURBOWASM_INVALID_ARGUMENT;

    generation = execution->next_host_wait_generation + 1u;
    if (generation == 0u)
        generation = 1u;
    execution->next_host_wait_generation = generation;

    execution->host_wait.generation = generation;
    execution->host_wait.operation_token = operation_token;
    execution->host_wait_active = true;
    execution->host_wait_completed = false;
    execution->host_wait_status = 0;
    *out_wait = execution->host_wait;
    execution->yield_reason = TURBOWASM_YIELD_HOST_WAIT;
    execution->state = TURBOWASM_EXECUTION_YIELDED;

    if (coro_yield() != 0) {
        execution->host_wait_active = false;
        execution->state = TURBOWASM_EXECUTION_FAILED;
        execution->terminal_status = TURBOWASM_INVALID_ARGUMENT;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    if (!execution->host_wait_active ||
        !execution->host_wait_completed) {
        execution->state = TURBOWASM_EXECUTION_FAILED;
        execution->terminal_status = TURBOWASM_INVALID_ARGUMENT;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    *out_status = execution->host_wait_status;
    execution->host_wait_active = false;
    execution->host_wait_completed = false;
    execution->host_wait = (turbowasm_host_wait){0};
    execution->host_wait_status = 0;
    execution->yield_reason = TURBOWASM_YIELD_NONE;
    execution->state = TURBOWASM_EXECUTION_RUNNING;
    return TURBOWASM_OK;
}

static turbowasm_status turbowasm_execution_host_wait_complete(
    void *context,
    turbowasm_host_wait wait,
    int status) {
    turbowasm_execution_impl *impl =
        (turbowasm_execution_impl *)context;

    if (impl == NULL ||
        !impl->host_wait_active ||
        impl->host_wait_completed ||
        wait.generation == 0u ||
        wait.generation != impl->host_wait.generation ||
        wait.operation_token != impl->host_wait.operation_token)
        return TURBOWASM_INVALID_ARGUMENT;

    impl->host_wait_status = status;
    impl->host_wait_completed = true;
    return TURBOWASM_OK;
}

static void turbowasm_execution_entry(coro_t *co, void *arg) {
    turbowasm_execution_impl *execution =
        (turbowasm_execution_impl *)arg;
    turbowasm_status status;

    (void)co;
    if (execution == NULL || execution->instance == NULL)
        return;

    execution->instance->pending_exception = NULL;
    status = turbowasm_instance_invoke_interpreter_internal(
        execution->instance,
        execution->function_index,
        execution->arguments,
        execution->argument_count,
        execution->results,
        execution->result_capacity,
        &execution->result_count,
        &execution->trap,
        &execution->control);

    execution->terminal_status = status;
    execution->yield_reason = TURBOWASM_YIELD_NONE;

    switch (status) {
        case TURBOWASM_OK:
            execution->state = TURBOWASM_EXECUTION_COMPLETED;
            break;
        case TURBOWASM_TRAPPED:
            execution->state = TURBOWASM_EXECUTION_TRAPPED;
            break;
        case TURBOWASM_EXCEPTION:
            execution->state = TURBOWASM_EXECUTION_EXCEPTION;
            break;
        default:
            execution->state = TURBOWASM_EXECUTION_FAILED;
            break;
    }
}

turbowasm_status turbowasm_execution_create(
    turbowasm_execution *execution,
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_instance_impl *instance_impl;
    const turbowasm_module_impl *module;
    const turbowasm_validation_func_type *type;
    turbowasm_execution_impl *impl;
    coro_opts_t opts = coro_OPTS_DEFAULT;

    if (execution == NULL || execution->impl != NULL ||
        instance == NULL || instance->impl == NULL ||
        (argument_count != 0u && arguments == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    instance_impl = (turbowasm_instance_impl *)instance->impl;
    module = turbowasm_module_impl_get(instance_impl->module);
    if (module == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    type = turbowasm_validation_context_function_type(
        &module->validation, function_index);
    if (type == NULL || !type->defined ||
        argument_count != type->param_count)
        return TURBOWASM_INVALID_ARGUMENT;

    if (argument_count > SIZE_MAX / sizeof(*arguments) ||
        type->result_count > SIZE_MAX / sizeof(turbowasm_value))
        return TURBOWASM_OUT_OF_MEMORY;

    {
        turbowasm_runtime_scope scope =
            turbowasm_runtime_scope_enter(&module->config);

        impl = (turbowasm_execution_impl *)turbowasm_rt_calloc(
            1u, sizeof(*impl));
        if (impl == NULL) {
            turbowasm_runtime_scope_leave(scope);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        if (argument_count != 0u) {
        impl->arguments = (turbowasm_value *)turbowasm_rt_malloc(
            argument_count * sizeof(*impl->arguments));
        if (impl->arguments == NULL)
            goto out_of_memory;
        memcpy(
            impl->arguments,
            arguments,
            argument_count * sizeof(*impl->arguments));
    }

    impl->result_capacity = type->result_count;
    if (impl->result_capacity != 0u) {
        impl->results = (turbowasm_value *)turbowasm_rt_calloc(
            impl->result_capacity, sizeof(*impl->results));
        if (impl->results == NULL)
            goto out_of_memory;
    }

    impl->instance = instance_impl;
    impl->function_index = function_index;
    impl->argument_count = argument_count;
    impl->trap = TURBOWASM_TRAP_NONE;
    impl->terminal_status = TURBOWASM_OK;
    impl->state = TURBOWASM_EXECUTION_READY;
    impl->yield_reason = TURBOWASM_YIELD_NONE;
    impl->control.suspend = turbowasm_execution_suspend;
    impl->control.suspend_context = impl;
    impl->control.host_wait = turbowasm_execution_host_wait;
    impl->control.host_wait_complete =
        turbowasm_execution_host_wait_complete;
    impl->control.host_wait_context = impl;

    opts.stack_size = TURBOWASM_RESUMABLE_STACK_SIZE;
    impl->coroutine = coro_create(
        turbowasm_execution_entry, impl, &opts);
    if (impl->coroutine == NULL)
        goto out_of_memory;

        execution->impl = impl;
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_OK;

out_of_memory:
        turbowasm_rt_free(impl->results);
        turbowasm_rt_free(impl->arguments);
        turbowasm_rt_free(impl);
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_OUT_OF_MEMORY;
    }
}

void turbowasm_execution_destroy(turbowasm_execution *execution) {
    turbowasm_execution_impl *impl =
        turbowasm_execution_impl_mut(execution);

    if (impl == NULL)
        return;

    if (impl->coroutine != NULL)
        coro_destroy(impl->coroutine);
    turbowasm_rt_free(impl->results);
    turbowasm_rt_free(impl->arguments);
    turbowasm_rt_free(impl);
    execution->impl = NULL;
}

turbowasm_status turbowasm_execution_resume(
    turbowasm_execution *execution,
    const turbowasm_execution_options *options) {
    turbowasm_execution_impl *impl =
        turbowasm_execution_impl_mut(execution);

    if (impl == NULL ||
        (impl->state != TURBOWASM_EXECUTION_READY &&
         impl->state != TURBOWASM_EXECUTION_YIELDED))
        return TURBOWASM_INVALID_ARGUMENT;

    if (impl->state == TURBOWASM_EXECUTION_YIELDED &&
        impl->yield_reason == TURBOWASM_YIELD_HOST_WAIT &&
        impl->host_wait_active &&
        !impl->host_wait_completed)
        return TURBOWASM_YIELDED;

    impl->control.fuel_remaining =
        options != NULL ? options->fuel : 0u;
    impl->control.fuel_limited =
        options != NULL && options->has_fuel_limit;
    impl->control.should_interrupt =
        options != NULL ? options->should_interrupt : NULL;
    impl->control.interrupt_context =
        options != NULL ? options->interrupt_context : NULL;

    impl->yield_reason = TURBOWASM_YIELD_NONE;
    impl->state = TURBOWASM_EXECUTION_RUNNING;

    {
        const turbowasm_module_impl *module =
            turbowasm_module_impl_get(impl->instance->module);
        turbowasm_runtime_scope scope;
        int resume_status;

        if (module == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        scope = turbowasm_runtime_scope_enter(&module->config);
        resume_status = coro_resume(impl->coroutine);
        turbowasm_runtime_scope_leave(scope);
        if (resume_status != 0) {
            impl->state = TURBOWASM_EXECUTION_FAILED;
            impl->terminal_status = TURBOWASM_INVALID_ARGUMENT;
            return TURBOWASM_INVALID_ARGUMENT;
        }
    }

    if (impl->state == TURBOWASM_EXECUTION_YIELDED)
        return TURBOWASM_YIELDED;

    if (coro_state(impl->coroutine) != coro_DEAD ||
        impl->state == TURBOWASM_EXECUTION_RUNNING) {
        impl->state = TURBOWASM_EXECUTION_FAILED;
        impl->terminal_status = TURBOWASM_INVALID_ARGUMENT;
        return TURBOWASM_INVALID_ARGUMENT;
    }

    return impl->terminal_status;
}

turbowasm_execution_state turbowasm_execution_state_get(
    const turbowasm_execution *execution) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);
    return impl == NULL
        ? TURBOWASM_EXECUTION_FAILED
        : impl->state;
}

turbowasm_yield_reason turbowasm_execution_yield_reason_get(
    const turbowasm_execution *execution) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);
    return impl == NULL
        ? TURBOWASM_YIELD_NONE
        : impl->yield_reason;
}

bool turbowasm_execution_pending_host_wait(
    const turbowasm_execution *execution,
    turbowasm_host_wait *out_wait) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);

    if (impl == NULL || out_wait == NULL ||
        impl->state != TURBOWASM_EXECUTION_YIELDED ||
        impl->yield_reason != TURBOWASM_YIELD_HOST_WAIT ||
        !impl->host_wait_active)
        return false;

    *out_wait = impl->host_wait;
    return true;
}

turbowasm_status turbowasm_execution_complete_host_wait(
    turbowasm_execution *execution,
    turbowasm_host_wait wait,
    int status) {
    turbowasm_execution_impl *impl =
        turbowasm_execution_impl_mut(execution);

    if (impl == NULL ||
        impl->state != TURBOWASM_EXECUTION_YIELDED ||
        impl->yield_reason != TURBOWASM_YIELD_HOST_WAIT)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_execution_host_wait_complete(
        impl, wait, status);
}

turbowasm_status turbowasm_execution_terminal_status(
    const turbowasm_execution *execution) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);
    return impl == NULL
        ? TURBOWASM_INVALID_ARGUMENT
        : impl->terminal_status;
}

turbowasm_trap turbowasm_execution_trap(
    const turbowasm_execution *execution) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);
    return impl == NULL
        ? TURBOWASM_TRAP_NONE
        : impl->trap;
}

size_t turbowasm_execution_result_count(
    const turbowasm_execution *execution) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);
    return impl == NULL ? 0u : impl->result_count;
}

const turbowasm_value *turbowasm_execution_result_at(
    const turbowasm_execution *execution,
    size_t index) {
    const turbowasm_execution_impl *impl =
        turbowasm_execution_impl_get(execution);

    if (impl == NULL ||
        impl->state != TURBOWASM_EXECUTION_COMPLETED ||
        index >= impl->result_count)
        return NULL;
    return &impl->results[index];
}
