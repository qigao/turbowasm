#include "clock_host.h"

static turbowasm_status now_ns(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t result_capacity, size_t *result_count,
    turbowasm_trap *trap) {
    app_clock *clock = context;
    (void)arguments;
    if (!clock || !clock->read_ns || !call || argument_count || !results ||
        !result_capacity || !result_count || !trap)
        return TURBOWASM_INVALID_ARGUMENT;

    uint64_t value = 0;
    bool available = clock->read_ns(clock->context, &value);
    results[0].kind = TURBOWASM_VALUE_I64;
    results[0].as.i64 = available && value <= INT64_MAX ? (int64_t)value : -1;
    *result_count = 1;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

turbowasm_status app_clock_define(turbowasm_linker *linker, app_clock *clock) {
    static const turbowasm_value_kind result = TURBOWASM_VALUE_I64;
    const turbowasm_host_function_type type = {NULL, 0, &result, 1};
    static const uint8_t module[] = "app:clock/v1";
    static const uint8_t function[] = "now_ns";
    if (!clock || !clock->read_ns) return TURBOWASM_INVALID_ARGUMENT;
    return turbowasm_linker_define_host_function(linker,
        (turbowasm_name){module, sizeof(module) - 1},
        (turbowasm_name){function, sizeof(function) - 1}, &type, now_ns, clock);
}
