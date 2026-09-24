#include <turbowasm/cflow.h>
#include <turbowasm/turbowasm.h>

#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct completion_probe {
    int completed;
    turbowasm_status status;
    int32_t value;
} completion_probe;

static void complete(
    void *user,
    const turbowasm_cflow_completion *completion) {
    completion_probe *probe = (completion_probe *)user;
    probe->completed = 1;
    probe->status = completion->status;
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x07, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    cflow_executor executor = {0};
    completion_probe probe = {0};
    turbowasm_value result = {0};
    turbowasm_cflow_call call = {0};

    if (turbowasm_module_load_borrowed(
            &module, bytes, sizeof(bytes)) != TURBOWASM_OK)
        return 1;
    if (turbowasm_instance_create(
            &instance, &module) != TURBOWASM_OK)
        return 2;

    {
        cflow_clock clock = {0};
        turbowasm_cflow_deadline deadline = {0};
        turbowasm_execution_options options = {0};

        if (!cflow_clock_virtual_init(
                &clock, (cflow_instant){100u}))
            return 8;
        if (!turbowasm_cflow_deadline_init_after(
                &deadline, &clock, cflow_duration_from_ns(1u)))
            return 9;
        if (!turbowasm_cflow_execution_options_set_deadline(
                &options, &deadline))
            return 10;
        if (turbowasm_cflow_deadline_should_interrupt(
                &deadline))
            return 11;
        if (!cflow_clock_advance(
                &clock, cflow_duration_from_ns(1u)))
            return 12;
        if (!turbowasm_cflow_deadline_should_interrupt(
                &deadline))
            return 13;
        cflow_clock_destroy(&clock);
    }
    if (!cflow_executor_manual_init_with_capacity(&executor, 1u))
        return 3;

    call.instance = &instance;
    call.function_index = 0u;
    call.results = &result;
    call.result_capacity = 1u;
    call.complete = complete;
    call.user = &probe;

    if (turbowasm_cflow_try_submit(
            &executor, &call) != CFLOW_ADMISSION_ACCEPTED)
        return 4;
    if (cflow_executor_run_ready(&executor) != 1u)
        return 5;
    if (!probe.completed || probe.status != TURBOWASM_OK)
        return 6;
    if (result.kind != TURBOWASM_VALUE_I32 || result.as.i32 != 7)
        return 7;

    cflow_executor_shutdown(&executor);
    cflow_executor_destroy(&executor);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
