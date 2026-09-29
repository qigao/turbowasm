#include <turbowasm/wasi_threads.h>

#include <cflow/executor.h>

static int qualify_public_symbols(void) {
    turbowasm_wasi_threads_execution_policy policy = {0};
    turbowasm_execution_options options = {0};
    uint32_t exit_code = 0u;
    turbowasm_status status = TURBOWASM_OK;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    if (turbowasm_wasi_threads_execution_policy_init(
            &policy, NULL))
        return 1;
    if (turbowasm_wasi_threads_execution_policy_apply(
            &policy, &options))
        return 2;
    if (turbowasm_wasi_threads_init(NULL, NULL) !=
        TURBOWASM_INVALID_ARGUMENT)
        return 3;
    if (turbowasm_wasi_threads_destroy(NULL))
        return 4;
    if (turbowasm_wasi_threads_define(NULL, NULL) !=
        TURBOWASM_INVALID_ARGUMENT)
        return 5;
    if (turbowasm_wasi_threads_active(NULL) != 0u)
        return 6;
    if (turbowasm_wasi_threads_group_fatal(
            NULL, &status, &trap))
        return 7;

    turbowasm_wasi_threads_proc_exit(NULL, NULL, 0u);
    if (turbowasm_wasi_threads_group_exit_code(
            NULL, &exit_code))
        return 8;
    if (turbowasm_wasi_threads_group_exit(
            NULL, &exit_code))
        return 9;
    return 0;
}

int main(void) {
    cflow_executor executor = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    turbowasm_linker linker = {0};
    int qualification = qualify_public_symbols();

    if (qualification != 0)
        return 20 + qualification;

    if (!cflow_executor_worker_init_with_capacity(
            &executor, 1u, 2u))
        return 1;

    config.executor = &executor;
    config.capacity = 1u;
    if (turbowasm_wasi_threads_init(
            &threads, &config) != TURBOWASM_OK)
        return 2;

    if (turbowasm_linker_init(&linker) != TURBOWASM_OK)
        return 3;
    if (turbowasm_wasi_threads_define(
            &threads, &linker) != TURBOWASM_OK)
        return 4;

    turbowasm_linker_destroy(&linker);
    if (!turbowasm_wasi_threads_destroy(&threads))
        return 5;
    if (!cflow_executor_shutdown(&executor))
        return 6;
    cflow_executor_destroy(&executor);
    return 0;
}
