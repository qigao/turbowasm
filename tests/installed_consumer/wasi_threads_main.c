#include <turbowasm/wasi_threads.h>

#include <cflow/executor.h>

int main(void) {
    cflow_executor executor = {0};
    turbowasm_wasi_threads threads = {0};
    turbowasm_wasi_threads_config config = {0};
    turbowasm_linker linker = {0};

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
